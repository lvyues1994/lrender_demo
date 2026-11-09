#include "assets/AssetLoader.h"
#include "presentation/ReadbackAdapter.h"
#include "render/AssetInstall.h"
#include "render/RenderSession.h"
#include "support/QtWait.h"
#include <QCoreApplication>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <condition_variable>
#include <doctest/doctest.h>
#include <iostream>
#include <lqtexec/EventLoopContext.h>
#include <lr/core/Primitives.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
using namespace demo;
namespace {
CpuAssetPtr fixture(std::string id, unsigned parts = 3, float marker = 0) {
  auto a = std::make_shared<CpuAsset>();
  a->model = {id};
  auto mesh = lr::core::makeQuad({.5f, .5f});
  mesh.positions[0].x = marker;
  a->bounds = lr::core::boundsOf(mesh);
  auto handle = std::make_shared<lr::core::MeshData const>(std::move(mesh));
  for (auto role :
       {TextureRole::Diffuse, TextureRole::Normal, TextureRole::Specular}) {
    lr::core::ImageData i;
    i.extent = {1, 1};
    i.encoding = role == TextureRole::Diffuse ? lr::core::ColorEncoding::Srgb
                                              : lr::core::ColorEncoding::Linear;
    i.levels = {{128, 128, 255, 255}};
    a->textures.push_back(
        {std::make_shared<lr::core::ImageData const>(std::move(i)), {}, role});
  }
  MaterialAsset m;
  m.baseColorTexture = 0;
  m.normalTexture = 1;
  a->materials.push_back(m);
  for (unsigned n = 0; n < parts; ++n)
    a->parts.push_back({handle, 0});
  return a;
}
struct Joined {
  using receiver_concept = lexec::receiver_t;
  std::atomic<bool> *done;
  void set_value() && noexcept { done->store(true); }
};
struct App {
  lqtexec::EventLoopContext gui;
  lexec::counting_scope scope;
  QThreadPool pool;
  std::unique_ptr<AssetLoader> loader;
  App(AssetLoader::Decode decode = {}, int threads = 2) {
    pool.setMaxThreadCount(threads);
    loader = std::make_unique<AssetLoader>(
        &pool, gui.get_scheduler(), scope.get_token(), std::move(decode));
  }
  ~App() {
    if (!test::until([&] { return loader->inFlight() == 0; }))
      std::_Exit(2);
    loader.reset();
    scope.close();
    std::atomic<bool> done{false};
    auto join = lexec::connect(scope.join(), Joined{&done});
    lexec::start(join);
    if (!test::until([&] { return done.load(); }))
      std::_Exit(2);
  }
};
struct Gate {
  std::mutex mutex;
  std::condition_variable cv;
  std::map<std::string, bool> open;
  std::atomic<unsigned> started{0};
  std::atomic<bool> worker{true};
  void release(std::string id) {
    std::lock_guard<std::mutex> lock(mutex);
    open[id] = true;
    cv.notify_all();
  }
  Result<CpuAssetPtr> decode(AssetEntry const &e, std::function<bool()>) {
    worker = worker.load() &&
             QThread::currentThread() != QCoreApplication::instance()->thread();
    ++started;
    std::unique_lock<std::mutex> lock(mutex);
    if (!cv.wait_for(lock, std::chrono::seconds(3),
                     [&] { return open[e.id.value]; }))
      return RenderError{ErrorStage::Load, false, "gate timeout",
                         "bounded fixture gate"};
    return fixture(e.id.value, 64, e.id.value == "diablo" ? 2 : 1);
  }
};
AssetEntry entry(char const *id) {
  return std::get<AssetEntry>(AssetCatalog{}.describe({id}));
}
struct Owned {
  std::unique_ptr<lr::runtime::Runtime> runtime;
  std::unique_ptr<lr::render::Renderer> renderer;
  std::unique_ptr<lr::scene::World> world;
};
Owned resources(App &app) {
  Owned r;
  std::atomic<bool> done{false};
  std::exception_ptr error;
  lexec::spawn(lexec::schedule(lqtexec::ThreadPoolScheduler{&app.pool}) |
                   lexec::then([&]() noexcept {
                     try {
                       lr::runtime::RuntimeConfig c;
                       c.framePoolThreads = 2;
                       c.backgroundPoolThreads = 1;
                       c.ioWorkers = 1;
                       r.runtime = lr::runtime::makeRuntime(c);
                       lr::render::RendererConfig rc;
                       rc.backend = "cpu";
                       rc.runtime = r.runtime.get();
                       rc.framesInFlight = 2;
                       r.renderer = lr::render::makeRenderer(rc);
                     } catch (...) {
                       error = std::current_exception();
                     }
                     done = true;
                   }),
               app.scope.get_token());
  if (!test::until([&] { return done.load(); }))
    std::_Exit(2);
  if (error)
    std::rethrow_exception(error);
  r.world = lr::scene::makeWorld({});
  r.world->setEnvironment({lr::core::vec3{.25f}});
  return r;
}
struct Harness {
  bool stopped = false;
  unsigned images = 0;
  std::vector<RenderError> errors;
  std::vector<lr::render::FrameDesc> frames;
  std::optional<OwnedFrame> image;
  SubmitFrame fake() {
    return [this](auto f, auto stamp, auto done) {
      lr::render::FrameResult r;
      lr::render::ViewResult v;
      v.view = f.views[0].id;
      v.target = f.views[0].target;
      lr::render::ImageRgba8 i;
      i.extent = {16, 16};
      i.pixels.resize(256, 0xff00ffff);
      v.image = std::move(i);
      r.views.push_back(std::move(v));
      frames.push_back(std::move(f));
      done({stamp, std::move(r)});
    };
  }
  SessionEvents events() {
    return {{},
            [this](auto e) { errors.push_back(e); },
            [this] { stopped = true; }};
  }
  PresentationEvents presentation() {
    return {[this](auto f) {
              ++images;
              image = std::move(f);
            },
            [this](auto e) { errors.push_back(e); }};
  }
};
void start(RenderSession &s) {
  s.setViewport({{16, 16}, {16, 16}, 1});
  s.start();
}
void stop(RenderSession &s, Harness &h) {
  s.requestStop();
  REQUIRE(test::until([&] { return h.stopped; }));
}
} // namespace
TEST_CASE(
    "asset_async/install-batches-rollback-shared-references-and-unsent-delta") {
  auto world = lr::scene::makeWorld({});
  AssetInstall install(*world);
  auto old = fixture("old", 3);
  install.begin(old);
  CHECK_FALSE(install.step(1));
  CHECK(world->entityCount() == 1);
  CHECK_FALSE(install.step(1));
  CHECK(install.step(1));
  install.commit();
  auto first = world->extractDelta();
  CHECK(first.instances.size() == 3);
  CHECK(first.textures.size() == 2);
  CHECK(world->resources().counts().textures == 2);
  auto shared = std::make_shared<CpuAsset>(*old);
  shared->model = {"new"};
  install.begin(shared);
  CHECK_FALSE(install.step(1));
  CHECK(world->entityCount() == 4);
  install.rollback();
  CHECK(world->entityCount() == 3);
  CHECK(world->resources().counts().meshes == 1);
  CHECK(world->resources().counts().materials == 1);
  CHECK(world->resources().counts().textures == 2);
  auto delta = world->extractDelta();
  CHECK(delta.instances.empty());
  CHECK(delta.meshes.empty());
  CHECK(delta.materials.empty());
  CHECK(delta.removed.instances.empty());
  CHECK(delta.removed.meshes.empty());
  CHECK(install.current()->model.value == "old");
  install.clear();
  CHECK(world->entityCount() == 0);
}
TEST_CASE("asset_async/install-invalid-candidate-preserves-old") {
  auto world = lr::scene::makeWorld({});
  AssetInstall install(*world);
  auto old = fixture("old", 1);
  install.begin(old);
  REQUIRE(install.step(1));
  install.commit();
  auto bad = std::make_shared<CpuAsset>(*old);
  bad->parts[0].material = 99;
  CHECK_THROWS_AS(install.begin(bad), std::invalid_argument);
  CHECK(install.current() == old);
  CHECK(world->entityCount() == 1);
  install.clear();
}
TEST_CASE("asset_async/"
          "pool-out-of-order-cancel-is-once-gui-and-running-task-settles") {
  auto gate = std::make_shared<Gate>();
  App app([gate](auto const &e, auto stop) { return gate->decode(e, stop); });
  std::vector<LoadOutcome> results;
  bool gui = true;
  auto done = [&](auto r) {
    gui = gui &&
          QThread::currentThread() == QCoreApplication::instance()->thread();
    results.push_back(std::move(r));
  };
  auto a = app.loader->beginLoad(entry("african_head"), 1, done);
  app.loader->beginLoad(entry("diablo"), 2, done);
  REQUIRE(test::until([&] { return gate->started == 2; }));
  gate->release("diablo");
  REQUIRE(test::until([&] { return results.size() == 1; }));
  CHECK(results[0].request == 2);
  CHECK(results[0].status == LoadStatus::Loaded);
  a.cancel();
  a.cancel();
  CHECK(app.loader->inFlight() == 1);
  gate->release("african_head");
  REQUIRE(test::until([&] { return results.size() == 2; }));
  CHECK(results[1].status == LoadStatus::Cancelled);
  CHECK(gui);
  CHECK(gate->worker.load());
}
TEST_CASE("asset_async/queued-cancel-does-not-wait-for-blocking-worker") {
  auto gate = std::make_shared<Gate>();
  App app([gate](auto const &e, auto s) { return gate->decode(e, s); }, 1);
  std::vector<LoadOutcome> results;
  app.loader->beginLoad(entry("african_head"), 1,
                        [&](auto r) { results.push_back(r); });
  REQUIRE(test::until([&] { return gate->started == 1; }));
  auto ticket = app.loader->beginLoad(entry("diablo"), 2,
                                      [&](auto r) { results.push_back(r); });
  ticket.cancel();
  REQUIRE(test::until([&] { return results.size() == 1; }, 1000));
  CHECK(results[0].request == 2);
  CHECK(results[0].status == LoadStatus::Cancelled);
  CHECK(gate->started == 1);
  gate->release("african_head");
  REQUIRE(test::until([&] { return app.loader->inFlight() == 0; }));
}
TEST_CASE("asset_async/recoverable-error-not-generic-cancellation") {
  App app([](auto const &, auto) -> Result<CpuAssetPtr> {
    return RenderError{ErrorStage::Load, true, "temporary", "retry later"};
  });
  std::optional<LoadOutcome> result;
  app.loader->beginLoad(entry("floor"), 1, [&](auto r) { result = r; });
  REQUIRE(test::until([&] { return result.has_value(); }));
  CHECK(result->status == LoadStatus::Failed);
  CHECK(result->error->cause == "retry later");
}
TEST_CASE("asset_async/"
          "session-latest-wins-bounded-install-no-half-delta-close-waits") {
  auto gate = std::make_shared<Gate>();
  App app([gate](auto const &e, auto s) { return gate->decode(e, s); });
  auto r = resources(app);
  Harness h;
  RenderSession session(std::move(r.runtime), std::move(r.renderer),
                        std::move(r.world),
                        std::make_unique<ReadbackAdapter>(h.presentation()),
                        h.fake(), h.events());
  session.setAssetLoader(*app.loader);
  start(session);
  session.selectModel({"african_head"});
  REQUIRE(test::until([&] { return gate->started == 1; }));
  session.selectModel({"diablo"});
  REQUIRE(test::until([&] { return gate->started == 2; }));
  unsigned beats = 0;
  QTimer heartbeat;
  heartbeat.setInterval(0);
  QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++beats; });
  heartbeat.start();
  gate->release("diablo");
  REQUIRE(test::until([&] {
    return session.snapshot()->installedModel &&
           session.snapshot()->installedModel->value == "diablo" &&
           h.images >= 2;
  }));
  heartbeat.stop();
  CHECK(beats > 2);
  bool newScene = false;
  for (auto const &f : h.frames) {
    CHECK((f.delta.instances.empty() || f.delta.instances.size() == 64));
    for (auto const &m : f.delta.meshes) {
      CHECK(m.mesh->positions[0].x == 2);
      newScene = true;
    }
  }
  CHECK(newScene);
  session.requestStop();
  CHECK_FALSE(h.stopped);
  gate->release("african_head");
  REQUIRE(test::until([&] { return h.stopped; }));
  CHECK(app.loader->inFlight() == 0);
  std::cout << "Async latest: GUI ticks=" << beats
            << " frames=" << h.frames.size()
            << " full scene parts=64, no half delta\n";
}
TEST_CASE("asset_async/session-decode-failure-retains-installed-old") {
  App app([](auto const &e, auto) -> Result<CpuAssetPtr> {
    if (e.id.value == "diablo")
      return RenderError{ErrorStage::Load, true, "decode failed",
                         "original decoder reason"};
    return fixture(e.id.value, 2);
  });
  auto r = resources(app);
  Harness h;
  RenderSession session(std::move(r.runtime), std::move(r.renderer),
                        std::move(r.world),
                        std::make_unique<ReadbackAdapter>(h.presentation()),
                        h.fake(), h.events());
  session.setAssetLoader(*app.loader);
  start(session);
  session.selectModel({"african_head"});
  REQUIRE(test::until([&] {
    return session.snapshot()->installedModel.has_value() && h.image &&
           h.image->stamp.generation == session.snapshot()->generation;
  }));
  session.selectModel({"diablo"});
  REQUIRE(test::until([&] { return !h.errors.empty(); }));
  CHECK(session.snapshot()->installedModel->value == "african_head");
  CHECK(h.errors.back().cause == "original decoder reason");
  stop(session, h);
}
TEST_CASE("asset_async/real-decoder-world-install-cpu-image") {
  App app;
  auto r = resources(app);
  Harness h;
  RenderSession session(
      std::move(r.runtime), std::move(r.renderer), std::move(r.world),
      std::make_unique<ReadbackAdapter>(h.presentation()), {}, h.events());
  session.setAssetLoader(*app.loader);
  CameraState camera;
  camera.camera.position = {0, 2, 3};
  camera.camera.target = {0, -1, 0};
  session.setCamera(camera);
  start(session);
  session.selectModel({"floor"});
  REQUIRE(test::until([&] {
    return session.snapshot()->installedModel && h.image &&
           h.image->stamp.generation == session.snapshot()->generation;
  }));
  stop(session, h);
  REQUIRE(h.image);
  CHECK(h.image->image->extent.x == 16);
  CHECK(h.image->image->pixels.size() == 256);
  auto qimage = ReadbackAdapter::copyImage(*h.image->image);
  CHECK(qimage.pixelColor(8, 8).alpha() == 255);
  std::set<std::uint32_t> colors(h.image->image->pixels.begin(),
                                 h.image->image->pixels.end());
  CHECK(colors.size() > 1);
  if (auto path = std::getenv("DEMO_ASSET_IMAGE_PATH"))
    CHECK(qimage.save(QString::fromUtf8(path)));
  std::cout << "CPU unique colors=" << colors.size() << "\n";
  std::cout << "Actual CPU floor installed: images=" << h.images
            << " pixels=" << h.image->image->pixels.size()
            << " alpha=" << qimage.pixelColor(8, 8).alpha() << "\n";
}

