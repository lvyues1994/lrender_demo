#include "presentation/ReadbackAdapter.h"
#include "render/RenderSession.h"
#include "support/FakePresentation.h"
#include "support/QtWait.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <lexec/execution.hpp>
#include <lr/core/Primitives.h>
using namespace demo;
#include <doctest/doctest.h>
namespace {
lexec::static_thread_pool &fixturePool() {
  static lexec::static_thread_pool pool{1};
  return pool;
}
struct Joined {
  using receiver_concept = lexec::receiver_t;
  std::atomic<bool> *done;
  void set_value() && noexcept { done->store(true); }
};
struct Resources {
  std::unique_ptr<lr::runtime::Runtime> runtime;
  std::unique_ptr<lr::render::Renderer> renderer;
  std::unique_ptr<lr::scene::World> world;
};
Resources resources() {
  Resources r;
  std::exception_ptr failure;
  lexec::simple_counting_scope scope;
  lexec::spawn(lexec::schedule(fixturePool().get_scheduler()) | lexec::then([&]() noexcept {
                 try {
                   lr::runtime::RuntimeConfig config;
                   config.framePoolThreads = 2;
                   config.backgroundPoolThreads = 1;
                   config.ioWorkers = 1;
                   r.runtime = lr::runtime::makeRuntime(config);
                   lr::render::RendererConfig rc;
                   rc.backend = "cpu";
                   rc.runtime = r.runtime.get();
                   rc.framesInFlight = 2;
                   r.renderer = lr::render::makeRenderer(rc);
                   r.world = lr::scene::makeWorld({}); // serial host propagation, no GUI wait on frame pool
                   auto mesh = std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({0.5f, 0.5f}));
                   r.world->spawnInstance(mesh, lr::core::Transform::identity());
                   r.world->setEnvironment({lr::core::vec3{0.25f}});
                 } catch (...) {
                   failure = std::current_exception();
                 }
               }),
               scope.get_token());
  scope.close();
  std::atomic<bool> joined{false};
  auto join = lexec::connect(scope.join(), Joined{&joined});
  lexec::start(join);
  if (!test::until([&] { return joined.load(); })) {
    std::cerr << "CPU fixture initialization exceeded bounded wait\n";
    std::_Exit(2);
  }
  if (failure)
    std::rethrow_exception(failure);
  return r;
}
struct Job {
  lr::render::FrameDesc frame;
  FrameStamp stamp;
  std::function<void(FrameCompletion)> done;
};
struct Harness {
  std::vector<Job> jobs;
  unsigned images = 0;
  std::vector<RenderError> errors;
  bool stopped = false;
  bool throwAfterAccept = false;
  bool throwNonStandard = false;
  std::function<SessionSnapshotPtr()> readSnapshot;
  std::vector<std::pair<SessionGeneration, unsigned>> samples;
  std::optional<OwnedFrame> lastImage;
  SubmitFrame submit() {
    return [this](auto frame, auto stamp, auto done) {
      jobs.push_back({std::move(frame), stamp, std::move(done)});
      if (throwNonStandard)
        throw 42;
      if (throwAfterAccept)
        throw std::runtime_error("ambiguous acceptance");
    };
  }
  PresentationEvents imageEvents() {
    return {[this](auto frame) {
              REQUIRE(frame.image);
              ++images;
              if (readSnapshot)
                samples.emplace_back(frame.stamp.generation, readSnapshot()->performance.accumulatedSamples);
              lastImage = std::move(frame);
            },
            [this](auto e) { errors.push_back(std::move(e)); }};
  }
  SessionEvents events() {
    return {{}, [this](auto e) { errors.push_back(std::move(e)); }, [this] { stopped = true; }};
  }
  void complete(std::size_t n, bool error = false) {
    auto &j = jobs.at(n);
    if (error) {
      j.done({j.stamp, RenderError{ErrorStage::Submit, false, "device failed", "original failure"}});
      return;
    }
    lr::render::FrameResult r;
    lr::render::ViewResult v;
    v.view = j.frame.views.at(0).id;
    v.target = j.frame.views.at(0).target;
    lr::render::ImageRgba8 image;
    image.extent = {8, 8};
    image.pixels.resize(64, 0xff0000ff);
    v.image = std::move(image);
    r.views.push_back(std::move(v));
    j.done({j.stamp, std::move(r)});
  }
};
std::unique_ptr<RenderSession> session(Harness &h, bool real = false) {
  auto r = resources();
  return std::make_unique<RenderSession>(std::move(r.runtime), std::move(r.renderer), std::move(r.world),
                                         std::make_unique<ReadbackAdapter>(h.imageEvents()),
                                         real ? SubmitFrame{} : h.submit(), h.events());
}
void start(RenderSession &s, bool continuous = true) {
  s.setViewport({{8, 8}, {8, 8}, 1});
  RenderOptions options;
  options.pipeline = continuous ? PipelineKind::Pathtrace : PipelineKind::Visibility;
  s.setRenderOptions(options);
  s.start();
}
void stop(RenderSession &s, Harness &h) {
  s.requestStop();
  REQUIRE(test::until([&] { return h.stopped; }));
  CHECK(s.snapshot()->life == LifeState::Stopped);
}
} // namespace
TEST_CASE("session/cap-real-delta-latest-camera-and-stable-view") {
  Harness h;
  auto s = session(h);
  CameraState camera;
  camera.camera.position.x = 1;
  s->setCamera(camera);
  camera.camera.position.x = 2;
  s->setCamera(camera);
  start(*s);
  REQUIRE(test::until([&] { return h.jobs.size() == 2; }));
  CHECK(s->snapshot()->performance.inFlight == 2);
  CHECK(s->snapshot()->performance.deltaExtractions == 2);
  CHECK(h.jobs[0].frame.delta.meshes.size() == 1);
  CHECK(h.jobs[1].frame.delta.meshes.empty());
  CHECK(h.jobs[1].frame.delta.instances.empty());
  CHECK(h.jobs[0].frame.views[0].camera.position.x == 2);
  CHECK(h.jobs[0].frame.views[0].id == h.jobs[1].frame.views[0].id);
  for (int i = 0; i != 1000; ++i) {
    RenderOptions options;
    options.pipeline = PipelineKind::Pathtrace;
    options.exposure = 1 + i * 0.001f;
    s->setRenderOptions(options);
  }
  CHECK(s->snapshot()->performance.deltaExtractions == 2);
  s->requestStop();
  h.complete(0);
  h.complete(1);
  stop(*s, h);
  CHECK(h.images == 0);
}
TEST_CASE("session/resize-old-completions-drain-not-display-no-resource-replay") {
  Harness h;
  auto s = session(h);
  start(*s);
  REQUIRE(test::until([&] { return h.jobs.size() == 2; }));
  auto generation = s->snapshot()->generation;
  s->setViewport({{9, 8}, {9, 8}, 1});
  h.complete(0);
  h.complete(1);
  REQUIRE(test::until([&] { return h.jobs.size() == 4; }));
  CHECK(h.images == 0);
  CHECK(s->snapshot()->generation > generation);
  CHECK(h.jobs[2].frame.delta.meshes.empty());
  CHECK(h.jobs[2].frame.views[0].id == h.jobs[0].frame.views[0].id);
  s->requestStop();
  h.complete(2);
  h.complete(3);
  stop(*s, h);
}
TEST_CASE("session/ambiguous-submit-capacity-two-fault-cannot-resume-or-reextract") {
  Harness h;
  h.throwAfterAccept = true;
  auto s = session(h);
  start(*s);
  REQUIRE(test::until([&] { return h.jobs.size() == 1 && s->snapshot()->life == LifeState::Failed; }));
  auto extracted = s->snapshot()->performance.deltaExtractions;
  s->resume();
  s->pause();
  s->resume();
  RenderOptions options;
  options.pipeline = PipelineKind::Pathtrace;
  options.exposure = 3;
  s->setRenderOptions(options);
  CHECK(h.jobs.size() == 1);
  CHECK(s->snapshot()->performance.inFlight == 1);
  CHECK(s->snapshot()->performance.deltaExtractions == extracted);
  h.complete(0);
  REQUIRE(test::until([&] { return s->snapshot()->performance.inFlight == 0; }));
  s->resume();
  CHECK(s->snapshot()->life == LifeState::Failed);
  stop(*s, h);
  CHECK(h.images == 0);
}
TEST_CASE("session/terminal-failure-duplicate-and-inline-completion-are-once") {
  Harness h;
  auto s = session(h);
  start(*s, false);
  REQUIRE(test::until([&] { return h.jobs.size() == 1; }));
  h.complete(0, true);
  h.complete(0, true);
  REQUIRE(test::until([&] { return s->snapshot()->life == LifeState::Failed; }));
  CHECK(s->snapshot()->performance.completed == 1);
  stop(*s, h);
  Harness inlineHarness;
  auto r = resources();
  unsigned calls = 0;
  SubmitFrame inlineSubmit = [&](auto, auto stamp, auto done) {
    ++calls;
    done({stamp, lr::render::FrameResult{}});
    done({stamp, lr::render::FrameResult{}});
  };
  RenderSession inlineSession(std::move(r.runtime), std::move(r.renderer), std::move(r.world),
                              std::make_unique<ReadbackAdapter>(PresentationEvents{}), inlineSubmit,
                              inlineHarness.events());
  start(inlineSession, false);
  REQUIRE(test::until([&] { return inlineSession.snapshot()->performance.completed == 1; }));
  CHECK(calls == 1);
  stop(inlineSession, inlineHarness);
}
TEST_CASE("session/zero-extent-pause-no-extract-and-close-before-prepare") {
  Harness h;
  auto s = session(h);
  s->setViewport({});
  s->start();
  s->resume();
  CHECK(s->snapshot()->performance.deltaExtractions == 0);
  CHECK(h.jobs.empty());
  s->setViewport({{8, 8}, {8, 8}, 1});
  s->requestStop();
  stop(*s, h);
  CHECK(h.jobs.empty());
}
TEST_CASE("session/without-p8-loader-selectmodel-reports-load-not-fake-assets") {
  Harness h;
  auto s = session(h);
  s->selectModel({"not-loaded"});
  REQUIRE_FALSE(h.errors.empty());
  CHECK(h.errors.back().stage == ErrorStage::Load);
  CHECK(s->snapshot()->request == 1);
  stop(*s, h);
}
TEST_CASE("session/cpu_smoke-real-senders-owned-pixels-and-history") {
  Harness h;
  auto s = session(h, true);
  h.readSnapshot = [&] { return s->snapshot(); };
  start(*s, false);
  REQUIRE(test::until([&] { return h.images >= 1; }));
  CHECK(s->snapshot()->performance.accumulatedSamples == 0);
  RenderOptions options;
  options.pipeline = PipelineKind::Pathtrace;
  s->setRenderOptions(options);
  REQUIRE(test::until([&] { return s->snapshot()->performance.accumulatedSamples >= 2; }));
  s->pause();
  REQUIRE(test::until([&] { return s->snapshot()->performance.inFlight == 0; }));
  auto sampleBegin = h.samples.size();
  auto sampleStarts = s->snapshot()->performance.sampleOneFrames;
  auto generation = s->snapshot()->generation;
  CameraState camera;
  camera.camera.position.x = 0.1f;
  s->setCamera(camera);
  s->resume();
  REQUIRE(test::until([&] {
    return s->snapshot()->performance.sampleOneFrames > sampleStarts && h.samples.size() > sampleBegin;
  }));
  CHECK(s->snapshot()->backend == "cpu");
  CHECK(s->snapshot()->performance.completed > 1);
  auto newGeneration = s->snapshot()->generation;
  CHECK(s->snapshot()->generation == generation);
  auto first = std::find_if(h.samples.begin() + sampleBegin, h.samples.end(),
                            [&](auto pair) { return pair.first == newGeneration; });
  REQUIRE(first != h.samples.end());
  CHECK(first->second == 1);
  auto firstSamples = first->second;
  CHECK(s->snapshot()->performance.sampleOneFrames >= 2); // receipt counter survives coalesced UI snapshots
  stop(*s, h);
  REQUIRE(h.lastImage);
  auto image = ReadbackAdapter::copyImage(*h.lastImage->image);
  h.lastImage.reset();
  CHECK(image.pixelColor(0, 0).alpha() == 255);
  std::cout << "CPU session smoke: backend=" << s->snapshot()->backend
            << " completed=" << s->snapshot()->performance.completed << " images=" << h.images
            << " extractions=" << s->snapshot()->performance.deltaExtractions
            << " cameraGeneration=" << newGeneration << " firstSamples=" << firstSamples
            << " finalInFlight=" << s->snapshot()->performance.inFlight
            << " ownedQImageAlpha=" << image.pixelColor(0, 0).alpha() << "\n";
}

