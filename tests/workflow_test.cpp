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

#include <atomic>
#include <deque>
#include <string>
#include <vector>

namespace {

struct RecordingAdapter final : demo::PresentationAdapter {
  std::vector<std::string> stages;
  std::deque<demo::Done> pending;
  bool failDrain = false;

  void probe(tc::DeviceInfo const &, std::function<void(demo::Result<demo::ProbeReport>)>) override {}
  void preparePath(demo::PresentationPath, std::shared_ptr<demo::NativeHostLease>, demo::Done) override {}
  demo::TargetDesc describeTarget(demo::ViewportExtent const &) const override {
    return lr::render::ImageTargetDesc{};
  }
  void setFrameContext(demo::SessionGeneration, demo::ViewportExtent const &) override {}
  void consumeImage(demo::OwnedFrame) override {}
  void beginDrain(demo::Done done) override {
    stages.push_back("drain");
    if (failDrain) {
      done(demo::RenderError{demo::ErrorStage::Shutdown, true, "drain failed", "injected"});
      return;
    }
    pending.push_back(std::move(done));
  }
  void releasePath(demo::Done done) override {
    stages.push_back("release");
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
void spawnSafe(Sndr &&sndr, lexec::counting_scope::token token, std::atomic_bool &finished,
               std::string &outcome) {
  lexec::spawn(std::forward<Sndr>(sndr) | lexec::upon_error([&](std::exception_ptr) noexcept {
                 outcome = "dispatch-error";
                 finished = true;
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

} // namespace

TEST_CASE("workflow/drain-then-release-order-on-gui") {
  REQUIRE(QCoreApplication::instance());
  lqtexec::EventLoopContext ctx;
  QObject host;
  lexec::counting_scope root;
  RecordingAdapter adapter;
  std::atomic_bool finished{false};
  std::string outcome;
  {
    lqtexec::ObjectScope scope{&host, ctx.get_scheduler(), root.get_token()};
    auto chain = demo::workflows::beginDrainSender(adapter) |
                 lqtexec::then_on(scope, [&](demo::Result<std::monostate> drainResult) {
                   if (auto *e = std::get_if<demo::RenderError>(&drainResult)) {
                     outcome = e->message;
                     finished = true;
                     return;
                   }
                   auto release = demo::workflows::releasePathSender(adapter) |
                                  lqtexec::then_on(scope, [&](demo::Result<std::monostate> releaseResult) {
                                    if (auto *e = std::get_if<demo::RenderError>(&releaseResult))
                                      outcome = e->message;
                                    else
                                      outcome = "ok";
                                    finished = true;
                                  });
                   spawnSafe(std::move(release), root.get_token(), finished, outcome);
                 });
    spawnSafe(std::move(chain), root.get_token(), finished, outcome);

    REQUIRE(adapter.stages.size() == 1);
    CHECK(adapter.stages[0] == "drain");
    adapter.completeNext();
    for (int i = 0; i < 50 && adapter.stages.size() < 2; ++i)
      pumpOnce();
    REQUIRE(adapter.stages.size() == 2);
    CHECK(adapter.stages[1] == "release");
    adapter.completeNext();
    REQUIRE(waitFlag(finished));
    CHECK(outcome == "ok");
    CHECK(adapter.pending.empty());
  }
  root.close();
  std::atomic_bool joined{false};
  auto op = lexec::connect(root.join(), Joined{&joined});
  lexec::start(op);
  REQUIRE(waitFlag(joined));
}

TEST_CASE("workflow/drain-error-skips-release") {
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
    auto chain = demo::workflows::beginDrainSender(adapter) |
                 lqtexec::then_on(scope, [&](demo::Result<std::monostate> drainResult) {
                   if (auto *e = std::get_if<demo::RenderError>(&drainResult))
                     outcome = e->cause;
                   else
                     outcome = "ok";
                   finished = true;
                 });
    spawnSafe(std::move(chain), root.get_token(), finished, outcome);
    REQUIRE(waitFlag(finished));
    CHECK(outcome == "injected");
    CHECK(adapter.stages == std::vector<std::string>{"drain"});
  }
  root.close();
  std::atomic_bool joined{false};
  auto op = lexec::connect(root.join(), Joined{&joined});
  lexec::start(op);
  REQUIRE(waitFlag(joined));
}

TEST_CASE("workflow/soft-drain-only-never-releases") {
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
                 lqtexec::then_on(scope, [&](demo::Result<std::monostate>) { finished = true; });
    spawnSafe(std::move(chain), root.get_token(), finished, outcome);
    REQUIRE(adapter.stages.size() == 1);
    CHECK(adapter.stages[0] == "drain");
    adapter.completeNext();
    REQUIRE(waitFlag(finished));
    CHECK(adapter.stages == std::vector<std::string>{"drain"});
    CHECK(adapter.pending.empty());
  }
  root.close();
  std::atomic_bool joined{false};
  auto op = lexec::connect(root.join(), Joined{&joined});
  lexec::start(op);
  REQUIRE(waitFlag(joined));
}