TEST_CASE("asset_async/close-mid-install-never-extracts-partial-candidate") {
  App app([](auto const &e, auto) -> Result<CpuAssetPtr> {
    return fixture(e.id.value, 64);
  });
  auto r = resources(app);
  Harness h;
  RenderSession *owner = nullptr;
  bool interrupted = false;
  std::size_t stoppedAt = 0;
  auto events = h.events();
  events.onSnapshot = [&](auto snap) {
    if (snap->assetInstalling && snap->installationParts > 0 && !interrupted) {
      interrupted = true;
      stoppedAt = snap->installationParts;
      owner->requestStop();
    }
  };
  RenderSession session(std::move(r.runtime), std::move(r.renderer),
                        std::move(r.world),
                        std::make_unique<ReadbackAdapter>(h.presentation()),
                        h.fake(), std::move(events));
  owner = &session;
  session.setAssetLoader(*app.loader);
  start(session);
  session.selectModel({"diablo"});
  REQUIRE(test::until([&] { return h.stopped; }));
  CHECK(interrupted);
  CHECK(stoppedAt < 64);
  for (auto const &f : h.frames)
    CHECK(f.delta.instances.empty());
  CHECK(app.loader->inFlight() == 0);
}
TEST_CASE(
    "asset_async/window-observers-detach-while-independent-load-retires") {
  auto gate = std::make_shared<Gate>();
  App app([gate](auto const &e, auto s) { return gate->decode(e, s); });
  auto r = resources(app);
  unsigned destroyed = 0;
  struct Adapter : ReadbackAdapter {
    unsigned *destroyed;
    Adapter(unsigned &d) : ReadbackAdapter({}), destroyed(&d) {}
    ~Adapter() { ++*destroyed; }
  };
  Harness h;
  auto session = std::make_unique<RenderSession>(
      std::move(r.runtime), std::move(r.renderer), std::move(r.world),
      std::make_unique<Adapter>(destroyed), h.fake(), h.events());
  session->setAssetLoader(*app.loader);
  start(*session);
  session->selectModel({"african_head"});
  REQUIRE(test::until([&] { return gate->started == 1; }));
  session.reset();
  CHECK(destroyed == 0);
  gate->release("african_head");
  REQUIRE(test::until(
      [&] { return destroyed == 1 && app.loader->inFlight() == 0; }));
  CHECK_FALSE(h.stopped);
  CHECK(h.errors.empty());
}