TEST_CASE("session/worker-command-rejected-before-mutation") {
  Harness h;
  auto s = session(h);
  std::atomic<bool> rejected{false}, joined{false};
  lexec::simple_counting_scope scope;
  lexec::spawn(lexec::schedule(fixturePool().get_scheduler()) | lexec::then([&]() noexcept {
                 try {
                   s->requestStop();
                 } catch (std::logic_error const &) {
                   rejected = true;
                 }
               }),
               scope.get_token());
  scope.close();
  auto operation = lexec::connect(scope.join(), Joined{&joined});
  lexec::start(operation);
  REQUIRE(test::until([&] { return joined.load(); }));
  CHECK(rejected.load());
  start(*s, false);
  CHECK(test::until([&] { return h.jobs.size() == 1; }, 500));
  s->requestStop();
  if (!h.jobs.empty())
    h.complete(0);
  stop(*s, h);
}
TEST_CASE("session/presentation-worker-done-hops-gui-once-with-real-pool") {
  lexec::simple_counting_scope scope;
  unsigned calls = 0;
  bool gui = false;
  ReadbackAdapter a({}, [&](std::string, Done done) {
    lexec::spawn(lexec::schedule(fixturePool().get_scheduler()) |
                     lexec::then([done = std::move(done)]() noexcept {
                       done(std::monostate{});
                       done(std::monostate{});
                     }),
                 scope.get_token());
  });
  a.preparePath(PresentationPath::Readback, {}, [&](auto) {
    ++calls;
    gui = QThread::currentThread() == QCoreApplication::instance()->thread();
  });
  REQUIRE(test::until([&] { return calls == 1; }));
  scope.close();
  std::atomic<bool> joined{false};
  auto join = lexec::connect(scope.join(), Joined{&joined});
  lexec::start(join);
  REQUIRE(test::until([&] { return joined.load(); }));
  CHECK(gui);
  CHECK(calls == 1);
}
TEST_CASE("session/close-held-candidate-without-target-confirms-optional-destroy") {
  Harness h;
  auto r = resources();
  auto a = std::make_unique<FakePresentation>();
  auto boundary = a.get();
  RenderSession s(std::move(r.runtime), std::move(r.renderer), std::move(r.world), std::move(a), h.submit(),
                  h.events());
  start(s, false);
  REQUIRE(test::until([&] { return boundary->stages().size() == 1; }));
  s.requestStop();
  CHECK_FALSE(h.stopped);
  boundary->complete();
  REQUIRE(test::until([&] { return boundary->stages().size() == 2; }));
  CHECK(boundary->stages()[1] == "drain");
  boundary->complete();
  REQUIRE(test::until([&] { return boundary->stages().size() == 3; }));
  CHECK(boundary->stages()[2] == "release");
  boundary->complete();
  REQUIRE(test::until([&] { return h.stopped; }));
  CHECK(h.jobs.empty());
  CHECK(s.snapshot()->performance.deltaExtractions == 0);
}
TEST_CASE("session/drain-failure-retains-target-until-explicit-retry") {
  Harness h;
  auto r = resources();
  auto a = std::make_unique<FakePresentation>();
  auto boundary = a.get();
  RenderSession s(std::move(r.runtime), std::move(r.renderer), std::move(r.world), std::move(a), h.submit(),
                  h.events());
  start(s, false);
  REQUIRE(test::until([&] { return boundary->stages().size() == 1; }));
  boundary->complete();
  REQUIRE(test::until([&] { return h.jobs.size() == 1; }));
  h.complete(0);
  REQUIRE(test::until([&] { return s.snapshot()->performance.inFlight == 0; }));
  s.requestStop();
  REQUIRE(test::until([&] { return boundary->stages().size() == 2; }));
  boundary->complete(RenderError{ErrorStage::Shutdown, true, "drain failed", "original drain fault"});
  REQUIRE(test::until([&] { return !h.errors.empty(); }));
  CHECK_FALSE(h.stopped);
  CHECK(s.snapshot()->life == LifeState::Stopping);
  s.resume();
  CHECK(boundary->stages().size() == 2);
  s.requestStop();
  REQUIRE(test::until([&] { return boundary->stages().size() == 3; }));
  CHECK(boundary->stages()[2] == "drain");
  boundary->complete();
  REQUIRE(test::until([&] { return boundary->stages().size() == 4; }));
  boundary->complete();
  REQUIRE(test::until([&] { return h.stopped; }));
}
TEST_CASE("session/wrapper-destruction-does-not-cancel-only-drain-notification") {
  struct Tracked : ReadbackAdapter {
    std::shared_ptr<std::atomic<bool>> destroyed;
    explicit Tracked(std::shared_ptr<std::atomic<bool>> flag) : ReadbackAdapter({}), destroyed(flag) {}
    ~Tracked() { destroyed->store(true); }
  };
  auto flag = std::make_shared<std::atomic<bool>>(false);
  Harness h;
  auto r = resources();
  auto s = std::make_unique<RenderSession>(std::move(r.runtime), std::move(r.renderer), std::move(r.world),
                                           std::make_unique<Tracked>(flag), h.submit(), h.events());
  start(*s);
  REQUIRE(test::until([&] { return h.jobs.size() == 2; }));
  s.reset();
  CHECK_FALSE(flag->load());
  h.complete(0);
  h.complete(1);
  REQUIRE(test::until([&] { return flag->load(); }));
}
namespace {
struct FaultRenderer final : lr::render::Renderer {
  std::unique_ptr<lr::render::Renderer> inner;
  std::shared_ptr<std::atomic<QThread *>> destroyed;
  std::shared_ptr<std::atomic<bool>> queued;
  FaultRenderer(std::unique_ptr<lr::render::Renderer> renderer, std::shared_ptr<std::atomic<QThread *>> d,
                std::shared_ptr<std::atomic<bool>> q)
      : inner(std::move(renderer)), destroyed(d), queued(q) {}
  ~FaultRenderer() override {
    destroyed->store(QThread::currentThread());
    QThread::msleep(30); /* injected latency to expose GUI-blocking cleanup */
  }
  tc::DeviceInfo const &deviceInfo() const noexcept override { return inner->deviceInfo(); }
  lr::render::RendererCapabilities const &capabilities() const noexcept override {
    return inner->capabilities();
  }
  lr::render::RenderTargets &targets() noexcept override { return inner->targets(); }
  lr::render::PipelineRegistry &pipelines() noexcept override { return inner->pipelines(); }
  lr::render::MaterialModels &materialModels() noexcept override { return inner->materialModels(); }
  lr::render::FrameSender submit(lr::render::FrameDesc frame) override {
    auto observer = inner->submit(std::move(frame));
    queued->store(true);
    throw std::runtime_error("after actual CPU enqueue, observer lost");
  }
  lr::render::KernelReloadSender reloadKernels(std::string directory) override {
    return inner->reloadKernels(std::move(directory));
  }
};
} // namespace
TEST_CASE("session/real-enqueued-lost-observer-fault-closes-via-worker-renderer-barrier") {
  Harness h;
  auto r = resources();
  auto destroyed = std::make_shared<std::atomic<QThread *>>(nullptr);
  auto queued = std::make_shared<std::atomic<bool>>(false);
  r.renderer = std::make_unique<FaultRenderer>(std::move(r.renderer), destroyed, queued);
  RenderSession s(std::move(r.runtime), std::move(r.renderer), std::move(r.world),
                  std::make_unique<ReadbackAdapter>(h.imageEvents()), {}, h.events());
  start(s);
  REQUIRE(test::until([&] { return s.snapshot()->life == LifeState::Failed; }));
  CHECK(queued->load());
  CHECK(s.snapshot()->performance.deltaExtractions == 1);
  CHECK(s.snapshot()->performance.inFlight == 1);
  s.resume();
  CHECK(s.snapshot()->performance.deltaExtractions == 1);
  unsigned beats = 0;
  QTimer heartbeat;
  heartbeat.setInterval(1);
  QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++beats; });
  heartbeat.start();
  stop(s, h);
  heartbeat.stop();
  CHECK(beats > 0);
  std::cout << "CPU terminal barrier: GUI heartbeat ticks=" << beats
            << " (30ms injected destructor latency), renderer destroyed off GUI="
            << (destroyed->load() != QCoreApplication::instance()->thread()) << "\n";
  CHECK(s.snapshot()->performance.inFlight == 0);
  CHECK(destroyed->load() != nullptr);
  CHECK(destroyed->load() != QCoreApplication::instance()->thread());
  CHECK(h.images == 0);
}

