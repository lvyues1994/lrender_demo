#include <doctest/doctest.h>

#include "render/workflows/AdapterSenders.h"
#include "render/workflows/SessionWorkflows.h"

#include <lqtexec/EventLoopContext.h>
#include <lqtexec/ObjectScope.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

#include <lexec/execution.hpp>
#include <lexec/scopes/counting_scope.hpp>
#include <lexec/stop_token.hpp>

#include <atomic>
#include <deque>
#include <string>
#include <vector>

namespace {

struct RecordingAdapter final : demo::PresentationAdapter {
  std::vector<std::string> stages;
  std::deque<demo::Done> pending;
  bool failDrain = false;
  bool failRelease = false;
  bool skipDone = false; // negative: must not be used in production paths

  void probe(tc::DeviceInfo const &, std::function<void(demo::Result<demo::ProbeReport>)>) override {}
  void preparePath(demo::PresentationPath, std::shared_ptr<demo::NativeHostLease>, demo::Done done) override {
    stages.push_back("prepare");
    pending.push_back(std::move(done));
  }
  demo::TargetDesc describeTarget(demo::ViewportExtent const &) const override {
    return lr::render::ImageTargetDesc{};
  }
  void setFrameContext(demo::SessionGeneration, demo::ViewportExtent const &) override {}
  void consumeImage(demo::OwnedFrame) override {}
  void beginDrain(demo::Done done) override {
    stages.push_back("drain");
    if (skipDone)
      return;
    if (failDrain) {
      done(demo::RenderError{demo::ErrorStage::Shutdown, true, "drain failed", "injected"});
      return;
    }
    pending.push_back(std::move(done));
  }
  void releasePath(demo::Done done) override {
    stages.push_back("release");
    if (skipDone)
      return;
    if (failRelease) {
      done(demo::RenderError{demo::ErrorStage::Shutdown, true, "release failed", "injected"});
      return;
    }
    pending.push_back(std::move(done));
  }
  void completeNext(demo::Result<std::monostate> r = std::monostate{}) {
    REQUIRE_FALSE(pending.empty());
    auto d = std::move(pending.front());
    pending.pop_front();
    d(std::move(r));
  }
};

void pumpOnce() {
  QEventLoop loop;
  QTimer::singleShot(0, &loop, &QEventLoop::quit);
  loop.exec();
}

bool waitFlag(std::atomic_bool &done, int ms = 2000) {
  if (done.load())
    return true;
  QEventLoop loop;
  QTimer deadline, poll;
  deadline.setSingleShot(true);
  poll.setInterval(5);
  QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (done.load())
      loop.quit();
  });
  deadline.start(ms);
  poll.start();
  loop.exec();
  return done.load();
}

template <class Sndr>
void spawnSafe(Sndr &&sndr, lexec::counting_scope::token token, std::atomic_bool *fault = nullptr) {
  lexec::spawn(std::forward<Sndr>(sndr) | lexec::upon_error([fault](std::exception_ptr) noexcept {
                 if (fault)
                   fault->store(true);
               }),
               token);
}

struct Joined {
  using receiver_concept = lexec::receiver_t;
  std::atomic_bool *flag = nullptr;
  void set_value() && noexcept { flag->store(true); }
  void set_error(std::exception_ptr &&) && noexcept { flag->store(true); }
  void set_stopped() && noexcept { flag->store(true); }
};

void joinRoot(lexec::counting_scope &root) {
  root.close();
  std::atomic_bool joined{false};
  auto op = lexec::connect(root.join(), Joined{&joined});
  lexec::start(op);
  REQUIRE(waitFlag(joined));
}

} // namespace

TEST_CASE("workflow/soft-drain-then-on-gui-never-releases") {
  REQUIRE(QCoreApplication::instance());
  lqtexec::EventLoopContext ctx;
  QObject host;
  lexec::counting_scope root;
  RecordingAdapter adapter;
  std::atomic_bool finished{false};
  std::string outcome;
  {
    lqtexec::ObjectScope scope{&host, ctx.get_scheduler(), root.get_token()};
    auto chain = demo::workflows::softDrainOnly(adapter) |
                 lqtexec::then_on(scope, [&](demo::Result<std::monostate> r) {
                   if (std::holds_alternative<demo::RenderError>(r))
                     outcome = "err";
                   else
                     outcome = "ok";
                   finished = true;
                 });
    spawnSafe(std::move(chain), root.get_token());
    REQUIRE(adapter.stages.size() == 1);
    CHECK(adapter.stages[0] == "drain");
    adapter.completeNext();
    REQUIRE(waitFlag(finished));
    CHECK(outcome == "ok");
    CHECK(adapter.stages == std::vector<std::string>{"drain"});
  }
  joinRoot(root);
}