TEST_CASE("asset_async/preflight-invalid-candidate-retains-live-old-scene") {
  App app([](auto const &e, auto) -> Result<CpuAssetPtr> {
    auto a = std::make_shared<CpuAsset>(*fixture(e.id.value, 2));
    if (e.id.value == "diablo")
      a->parts[0].material = 99;
    return CpuAssetPtr(a);
  });
  auto r = resources(app);
  Harness h;
  RenderSession session(std::move(r.runtime), std::move(r.renderer),
                        std::move(r.world),
                        std::make_unique<ReadbackAdapter>(h.presentation()),
                        h.fake(), h.events());
  session.setAssetLoader(*app.loader);
  start(session);
  session.selectModel({"african_head"});
  REQUIRE(test::until([&] {
    return session.snapshot()->installedModel && h.image &&
           h.image->stamp.generation == session.snapshot()->generation;
  }));
  auto generation = session.snapshot()->generation;
  session.selectModel({"diablo"});
  REQUIRE(test::until([&] { return !h.errors.empty(); }));
  CHECK(session.snapshot()->installedModel->value == "african_head");
  CHECK(session.snapshot()->life != LifeState::Failed);
  CHECK(session.snapshot()->generation == generation);
  stop(session, h);
}
TEST_CASE("asset_async/cancellation-classification-is-narrow-and-typed") {
  for (unsigned n = 0; n < 3; ++n) {
    App app([n](auto const &, auto) -> Result<CpuAssetPtr> {
      return RenderError{n == 2 ? ErrorStage::Submit : ErrorStage::Load, n != 1,
                         "test", "cancelled"};
    });
    std::optional<LoadOutcome> result;
    app.loader->beginLoad(entry("floor"), 1, [&](auto r) { result = r; });
    REQUIRE(test::until([&] { return result.has_value(); }));
    CHECK(result->status ==
          (n == 0 ? LoadStatus::Cancelled : LoadStatus::Failed));
  }
}