TEST_CASE("session/zero-extent-after-acceptance-drains-before-positive-restore") {
  Harness h;
  auto s = session(h);
  start(*s);
  REQUIRE(test::until([&] { return h.jobs.size() == 2; }));
  auto target = h.jobs[0].frame.views[0].target;
  s->setViewport({});
  h.complete(0);
  h.complete(1);
  REQUIRE(test::until([&] { return s->snapshot()->performance.inFlight == 0; }));
  s->resume();
  QElapsedTimer elapsed;
  elapsed.start();
  REQUIRE(test::until([&] { return elapsed.elapsed() >= 20; }));
  CHECK(h.jobs.size() == 2);
  CHECK(h.images == 0);
  CHECK(s->snapshot()->performance.deltaExtractions == 2);
  s->setViewport({{8, 8}, {8, 8}, 1});
  REQUIRE(test::until([&] { return h.jobs.size() == 4; }));
  CHECK(h.jobs[2].frame.views[0].target != target);
  CHECK(h.jobs[2].frame.delta.meshes.empty());
  s->requestStop();
  h.complete(2);
  h.complete(3);
  stop(*s, h);
}

TEST_CASE("session/nonstandard-ambiguous-submit-also-fault-latches") {
  Harness h;
  h.throwNonStandard = true;
  auto s = session(h);
  start(*s);
  REQUIRE(test::until([&] { return s->snapshot()->life == LifeState::Failed; }));
  s->resume();
  CHECK(h.jobs.size() == 1);
  CHECK(s->snapshot()->performance.deltaExtractions == 1);
  h.complete(0);
  REQUIRE(test::until([&] { return s->snapshot()->performance.inFlight == 0; }));
  stop(*s, h);
}