TEST_CASE("workflow/hard-drain-destroy-release-order") {
  REQUIRE(QCoreApplication::instance());
  lqtexec::EventLoopContext ctx;
  QObject host;
  lexec::counting_scope root;
  RecordingAdapter adapter;
  std::atomic_bool finished{false};
  std::vector<std::string> gui;
  {
    lqtexec::ObjectScope scope{&host, ctx.get_scheduler(), root.get_token()};
    auto chain = demo::workflows::drainThenOnGui(adapter, scope, [&](demo::Result<std::monostate> drainResult) {
      if (std::holds_alternative<demo::RenderError>(drainResult)) {
        gui.push_back("drain-err");
        finished = true;
        return;
      }
      gui.push_back("destroyed"); // stand-in for destroyTarget on GUI
      auto release = demo::workflows::releaseThenOnGui(adapter, scope, [&](demo::Result<std::monostate> releaseResult) {
        if (std::holds_alternative<demo::RenderError>(releaseResult))
          gui.push_back("release-err");
        else
          gui.push_back("created"); // stand-in for create/prepare resume
        finished = true;
      });
      spawnSafe(std::move(release), root.get_token());
    });
    spawnSafe(std::move(chain), root.get_token());
    REQUIRE(adapter.stages.size() == 1);
    CHECK(adapter.stages[0] == "drain");
    adapter.completeNext();
    for (int i = 0; i < 50 && adapter.stages.size() < 2; ++i)
      pumpOnce();
    REQUIRE(adapter.stages.size() == 2);
    CHECK(adapter.stages[1] == "release");
    adapter.completeNext();
    REQUIRE(waitFlag(finished));
    CHECK(gui == std::vector<std::string>{"destroyed", "created"});
  }
  joinRoot(root);
}

TEST_CASE("workflow/drain-error-skips-release-and-converges") {
  REQUIRE(QCoreApplication::instance());
  lqtexec::EventLoopContext ctx;
  QObject host;
  lexec::counting_scope root;
  RecordingAdapter adapter;
  adapter.failDrain = true;
  std::atomic_bool finished{false};
  std::string outcome;
  {
    lqtexec::ObjectScope scope{&host, ctx.get_scheduler(), root.get_token()};
    auto chain = demo::workflows::drainThenOnGui(adapter, scope, [&](demo::Result<std::monostate> drainResult) {
      auto *e = std::get_if<demo::RenderError>(&drainResult);
      REQUIRE(e);
      outcome = e->cause;
      finished = true;
    });
    spawnSafe(std::move(chain), root.get_token());
    REQUIRE(waitFlag(finished));
    CHECK(outcome == "injected");
    CHECK(adapter.stages == std::vector<std::string>{"drain"});
  }
  joinRoot(root);
}

TEST_CASE("workflow/release-error-converges") {
  REQUIRE(QCoreApplication::instance());
  lqtexec::EventLoopContext ctx;
  QObject host;
  lexec::counting_scope root;
  RecordingAdapter adapter;
  adapter.failRelease = true;
  std::atomic_bool finished{false};
  std::string outcome;
  {
    lqtexec::ObjectScope scope{&host, ctx.get_scheduler(), root.get_token()};
    auto chain = demo::workflows::drainThenOnGui(adapter, scope, [&](demo::Result<std::monostate>) {
      auto release = demo::workflows::releaseThenOnGui(adapter, scope, [&](demo::Result<std::monostate> r) {
        auto *e = std::get_if<demo::RenderError>(&r);
        REQUIRE(e);
        outcome = e->cause;
        finished = true;
      });
      spawnSafe(std::move(release), root.get_token());
    });
    spawnSafe(std::move(chain), root.get_token());
    adapter.completeNext(); // drain ok
    for (int i = 0; i < 50 && !finished.load(); ++i)
      pumpOnce();
    REQUIRE(waitFlag(finished));
    CHECK(outcome == "injected");
    CHECK(adapter.stages == std::vector<std::string>{"drain", "release"});
  }
  joinRoot(root);
}

TEST_CASE("workflow/done-op-stop-token-still-delivers-value") {
  REQUIRE(QCoreApplication::instance());
  lqtexec::EventLoopContext ctx;
  QObject host;
  lexec::counting_scope root;
  RecordingAdapter adapter;
  std::atomic_bool finished{false};
  std::string outcome;
  lexec::inplace_stop_source stop;
  {
    lqtexec::ObjectScope scope{&host, ctx.get_scheduler(), root.get_token()};
    stop.request_stop();
    auto chain = lexec::write_env(demo::workflows::beginDrainSender(adapter),
                                  lexec::prop{lexec::get_stop_token, stop.get_token()}) |
                 lqtexec::then_on(scope, [&](demo::Result<std::monostate> r) {
                   auto *e = std::get_if<demo::RenderError>(&r);
                   REQUIRE(e);
                   outcome = e->cause;
                   finished = true;
                 });
    spawnSafe(std::move(chain), root.get_token());
    REQUIRE(adapter.stages.size() == 1);
    adapter.completeNext(std::monostate{}); // Done fires after stop requested
    REQUIRE(waitFlag(finished));
    CHECK(outcome == "stop token requested");
  }
  joinRoot(root);
}

TEST_CASE("workflow/phase-early-return-drops-late-done") {
  // Mirrors production `if (phase != Draining) return;` after a superseded rebuild.
  REQUIRE(QCoreApplication::instance());
  lqtexec::EventLoopContext ctx;
  QObject host;
  lexec::counting_scope root;
  RecordingAdapter adapter;
  std::atomic_bool finished{false};
  int applied = 0;
  std::atomic_bool phaseDraining{true};
  {
    lqtexec::ObjectScope scope{&host, ctx.get_scheduler(), root.get_token()};
    auto chain = demo::workflows::beginDrainSender(adapter) |
                 lqtexec::then_on(scope, [&](demo::Result<std::monostate>) {
                   if (!phaseDraining.load())
                     return; // silent early return — late Done ignored
                   ++applied;
                   finished = true;
                 });
    spawnSafe(std::move(chain), root.get_token());
    phaseDraining = false; // supersede before Done
    adapter.completeNext();
    for (int i = 0; i < 30; ++i)
      pumpOnce();
    CHECK(applied == 0);
    CHECK(finished.load() == false);
  }
  // Still must join: DoneOpSender completed the sender even if GUI fn no-op'd.
  joinRoot(root);
}