TEST_CASE("asset_async/coalesced-commands-start-only-latest-request") {
  auto gate = std::make_shared<Gate>();
  App app([gate](auto const &e, auto stop) { return gate->decode(e, stop); });
  auto r = resources(app);
  Harness h;
  RenderSession session(std::move(r.runtime), std::move(r.renderer),
                        std::move(r.world),
                        std::make_unique<ReadbackAdapter>(h.presentation()),
                        h.fake(), h.events());
  session.setAssetLoader(*app.loader);
  start(session);
  for (unsigned n = 0; n < 1000; ++n)
    session.selectModel({n % 2 ? "diablo" : "african_head"});
  REQUIRE(test::until([&] { return gate->started == 1; }));
  CHECK(session.snapshot()->request == 1000);
  CHECK(app.loader->inFlight() == 1);
  gate->release("diablo");
  REQUIRE(test::until(
      [&] { return session.snapshot()->installedModel.has_value(); }));
  CHECK(session.snapshot()->installedModel->value == "diablo");
  CHECK(gate->started == 1);
  stop(session, h);
}
TEST_CASE("asset_async/throwing-decoder-has-one-failed-gui-receipt") {
  App app([](auto const &, auto) -> Result<CpuAssetPtr> {
    throw std::runtime_error("original exception");
  });
  unsigned receipts = 0;
  std::optional<LoadOutcome> result;
  app.loader->beginLoad(entry("floor"), 7, [&](auto r) {
    ++receipts;
    result = std::move(r);
  });
  REQUIRE(test::until([&] { return app.loader->inFlight() == 0; }));
  REQUIRE(result);
  CHECK(receipts == 1);
  CHECK(result->status == LoadStatus::Failed);
  CHECK(result->error->cause == "original exception");
}