TEST_CASE("session/image-delivery-fault-preserves-original-cause") {
  Harness h;
  auto r = resources();
  PresentationEvents ev;
  ev.onImage = [](auto) { throw std::runtime_error("original image observer failure"); };
  RenderSession s(std::move(r.runtime), std::move(r.renderer), std::move(r.world),
                  std::make_unique<ReadbackAdapter>(ev), h.submit(), h.events());
  start(s, false);
  REQUIRE(test::until([&] { return h.jobs.size() == 1; }));
  h.complete(0);
  REQUIRE(test::until([&] { return s.snapshot()->life == LifeState::Failed; }));
  REQUIRE_FALSE(h.errors.empty());
  CHECK(h.errors.back().cause == "original image observer failure");
  stop(s, h);
}

TEST_CASE("session/snapshot-observer-may-destroy-wrapper-with-live-callback-captures") {
  struct Tracked : ReadbackAdapter {
    std::shared_ptr<std::atomic<bool>> destroyed;
    explicit Tracked(std::shared_ptr<std::atomic<bool>> flag) : ReadbackAdapter({}), destroyed(flag) {}
    ~Tracked() { destroyed->store(true); }
  };
  auto destroyed = std::make_shared<std::atomic<bool>>(false);
  auto keep = std::make_shared<int>(1);
  std::weak_ptr<int> weak = keep;
  std::unique_ptr<RenderSession> s;
  bool alive = false, returned = false;
  SessionEvents events;
  events.onSnapshot = [keep, &s, &weak, &alive, &returned](auto) {
    // Copy reference addresses before self-destruction: no access to a destroyed closure.
    auto session = &s;
    auto w = &weak;
    auto a = &alive;
    auto r = &returned;
    session->reset();
    *a = !w->expired();
    *r = true;
  };
  auto resource = resources();
  s = std::make_unique<RenderSession>(std::move(resource.runtime), std::move(resource.renderer),
                                      std::move(resource.world), std::make_unique<Tracked>(destroyed),
                                      SubmitFrame{}, std::move(events));
  keep.reset();
  s->start();
  REQUIRE(test::until([&] { return returned; }));
  CHECK(alive);
  REQUIRE(test::until([&] { return destroyed->load(); }));
  CHECK(weak.expired());
}