TEST_CASE(
    "asset_async/commit-shared-handles-releases-only-old-owned-references") {
  auto world = lr::scene::makeWorld({});
  AssetInstall install(*world);
  auto old = fixture("old", 3);
  install.begin(old);
  REQUIRE(install.step(3));
  install.commit();
  world->extractDelta();
  auto shared = std::make_shared<CpuAsset>(*old);
  shared->model = {"new"};
  install.begin(shared);
  CHECK_FALSE(install.step(1));
  CHECK(install.current() == old);
  CHECK(install.step(2));
  install.commit();
  CHECK(install.current()->model.value == "new");
  CHECK(world->entityCount() == 3);
  CHECK(world->resources().counts().meshes == 1);
  CHECK(world->resources().counts().materials == 1);
  CHECK(world->resources().counts().textures == 2);
  auto delta = world->extractDelta();
  CHECK(delta.instances.size() == 3);
  CHECK(delta.removed.instances.size() == 3);
  CHECK(delta.removed.meshes.empty());
  CHECK(delta.removed.textures.empty());
  install.clear();
  CHECK(world->entityCount() == 0);
  CHECK(world->resources().counts().meshes == 0);
  CHECK(world->resources().counts().materials == 0);
  CHECK(world->resources().counts().textures == 0);
}

TEST_CASE("asset_async/p9-material-override-keeps-real-texture-refs") {
  auto world = lr::scene::makeWorld({});
  AssetInstall install(*world);
  install.setRoughness(.8f);
  install.begin(fixture("old", 2));
  REQUIRE(install.step(2));
  install.commit();
  auto delta = world->extractDelta();
  REQUIRE(delta.materials.size() == 1);
  CHECK(delta.materials[0].material.roughness == doctest::Approx(.8));
  CHECK(delta.materials[0].material.normalTexture.has_value());
  install.setRoughness(.3f);
  delta = world->extractDelta();
  REQUIRE(delta.materials.size() == 1);
  CHECK(delta.materials[0].material.roughness == doctest::Approx(.3));
  CHECK(world->resources().counts().textures == 2);
  CHECK_THROWS_AS(install.setRoughness(-1), std::invalid_argument);
  install.clear();
}

TEST_CASE("asset_async/p9-normal-material-command-preserves-texture-reference") {
  auto world = lr::scene::makeWorld({});
  AssetInstall install(*world);
  install.begin(fixture("old", 1));
  REQUIRE(install.step(1));
  install.commit();
  world->extractDelta();
  install.setNormalMapping(false);
  auto delta = world->extractDelta();
  REQUIRE(delta.materials.size() == 1);
  CHECK(delta.materials[0].material.normalScale == 0);
  CHECK(delta.materials[0].material.normalTexture.has_value());
  install.setNormalMapping(true);
  delta = world->extractDelta();
  REQUIRE(delta.materials.size() == 1);
  CHECK(delta.materials[0].material.normalScale == 1);
  install.clear();
}

TEST_CASE("asset_async/p9-texture-upload-gets-one-completion-driven-followup-not-reupload") {
  App app([](auto const &e, auto) -> Result<CpuAssetPtr> { return fixture(e.id.value, 1); });
  auto r = resources(app);
  Harness h;
  RenderSession session(std::move(r.runtime), std::move(r.renderer), std::move(r.world),
                        std::make_unique<ReadbackAdapter>(h.presentation()), h.fake(), h.events());
  session.setAssetLoader(*app.loader);
  start(session);
  session.selectModel({"african_head"});
  bool refreshed = test::until(
      [&] {
        for (std::size_t i = 0; i < h.frames.size(); ++i)
          if (!h.frames[i].delta.textures.empty())
            return h.frames.size() > i + 1;
        return false;
      },
      300);
  CHECK(refreshed);
  unsigned uploads = 0;
  for (auto const &f : h.frames)
    uploads += !f.delta.textures.empty();
  CHECK(uploads == 1);
  stop(session, h);
}
TEST_CASE("asset_async/p9-roughness-command-does-not-reenable-disabled-normal-map") {
  auto world = lr::scene::makeWorld({});
  AssetInstall install(*world);
  install.begin(fixture("old", 1));
  REQUIRE(install.step(1));
  install.commit();
  world->extractDelta();
  install.setNormalMapping(false);
  world->extractDelta();
  install.setRoughness(.7f);
  auto delta = world->extractDelta();
  REQUIRE(delta.materials.size() == 1);
  CHECK(delta.materials[0].material.normalScale == 0);
  CHECK(delta.materials[0].material.roughness == doctest::Approx(.7));
  install.clear();
}

TEST_CASE("asset_async/p9-temporal-history-restarts-once-after-texture-settlement") {
  App app([](auto const &e, auto) -> Result<CpuAssetPtr> { return fixture(e.id.value, 1); });
  auto r = resources(app);
  Harness h;
  RenderSession session(std::move(r.runtime), std::move(r.renderer), std::move(r.world),
                        std::make_unique<ReadbackAdapter>(h.presentation()), h.fake(), h.events());
  session.setAssetLoader(*app.loader);
  session.selectModel({"african_head"});
  REQUIRE(test::until([&] { return session.snapshot()->installedModel.has_value(); }));
  session.setRenderOptions({PipelineKind::Pathtrace, 1, false});
  start(session);
  bool reset = test::until(
      [&] {
        if (h.frames.size() < 2)
          return false;
        return h.frames.back().views[0].target != h.frames.front().views[0].target;
      },
      300);
  CHECK(reset);
  unsigned payloads = 0;
  for (auto const &frame : h.frames)
    payloads += !frame.delta.textures.empty();
  CHECK(payloads == 1);
  stop(session, h);
}