TEST_CASE("session/fix-visibility-camera-keeps-target-and-submits-latest-camera") {
  Harness h;
  auto r = resources();
  std::vector<std::string> stages;
  auto adapter = std::make_unique<ReadbackAdapter>(h.imageEvents(), [&](auto stage, Done done) {
    stages.push_back(stage);
    done(std::monostate{});
  });
  RenderSession s(std::move(r.runtime), std::move(r.renderer), std::move(r.world), std::move(adapter),
                  h.submit(), h.events());
  start(s, false);
  REQUIRE(test::until([&] { return h.jobs.size() == 1; }));
  auto target = h.jobs[0].frame.views[0].target;
  auto generation = s.snapshot()->generation;
  h.complete(0);
  REQUIRE(test::until([&] { return s.snapshot()->performance.inFlight == 0; }));
  for (unsigned i = 1; i <= 3; ++i) {
    CameraState camera;
    camera.camera.position.x = i * 0.1f;
    s.setCamera(camera);
    RenderOptions exposure;
    exposure.exposure = 1 + i * 0.1f;
    s.setRenderOptions(exposure);
    REQUIRE(test::until([&] { return h.jobs.size() == i + 1; }));
    CHECK(h.jobs[i].frame.views[0].camera.position.x == camera.camera.position.x);
    CHECK(h.jobs[i].frame.views[0].target == target);
    CHECK(s.snapshot()->generation == generation);
    h.complete(i);
    REQUIRE(test::until([&] { return s.snapshot()->performance.inFlight == 0; }));
  }
  CHECK(stages == std::vector<std::string>{"prepare"});
  stop(s, h);
}
TEST_CASE("session/fix-visibility-camera-real-cpu-does-not-reprepare") {
  Harness h;
  auto r = resources();
  std::vector<std::string> stages;
  auto adapter = std::make_unique<ReadbackAdapter>(h.imageEvents(), [&](auto stage, Done done) {
    stages.push_back(stage);
    done(std::monostate{});
  });
  RenderSession s(std::move(r.runtime), std::move(r.renderer), std::move(r.world), std::move(adapter), {},
                  h.events());
  start(s, false);
  REQUIRE(test::until([&] { return h.images >= 1; }));
  auto generation = s.snapshot()->generation;
  for (unsigned i = 1; i <= 3; ++i) {
    auto next = h.images + 1;
    CameraState camera;
    camera.camera.position.x = i * 0.1f;
    s.setCamera(camera);
    REQUIRE(test::until([&] { return h.images >= next; }));
    CHECK(s.snapshot()->generation == generation);
    CHECK(stages == std::vector<std::string>{"prepare"});
  }
  std::cout << "Visibility camera CPU: images=" << h.images
            << " prepare/drain/release stages=" << stages.size() << " generation=" << s.snapshot()->generation
            << "\n";
  stop(s, h);
}
TEST_CASE("session/fix-disabling-taa-still-rebuilds-history-target") {
  Harness h;
  auto s = session(h);
  RenderOptions options;
  options.taa = true;
  s->setRenderOptions(options);
  s->setViewport({{8, 8}, {8, 8}, 1});
  s->start();
  REQUIRE(test::until([&] { return h.jobs.size() == 2; }));
  auto target = h.jobs[0].frame.views[0].target;
  auto generation = s->snapshot()->generation;
  options.taa = false;
  s->setRenderOptions(options);
  h.complete(0);
  h.complete(1);
  REQUIRE(test::until([&] { return h.jobs.size() == 3; }));
  CHECK(s->snapshot()->generation > generation);
  CHECK(h.jobs[2].frame.views[0].target != target);
  s->requestStop();
  h.complete(2);
  stop(*s, h);
}