TEST_CASE("asset_async/n3-cancel-mid-install-keeps-editor-entities-editable") {
  App app([](auto const &e, auto) -> Result<CpuAssetPtr> {
    if (e.id.value == "diablo")
      return fixture(e.id.value, 64);
    return fixture(e.id.value, 2);
  });
  auto r = resources(app);
  Harness h;
  RenderSession *owner = nullptr;
  EditorEntityId keep{};
  bool cancelled = false;
  auto events = h.events();
  events.onSnapshot = [&](auto snap) {
    if (cancelled || !owner || !keep.isValid())
      return;
    if (snap->assetInstalling && snap->installationParts > 0) {
      cancelled = true;
      // Cancel mid-staging: rollback + readopt must keep editor-created entities.
      owner->selectModel({"not-in-catalog"});
    }
  };
  RenderSession session(std::move(r.runtime), std::move(r.renderer),
                        std::move(r.world),
                        std::make_unique<ReadbackAdapter>(h.presentation()),
                        h.fake(), std::move(events));
  owner = &session;
  session.setAssetLoader(*app.loader);
  start(session);
  session.selectModel({"african_head"});
  REQUIRE(test::until([&] {
    return session.snapshot()->installedModel && h.image &&
           h.image->stamp.generation == session.snapshot()->generation;
  }));
  // Parent an editor child under installed part0, then cancel mid-install (I1).
  EditorEntityId part0{};
  {
    auto snap = session.snapshot();
    REQUIRE(snap->entities.size() >= 2);
    for (auto const &e : snap->entities)
      if (e.installed && e.name.find("/part0") != std::string::npos)
        part0 = e.id;
    REQUIRE(part0.isValid());
  }
  auto child = session.createEditorEntity("child", {}, part0);
  REQUIRE(child.isValid());
  REQUIRE(test::until([&] {
    auto snap = session.snapshot();
    for (auto const &e : snap->entities)
      if (e.id.value == child.value && e.parent && e.parent->value == part0.value)
        return true;
    return false;
  }));
  keep = session.createEditorEntity("keep", {});
  REQUIRE(keep.isValid());
  {
    auto snap = session.snapshot();
    REQUIRE(snap->entities.size() >= 4);
  }
  session.selectModel({"diablo"});
  REQUIRE(test::until([&] {
    return cancelled && !session.snapshot()->assetInstalling &&
           session.snapshot()->installedModel.has_value() &&
           session.snapshot()->installedModel->value == "african_head";
  }));
  {
    auto snap = session.snapshot();
    bool foundKeep = false;
    bool childRebound = false;
    EditorEntityId newPart0{};
    for (auto const &e : snap->entities) {
      if (e.id.value == keep.value && e.name == "keep")
        foundKeep = true;
      if (e.installed && e.name.find("/part0") != std::string::npos)
        newPart0 = e.id;
    }
    CHECK(foundKeep);
    REQUIRE(newPart0.isValid());
    for (auto const &e : snap->entities)
      if (e.id.value == child.value && e.parent && e.parent->value == newPart0.value)
        childRebound = true;
    CHECK(childRebound);
    // Editor agrees with World: parenting part0 under child is a cycle.
    CHECK_THROWS_AS(session.setEntityParent(newPart0, child), std::invalid_argument);
  }
  lr::core::Transform xf{};
  xf.translation = {1.f, 2.f, 3.f};
  CHECK_NOTHROW(session.setEntityTransform(keep, xf));
  {
    auto snap = session.snapshot();
    bool edited = false;
    for (auto const &e : snap->entities)
      if (e.id.value == keep.value && e.local.translation.x == 1.f)
        edited = true;
    CHECK(edited);
  }
  // After prior sync activity, stop must still complete (C1 guard on this path).
  stop(session, h);
}