TEST_CASE("session/fix-worker-wrapper-destruction-queues-gui-cleanup") {
  struct Tracked : ReadbackAdapter {
    std::shared_ptr<std::atomic<QThread *>> destroyed;
    explicit Tracked(std::shared_ptr<std::atomic<QThread *>> d) : ReadbackAdapter({}), destroyed(d) {}
    ~Tracked() { destroyed->store(QThread::currentThread()); }
  };
  auto destroyed = std::make_shared<std::atomic<QThread *>>(nullptr);
  auto r = resources();
  auto s =
      std::make_unique<RenderSession>(std::move(r.runtime), std::move(r.renderer), std::move(r.world),
                                      std::make_unique<Tracked>(destroyed), SubmitFrame{}, SessionEvents{});
  lexec::simple_counting_scope scope;
  std::atomic<bool> joined{false};
  lexec::spawn(lexec::schedule(fixturePool().get_scheduler()) |
                   lexec::then([s = std::move(s)]() mutable noexcept { s.reset(); }),
               scope.get_token());
  scope.close();
  auto join = lexec::connect(scope.join(), Joined{&joined});
  lexec::start(join);
  REQUIRE(test::until([&] { return joined.load(); }));
  REQUIRE(test::until([&] { return destroyed->load() != nullptr; }));
  CHECK(destroyed->load() == QCoreApplication::instance()->thread());
}
namespace {
struct TargetEvidence {
  unsigned created = 0, destroyed = 0;
  bool releasedAfterDestroy = false;
  std::atomic<bool> adapterDestroyed{false};
};
struct CountingTargets : lr::render::RenderTargets {
  lr::render::RenderTargets &inner;
  std::shared_ptr<TargetEvidence> evidence;
  CountingTargets(lr::render::RenderTargets &i, std::shared_ptr<TargetEvidence> e) : inner(i), evidence(e) {}
  lr::core::RenderTargetId createWindow(lr::render::WindowTargetDesc const &d) override {
    return inner.createWindow(d);
  }
  lr::core::RenderTargetId createImage(lr::render::ImageTargetDesc const &d) override {
    auto id = inner.createImage(d);
    ++evidence->created;
    return id;
  }
  void resize(lr::core::RenderTargetId id, lr::core::ivec2 e) override { inner.resize(id, e); }
  void destroy(lr::core::RenderTargetId id) override {
    inner.destroy(id);
    ++evidence->destroyed;
  }
};
struct CountingRenderer : lr::render::Renderer {
  std::unique_ptr<lr::render::Renderer> inner;
  CountingTargets tracked;
  CountingRenderer(std::unique_ptr<lr::render::Renderer> i, std::shared_ptr<TargetEvidence> e)
      : inner(std::move(i)), tracked(inner->targets(), e) {}
  tc::DeviceInfo const &deviceInfo() const noexcept override { return inner->deviceInfo(); }
  lr::render::RendererCapabilities const &capabilities() const noexcept override {
    return inner->capabilities();
  }
  lr::render::RenderTargets &targets() noexcept override { return tracked; }
  lr::render::PipelineRegistry &pipelines() noexcept override { return inner->pipelines(); }
  lr::render::MaterialModels &materialModels() noexcept override { return inner->materialModels(); }
  lr::render::FrameSender submit(lr::render::FrameDesc f) override { return inner->submit(std::move(f)); }
  lr::render::KernelReloadSender reloadKernels(std::string d) override {
    return inner->reloadKernels(std::move(d));
  }
};
struct ThrowContext : ReadbackAdapter {
  std::shared_ptr<TargetEvidence> evidence;
  bool throwOnBind = true;
  std::function<void()> onBind;
  explicit ThrowContext(std::shared_ptr<TargetEvidence> e) : ReadbackAdapter({}), evidence(e) {}
  ~ThrowContext() { evidence->adapterDestroyed = true; }
  void setFrameContext(SessionGeneration g, ViewportExtent const &e) override {
    ReadbackAdapter::setFrameContext(g, e);
    if (throwOnBind)
      throw std::runtime_error("after actual target creation");
    if (onBind)
      onBind();
  }
  void releasePath(Done done) override {
    evidence->releasedAfterDestroy = evidence->created == 1 && evidence->destroyed == 1;
    ReadbackAdapter::releasePath(std::move(done));
  }
};
} // namespace
TEST_CASE("session/fix-created-target-bind-fault-drains-releases-and-drops-keepalive") {
  bool stale = false;
  SUBCASE("frame context throws") {}
  SUBCASE("ready rejects changed generation") { stale = true; }
  Harness h;
  auto e = std::make_shared<TargetEvidence>();
  auto r = resources();
  r.renderer = std::make_unique<CountingRenderer>(std::move(r.renderer), e);
  std::unique_ptr<RenderSession> s;
  auto adapter = std::make_unique<ThrowContext>(e);
  adapter->throwOnBind = !stale;
  adapter->onBind = [&] { s->setViewport({{9, 8}, {9, 8}, 1}); };
  s = std::make_unique<RenderSession>(std::move(r.runtime), std::move(r.renderer), std::move(r.world),
                                      std::move(adapter), h.submit(), h.events());
  start(*s, false);
  REQUIRE(test::until([&] { return s->snapshot()->life == LifeState::Failed; }));
  CHECK(e->created == 1);
  CHECK(e->destroyed == 0);
  CHECK(h.jobs.empty());
  REQUIRE_FALSE(h.errors.empty());
  CHECK(h.errors.back().cause == (stale ? "stale presentation candidate" : "after actual target creation"));
  stop(*s, h);
  CHECK(e->destroyed == 1);
  CHECK(e->releasedAfterDestroy);
  s.reset();
  REQUIRE(test::until([&] { return e->adapterDestroyed.load(); }));
}

TEST_CASE("session/p9-auto-fallback-is-observable-and-strict-mode-fails") {
  for (bool automatic : {true, false}) {
    Harness h;
    auto r = resources();
    std::vector<PresentationPath> paths;
    struct Reject : ReadbackAdapter {
      std::vector<PresentationPath> &paths;
      bool rejected = false;
      Reject(PresentationEvents e, std::vector<PresentationPath> &p) : ReadbackAdapter(e), paths(p) {}
      void preparePath(PresentationPath p, std::shared_ptr<NativeHostLease> l, Done d) override {
        paths.push_back(p);
        rejected = p == PresentationPath::VulkanShared;
        if (rejected)
          d(RenderError{ErrorStage::Probe, true, "Capability injected", "missing external-memory"});
        else
          ReadbackAdapter::preparePath(p, l, std::move(d));
      }
      void beginDrain(Done d) override {
        if (rejected)
          d(std::monostate{});
        else
          ReadbackAdapter::beginDrain(std::move(d));
      }
      void releasePath(Done d) override {
        if (rejected) {
          rejected = false;
          d(std::monostate{});
        } else
          ReadbackAdapter::releasePath(std::move(d));
      }
    };
    RenderSession s(std::move(r.runtime), std::move(r.renderer), std::move(r.world),
                    std::make_unique<Reject>(h.imageEvents(), paths), {}, h.events());
    s.setPresentationMode(PresentationPath::VulkanShared, automatic);
    start(s, false);
    REQUIRE(test::until([&] { return h.images > 0 || s.snapshot()->life == LifeState::Failed; }));
    REQUIRE_FALSE(paths.empty());
    CHECK(paths[0] == PresentationPath::VulkanShared);
    if (automatic) {
      CHECK(h.images > 0);
      CHECK(s.snapshot()->path == PresentationPath::Readback);
      CHECK_FALSE(s.snapshot()->warnings.empty());
    } else {
      CHECK(h.images == 0);
      CHECK(s.snapshot()->life == LifeState::Failed);
    }
    stop(s, h);
  }
}
TEST_CASE("session/p9-hide-native-invalidate-retire-old-frames-and-rebuild") {
  Harness h;
  auto s = session(h);
  start(*s, false);
  REQUIRE(test::until([&] { return h.jobs.size() == 1; }));
  auto old = s->snapshot()->generation;
  s->setVisible(false);
  h.complete(0);
  REQUIRE(test::until([&] { return s->snapshot()->performance.inFlight == 0; }));
  CHECK(h.images == 0);
  CHECK(s->snapshot()->generation > old);
  s->setViewport({{0, 0}, {0, 0}, 1});
  s->setVisible(true);
  auto count = h.jobs.size();
  QCoreApplication::processEvents();
  CHECK(h.jobs.size() == count);
  s->setViewport({{8, 8}, {8, 8}, 1});
  REQUIRE(test::until([&] { return h.jobs.size() == 2; }));
  auto next = s->snapshot()->generation;
  s->invalidatePresentation({ErrorStage::Surface, true, "Platform lost", "test invalid native lease"});
  CHECK(s->snapshot()->generation > next);
  h.complete(1);
  s->requestStop();
  REQUIRE(test::until([&] { return h.stopped; }));
  CHECK(h.images == 0);
}

TEST_CASE("session/p9-lighting-value-command-updates-real-delta-without-camera-target-churn") {
  Harness h;
  auto s = session(h);
  start(*s, false);
  REQUIRE(test::until([&] { return h.jobs.size() == 1; }));
  h.complete(0);
  REQUIRE(test::until([&] { return s->snapshot()->performance.inFlight == 0; }));
  auto generation = s->snapshot()->generation;
  s->setAmbient(.5f);
  CameraState camera;
  camera.camera.position.x = 1;
  s->setCamera(camera);
  REQUIRE(test::until([&] { return h.jobs.size() == 2; }));
  CHECK(h.jobs[1].frame.delta.environment.has_value());
  if (h.jobs[1].frame.delta.environment)
    CHECK(h.jobs[1].frame.delta.environment->radiance.x == doctest::Approx(.5));
  CHECK(s->snapshot()->generation == generation);
  CHECK_THROWS_AS(s->setAmbient(-1), std::invalid_argument);
  CHECK_THROWS_AS(s->setRoughness(2), std::invalid_argument);
  h.complete(1);
  s->requestStop();
  REQUIRE(test::until([&] { return h.stopped; }));
}

TEST_CASE("session/p10-snapshot-real-appearance-and-stop-rejects-changes") {
  Harness h;
  auto s = session(h);
  s->setAmbient(.8f);
  s->setRoughness(.3f);
  s->setNormalMapping(false);
  CHECK(s->snapshot()->ambient == doctest::Approx(.8));
  CHECK(s->snapshot()->roughness == doctest::Approx(.3));
  CHECK_FALSE(s->snapshot()->normalMapping);
  s->requestStop();
  s->setAmbient(1.7f);
  s->setRoughness(.9f);
  s->setNormalMapping(true);
  CHECK(s->snapshot()->ambient == doctest::Approx(.8));
  CHECK(s->snapshot()->roughness == doctest::Approx(.3));
  CHECK_FALSE(s->snapshot()->normalMapping);
  stop(*s, h);
}

TEST_CASE("session/p10-pt-camera-real-cpu-keeps-target-and-sample-one") {
  Harness h;
  auto r = resources();
  std::vector<std::string> stages;
  auto adapter = std::make_unique<ReadbackAdapter>(h.imageEvents(), [&](auto stage, Done done) {
    stages.push_back(stage);
    done(std::monostate{});
  });
  RenderSession s(std::move(r.runtime), std::move(r.renderer), std::move(r.world), std::move(adapter),
                  SubmitFrame{}, h.events());
  h.readSnapshot = [&] { return s.snapshot(); };
  start(s);
  REQUIRE(test::until([&] { return s.snapshot()->performance.accumulatedSamples >= 2; }));
  s.pause();
  REQUIRE(test::until([&] { return s.snapshot()->performance.inFlight == 0; }));
  auto generation = s.snapshot()->generation;
  auto begin = h.samples.size();
  auto starts = s.snapshot()->performance.sampleOneFrames;
  auto c = s.snapshot()->camera;
  c.camera.position.x += .2f;
  s.setCamera(c);
  s.resume();
  REQUIRE(test::until(
      [&] { return s.snapshot()->performance.sampleOneFrames > starts && h.samples.size() > begin; }));
  s.pause();
  REQUIRE(test::until([&] { return s.snapshot()->performance.inFlight == 0; }));
  CHECK(s.snapshot()->generation == generation);
  CHECK(h.samples[begin].second == 1);
  CHECK(stages == std::vector<std::string>{"prepare"});
  stop(s, h);
}
TEST_CASE("session/p10-taa-camera-real-target-preserved-cut-only-next-submit") {
  Harness h;
  auto r = resources();
  std::vector<std::string> stages;
  auto adapter = std::make_unique<ReadbackAdapter>(h.imageEvents(), [&](auto stage, Done done) {
    stages.push_back(stage);
    done(std::monostate{});
  });
  RenderSession s(std::move(r.runtime), std::move(r.renderer), std::move(r.world), std::move(adapter),
                  h.submit(), h.events());
  s.setViewport({{8, 8}, {8, 8}, 1});
  s.setRenderOptions({PipelineKind::Visibility, 1, true});
  s.start();
  REQUIRE(test::until([&] { return h.jobs.size() == 2; }));
  s.pause();
  h.complete(0);
  h.complete(1);
  REQUIRE(test::until([&] { return s.snapshot()->performance.inFlight == 0; }));
  auto target = h.jobs[1].frame.views[0].target;
  auto gen = s.snapshot()->generation;
  auto c = s.snapshot()->camera;
  c.camera.position.x += .2f;
  s.setCamera(c);
  s.resume();
  REQUIRE(test::until([&] { return h.jobs.size() == 4; }));
  CHECK(s.snapshot()->generation == gen);
  CHECK(h.jobs[2].frame.views[0].target == target);
  CHECK(h.jobs[2].frame.views[0].cameraCut);
  CHECK_FALSE(h.jobs[3].frame.views[0].cameraCut);
  CHECK(h.jobs[2].frame.views[0].camera.position.x == doctest::Approx(.2));
  CHECK(h.jobs[2].frame.delta.meshes.empty());
  CHECK(stages == std::vector<std::string>{"prepare"});
  s.requestStop();
  h.complete(2);
  h.complete(3);
  stop(s, h);
}