TEST_CASE("asset_async/reverse-divergence-installed-under-user") {
  App app([](auto const &e, auto) -> Result<CpuAssetPtr> {
    if (e.id.value == "diablo")
      return fixture(e.id.value, 64);
    return fixture(e.id.value, 2);
  });
  auto r = resources(app);
  Harness h;
  RenderSession *owner = nullptr;
  bool cancelled = false;
  bool armCancel = false; // do not cancel the initial african_head install
  auto events = h.events();
  events.onSnapshot = [&](auto snap) {
    if (cancelled || !owner || !armCancel)
      return;
    if (snap->assetInstalling && snap->installationParts > 0) {
      cancelled = true;
      owner->selectModel({"not-in-catalog"});
    }
  };
  RenderSession session(std::move(r.runtime), std::move(r.renderer),
                        std::move(r.world),
                        std::make_unique<ReadbackAdapter>(h.presentation()),
                        h.fake(), std::move(events));
  owner = &session;
  session.setAssetLoader(*app.loader);
  start(session);
  session.selectModel({"african_head"});
  REQUIRE(test::until([&] {
    return session.snapshot()->installedModel && h.image &&
           h.image->stamp.generation == session.snapshot()->generation;
  }));

  EditorEntityId part0{}, part1{};
  {
    auto snap = session.snapshot();
    REQUIRE(snap->entities.size() >= 2);
    for (auto const &e : snap->entities) {
      if (!e.installed)
        continue;
      if (e.name.find("/part0") != std::string::npos)
        part0 = e.id;
      if (e.name.find("/part1") != std::string::npos)
        part1 = e.id;
    }
    REQUIRE(part0.isValid());
    REQUIRE(part1.isValid());
  }

  lr::core::Transform uxf{};
  uxf.translation = {7.f, 0.f, 0.f};
  auto u = session.createEditorEntity("U", uxf);
  REQUIRE(u.isValid());
  session.setEntityParent(part0, u);
  lr::core::Transform p1xf{};
  p1xf.translation = {5.f, 0.f, 0.f};
  session.setEntityTransform(part1, p1xf);

  REQUIRE(test::until([&] {
    auto snap = session.snapshot();
    bool p0UnderU = false, p1Moved = false;
    for (auto const &e : snap->entities) {
      if (e.id.value == part0.value && e.parent && e.parent->value == u.value)
        p0UnderU = true;
      if (e.id.value == part1.value && e.local.translation.x == 5.f)
        p1Moved = true;
    }
    return p0UnderU && p1Moved;
  }));

  armCancel = true;
  session.selectModel({"diablo"});
  REQUIRE(test::until([&] {
    return cancelled && !session.snapshot()->assetInstalling &&
           session.snapshot()->installedModel.has_value() &&
           session.snapshot()->installedModel->value == "african_head";
  }));

  {
    auto snap = session.snapshot();
    EditorEntityId newP0{}, newP1{};
    bool uOk = false;
    for (auto const &e : snap->entities) {
      if (e.id.value == u.value)
        uOk = true;
      if (e.installed && e.name.find("/part0") != std::string::npos)
        newP0 = e.id;
      if (e.installed && e.name.find("/part1") != std::string::npos)
        newP1 = e.id;
    }
    CHECK(uOk);
    REQUIRE(newP0.isValid());
    REQUIRE(newP1.isValid());
    bool p0ParentU = false, p1At5 = false;
    for (auto const &e : snap->entities) {
      if (e.id.value == newP0.value && e.parent && e.parent->value == u.value)
        p0ParentU = true;
      if (e.id.value == newP1.value && e.local.translation.x == 5.f)
        p1At5 = true;
    }
    CHECK(p0ParentU);
    CHECK(p1At5);
    // Reverse divergence: parenting U under newP0 is a cycle — rejected at edit time.
    CHECK_THROWS_AS(session.setEntityParent(u, newP0), std::invalid_argument);
    CHECK(session.snapshot()->life != LifeState::Failed);
  }
  stop(session, h);
}

#include "app/ApplicationLifetime.h"
TEST_CASE(
    "shutdown/late-real-loader-cancel-receipt-wakes-app-after-session-stop") {
  Gate gate;
  App app([&](auto const &e, auto stop) { return gate.decode(e, stop); });
  unsigned retired = 0, receipt = 0, joins = 0;
  ApplicationLifetime life(
      {{}, [&] { ++retired; }, {}, {}, [&] { ++joins; }, {}});
  life.start();
  struct ClearIdle {
    AssetLoader *loader;
    ~ClearIdle() { loader->setIdleHandler({}); }
  } clear{app.loader.get()};
  app.loader->setIdleHandler(
      [&] { life.onShutdownProgress(false, true, app.loader->inFlight()); });
  auto ticket = app.loader->beginLoad(entry("floor"), 987, [&](auto result) {
    CHECK(result.status == LoadStatus::Cancelled);
    CHECK(app.loader->inFlight() == 1);
    ++receipt;
    life.onShutdownProgress(false, true, app.loader->inFlight());
  });
  REQUIRE(test::until([&] { return gate.started == 1; }));
  CHECK_THROWS_AS(app.loader->assertIdle(), std::logic_error);
  ticket.cancel();
  life.onShutdownProgress(false, true, 1);
  life.requestExit();
  CHECK(retired == 0);
  CHECK(receipt == 0);
  CHECK(joins == 0);
  gate.release("floor");
  REQUIRE(test::until([&] { return joins == 1; }));
  CHECK(receipt == 1);
  CHECK(retired == 1);
  CHECK_NOTHROW(app.loader->assertIdle());
  app.loader->setIdleHandler({});
  life.onRootJoined();
  life.assertSafeDestruction();
}