#include "ui/MainWindow.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QKeyEvent>
#include <QLineEdit>
TEST_CASE(
    "session/p10-fix-real-cpu-gui-command-ack-survives-delayed-publication") {
  Harness h;
  auto r = resources();
  MainWindow *window = nullptr;
  unsigned delivered = 0, commandCount = 0;
  SessionEvents events = h.events();
  events.onSnapshot = [&](SessionSnapshotPtr snap) {
    if (window)
      QTimer::singleShot(80, window, [&, snap] {
        window->showSnapshot(snap);
        ++delivered;
      });
  };
  RenderSession s(
      std::move(r.runtime), std::move(r.renderer), std::move(r.world),
      std::make_unique<ReadbackAdapter>(h.imageEvents()), {}, events);
  UiCommands c;
  c.ambient = [&](float v) {
    ++commandCount;
    s.setAmbient(v);
    return s.snapshot()->ambient;
  };
  MainWindow w(c);
  window = &w;
  start(s, false);
  REQUIRE(test::until([&] { return h.images > 0 && delivered > 0; }));
  auto old = s.snapshot();
  w.showSnapshot(old);
  w.show();
  QApplication::processEvents();
  auto spin = w.findChild<QDoubleSpinBox *>("ambient");
  auto edit = spin->findChild<QLineEdit *>();
  REQUIRE(edit);
  spin->setFocus();
  QApplication::processEvents();
  auto key = [&](int k, QString text = {},
                 Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QKeyEvent down(QEvent::KeyPress, k, mods, text),
        up(QEvent::KeyRelease, k, mods, text);
    QApplication::sendEvent(edit, &down);
    QApplication::sendEvent(edit, &up);
  };
  key(Qt::Key_A, {}, Qt::ControlModifier);
  key(Qt::Key_1, "1");
  key(Qt::Key_Period, ".");
  unsigned ticks = 0;
  QTimer refresh;
  QObject::connect(&refresh, &QTimer::timeout, &w, [&] {
    w.showSnapshot(old);
    ++ticks;
  });
  refresh.start(5);
  REQUIRE(test::until([&] { return ticks >= 3; }));
  CHECK(edit->text() == "1.");
  CHECK(commandCount == 0);
  refresh.stop();
  key(Qt::Key_5, "5");
  auto before = h.images, published = delivered;
  key(Qt::Key_Return, "\r");
  CHECK(s.snapshot()->ambient == doctest::Approx(1.5));
  w.showSnapshot(old);
  CHECK(spin->value() == doctest::Approx(1.5));
  REQUIRE(
      test::until([&] { return delivered > published && h.images > before; }));
  CHECK(spin->value() == doctest::Approx(1.5));
  CHECK(commandCount == 1);
  REQUIRE(h.lastImage);
  CHECK(h.lastImage->image->pixels.size() == 64);
  CHECK(h.errors.empty());
  stop(s, h);
  w.showSnapshot(s.snapshot());
  key(Qt::Key_A, {}, Qt::ControlModifier);
  key(Qt::Key_2, "2");
  key(Qt::Key_Period, ".");
  key(Qt::Key_5, "5");
  key(Qt::Key_Return, "\r");
  CHECK(commandCount == 2);
  CHECK(s.snapshot()->ambient == doctest::Approx(1.5));
  CHECK(spin->value() == doctest::Approx(1.5));
}

TEST_CASE(
    "shutdown/two-submit-receipts-held-drain-and-release-never-block-gui") {
  Harness h;
  auto r = resources();
  auto a = std::make_unique<FakePresentation>();
  auto b = a.get();
  RenderSession *owner = nullptr;
  UiCommands commands;commands.stop = [&] { if (owner) owner->requestStop(); };
  MainWindow window(commands);window.show();
  auto events = h.events();
  events.onStopped = [&] { h.stopped = true;window.showSnapshot(owner->snapshot());window.close(); };
  RenderSession s(std::move(r.runtime), std::move(r.renderer),
                  std::move(r.world), std::move(a), h.submit(), events);
  owner = &s;
  start(s, true);
  REQUIRE(test::until([&] { return b->stages().size() == 1; }));
  b->complete();
  REQUIRE(test::until([&] { return h.jobs.size() == 2; }));
  window.close();
  window.close();
  unsigned ticks = 0;
  QTimer heartbeat;
  QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++ticks; });
  heartbeat.start(1);
  REQUIRE(test::until([&] { return ticks >= 5; }));
  CHECK_FALSE(h.stopped);
  CHECK(window.isVisible());
  CHECK(s.snapshot()->performance.inFlight == 2);
  h.complete(0);
  h.complete(0);
  h.complete(1);
  REQUIRE(test::until([&] { return b->stages().size() == 2; }));
  CHECK(s.snapshot()->performance.inFlight == 0);
  CHECK(b->stages()[1] == "drain");
  auto t = ticks;
  REQUIRE(test::until([&] { return ticks >= t + 5; }));
  CHECK_FALSE(h.stopped);
  CHECK(window.isVisible());
  b->complete();
  REQUIRE(test::until([&] { return b->stages().size() == 3; }));
  CHECK(b->stages()[2] == "release");
  CHECK_FALSE(h.stopped);
  CHECK(window.isVisible());
  t = ticks;
  REQUIRE(test::until([&] { return ticks >= t + 5; }));
  b->complete();
  REQUIRE(test::until([&] { return h.stopped; }));
  CHECK_FALSE(window.isVisible());
  CHECK(h.jobs.size() == 2);
  CHECK(s.snapshot()->life == LifeState::Stopped);
}
