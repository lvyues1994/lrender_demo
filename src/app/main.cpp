#include "assets/AssetLoader.h"
#include "assets/AssetRoot.h"
#include "presentation/VulkanSharedAdapter.h"
#include "render/RenderSession.h"
#include "ui/MainWindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCommandLineParser>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLabel>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QScreen>
#include <QTimer>
#include <QWindow>
#include <iostream>
#include <lqtexec/EventLoopContext.h>
#include <optional>
namespace demo {
struct Bootstrap {
  std::unique_ptr<lr::runtime::Runtime> runtime;
  std::unique_ptr<lr::render::Renderer> renderer;
  std::exception_ptr error;
};
struct Viewer : QObject {
  QApplication &app;
  QThreadPool pool;
  lqtexec::EventLoopContext gui;
  lexec::counting_scope root;
  std::unique_ptr<MainWindow> window;
  std::unique_ptr<AssetLoader> loader;
  std::unique_ptr<RenderSession> session;
  VulkanSharedAdapter *presentation = nullptr;
  std::string backend, mode, model, test, screenshot;
  bool loading = true, closing = false, finishing = false;
  int code = 0, stage = 0;
  std::uint64_t beats = 0, lastCompleted = 0, stopBeats = 0;
  qint64 previousBeat = 0, maxGuiGap = 0;
  std::size_t warningCount = 0;
  bool sawSampleOne = false;
  std::uint64_t sampleOneAtCamera = 0;
  SessionGeneration cameraGeneration = 0;
  WId cameraHost = 0;
  QElapsedTimer elapsed, phase;
  QTimer heartbeat, script, statistics;
  std::optional<std::future<lr::render::ImageRgba8>> capture;
  std::vector<std::uint32_t> normalPixels;
  struct Joined {
    using receiver_concept = lexec::receiver_t;
    Viewer *owner;
    void set_value() && noexcept {
      QTimer::singleShot(0, &owner->app, [v = owner] {
        std::cout << "viewer stopped GUI heartbeats=" << v->beats << " elapsedMs=" << v->elapsed.elapsed()
                  << std::endl;
        v->app.exit(v->code);
      });
    }
  };
  using Join = lexec::connect_result_t<decltype(std::declval<lexec::counting_scope &>().join()), Joined>;
  std::unique_ptr<Join> joined;
  Viewer(QApplication &a, std::string b, std::string p, std::string m, std::string t, std::string shot,
         std::filesystem::path assetRoot)
      : app(a), backend(b), mode(p), model(m), test(t), screenshot(shot) {
    pool.setMaxThreadCount(2);
    elapsed.start();
    heartbeat.setInterval(2);
    connect(&heartbeat, &QTimer::timeout, this, [this] {
      ++beats;
      auto now = elapsed.elapsed();
      maxGuiGap = std::max(maxGuiGap, now - previousBeat);
      previousBeat = now;
    });
    if (!test.empty())
      heartbeat.start();
    UiCommands commands;
    commands.normalMapping = [this](bool enabled) {
      if (session)
        session->setNormalMapping(enabled);
      return session ? session->snapshot()->normalMapping : true;
    };
    commands.ambient = [this](float v) {
      if (session)
        session->setAmbient(v);
      return session ? session->snapshot()->ambient : .25f;
    };
    commands.roughness = [this](float v) {
      if (session)
        session->setRoughness(v);
      return session ? session->snapshot()->roughness : .5f;
    };
    commands.camera = [this](auto c) {
      if (session)
        session->setCamera(c);
    };
    commands.options = [this](auto o) {
      if (session)
        session->setRenderOptions(o);
    };
    commands.model = [this](auto m) {
      if (session)
        session->selectModel(m);
    };
    commands.viewport = [this](auto e, bool visible) {
      if (session) {
        session->setViewport(e);
        session->setVisible(visible);
      }
    };
    commands.invalidated = [this](auto e) {
      if (session)
        session->invalidatePresentation(e);
    };
    commands.stop = [this] { requestStop(); };
    window = std::make_unique<MainWindow>(commands);
    if (!test.empty())
      window->viewport().setFixedSize(160, 120);
    window->show();
    loader = std::make_unique<AssetLoader>(&pool, gui.get_scheduler(), root.get_token(),
                                           [assetRoot](auto const &entry, auto stop) {
                                             return decodeAssetAtRoot(entry, assetRoot, std::move(stop));
                                           });
    std::cout << "asset root=" << assetRoot << std::endl;
    auto box = std::make_shared<Bootstrap>();
    auto work = lexec::schedule(lqtexec::ThreadPoolScheduler{&pool}) | lexec::then([box, b]() noexcept {
                  try {
                    lr::runtime::RuntimeConfig config;
                    config.framePoolThreads = 2;
                    config.backgroundPoolThreads = 1;
                    config.ioWorkers = 1;
                    box->runtime = lr::runtime::makeRuntime(config);
                    lr::render::RendererConfig renderer;
                    renderer.backend = b;
                    renderer.runtime = box->runtime.get();
                    renderer.framesInFlight = 2;
                    renderer.textureConversions = lr::render::ConversionDelivery::Deterministic;
                    // Known catalog: <=6 bound images at <=2048 squared. 128MiB
                    // fits the whole bounded batch; extra frame settles
                    // conversions.
                    renderer.textureStreamingBytes = std::size_t{128} << 20;
                    box->renderer = lr::render::makeRenderer(renderer);
                  } catch (...) {
                    box->error = std::current_exception();
                    box->renderer.reset();
                    box->runtime.reset();
                  }
                });
    lexec::spawn(lqtexec::then_on(std::move(work), *completionScope(),
                                  [this, box]() noexcept {
                                    try {
                                      initialized(box);
                                    } catch (std::exception const &e) {
                                      std::cerr << "startup failed " << e.what() << std::endl;
                                      code = 1;
                                      auto retire = lexec::schedule(lqtexec::ThreadPoolScheduler{&pool}) |
                                                    lexec::then([box]() noexcept {
                                                      box->renderer.reset();
                                                      box->runtime.reset();
                                                    });
                                      lexec::spawn(lqtexec::then_on(std::move(retire), *completionScope(),
                                                                    [this]() noexcept { requestStop(); }),
                                                   root.get_token());
                                    }
                                  }),
                 root.get_token());
    statistics.setInterval(500);
    connect(&statistics, &QTimer::timeout, this, [this] {
      if (session && window)
        window->showSnapshot(session->snapshot());
    });
    statistics.start();
    script.setInterval(20);
    connect(&script, &QTimer::timeout, this, [this] {
      if (elapsed.elapsed() > 45000) {
        std::cerr << "deadline state=" << stage << " closing=" << closing
                  << " (resources retained; external test deadline required)" << std::endl;
        code = 1;
        requestStop();
        script.stop();
        return;
      }
      if (!test.empty() && !closing)
        try {
          selfTest();
        } catch (std::exception const &e) {
          std::cerr << "automation failure: " << e.what() << std::endl;
          code = 1;
          requestStop();
        }
    });
    if (!test.empty())
      script.start();
  }
  std::unique_ptr<lqtexec::ObjectScope> applicationScope;
  lqtexec::ObjectScope *completionScope() {
    if (!applicationScope)
      applicationScope = std::make_unique<lqtexec::ObjectScope>(this, gui.get_scheduler(), root.get_token());
    return applicationScope.get();
  }
  void initialized(std::shared_ptr<Bootstrap> box) {
    loading = false;
    if (box->error) {
      try {
        std::rethrow_exception(box->error);
      } catch (std::exception const &e) {
        std::cerr << "renderer initialization failed: " << e.what() << std::endl;
      }
      code = 1;
      finish();
      return;
    }
    auto world = lr::scene::makeWorld({});
    world->setEnvironment({lr::core::vec3{.25f}});
    auto sun = world->createEntity(lr::core::Transform::identity(), std::nullopt);
    lr::core::DirectionalLight light;
    light.direction = {-.5f, -1, -1};
    light.illuminance = 2;
    world->setLight(sun, light);
    auto info = box->renderer->deviceInfo();
    PresentationEvents images{[this](OwnedFrame f) {
                                if (window && !closing)
                                  window->viewport().showFrame(std::move(f));
                              },
                              [this](RenderError e) {
                                std::cerr << "presentation error: " << e.message << ": " << e.cause
                                          << std::endl;
                              }};
    auto adapter = std::make_unique<VulkanSharedAdapter>(
        info, gui.get_scheduler(), root.get_token(),
        [this] { return window->viewport().createNativeLease(); },
        [this] { window->viewport().retireNative(); }, images);
    presentation = adapter.get();
    SessionEvents events;
    events.onSnapshot = [this](auto s) {
      if (s->warnings.size() > warningCount) {
        warningCount = s->warnings.size();
        std::cout << "session warning=" << s->warnings.back() << std::endl;
      }
      if (window)
        window->showSnapshot(s);
      if (stage == 3 && s->generation >= cameraGeneration &&
          s->performance.sampleOneFrames > sampleOneAtCamera)
        sawSampleOne = true;
      if (test == "close-install" && s->assetInstalling && s->installationParts > 0) {
        std::cout << "close during installation parts=" << s->installationParts << std::endl;
        requestStop();
      }
      if (test == "close-load" && s->assetLoadsInFlight > 0) {
        std::cout << "close during actual load count=" << s->assetLoadsInFlight << std::endl;
        requestStop();
      }
    };
    events.onError = [this](auto e) {
      std::cerr << "session error: " << e.message << ": " << e.cause << std::endl;
      if (session && session->snapshot()->life == LifeState::Failed) {
        code = 1;
        requestStop();
      }
    };
    events.onStopped = [this] { finish(); };
    session = std::make_unique<RenderSession>(std::move(box->runtime), std::move(box->renderer),
                                              std::move(world), std::move(adapter), SubmitFrame{}, events);
    session->setAssetLoader(*loader);
    session->setPresentationMode(
        mode == "readback" ? PresentationPath::Readback : PresentationPath::VulkanShared, mode == "auto");
    CameraState camera;
    camera.camera.position = {0, 0, 3};
    session->setCamera(camera);
    auto &v = window->viewport();
    auto d = v.devicePixelRatioF();
    session->setViewport({{v.width(), v.height()}, {qRound(v.width() * d), qRound(v.height() * d)}, d});
    session->setVisible(v.isVisible());
    session->start();
    session->selectModel({model});
    std::cout << "viewer backend=" << info.backend << " device=" << info.deviceName << " DPR=" << d
              << " Qt=" << qVersion() << std::endl;
    if (closing)
      session->requestStop();
  }
  void requestStop() {
    if (closing)
      return;
    closing = true;
    stopBeats = beats;
    script.stop();
    if (session)
      session->requestStop();
    else if (!loading)
      finish();
  }
  void finish() {
    if (finishing)
      return;
    finishing = true;
    statistics.stop();
    closing = true;
    script.stop();
    if (session) {
      auto s = session->snapshot();
      std::cout << "final completed=" << s->performance.completed << " shown=" << s->performance.displayed
                << " sharedShown=" << s->performance.sharedShown << " inFlight=" << s->performance.inFlight
                << std::endl;
      auto stopped = std::make_shared<SessionSnapshot>(*s);
      stopped->life = LifeState::Stopped;
      window->showSnapshot(stopped);
      session.reset();
    }
    std::cout << "GUI cleanup ticks=" << beats - stopBeats << " maxGapMs=" << maxGuiGap << std::endl;
    if (maxGuiGap > 500)
      code = 1;
    presentation = nullptr;
    if (loader && loader->inFlight() != 0) {
      std::cerr << "ERROR loader still active at onStopped" << std::endl;
      code = 1;
      return;
    }
    loader.reset();
    if (window) {
      auto s = std::make_shared<SessionSnapshot>();
      s->life = LifeState::Stopped;
      window->showSnapshot(s);
      window->close();
    }
    applicationScope.reset();
    root.close();
    joined = std::unique_ptr<Join>(new Join(lexec::connect(root.join(), Joined{this})));
    lexec::start(*joined);
  }
  void selfTest() {
    if (!session)
      return;
    auto s = session->snapshot();
    if (s->life == LifeState::Failed) {
      code = 1;
      requestStop();
      return;
    }
    if (!s->installedModel)
      return;
    if (s->life != LifeState::Running && stage != 6 && stage != 7)
      return;
    auto completed = s->performance.completed;
    switch (stage) {
    case 0:
      if (completed == 0 || (s->path == PresentationPath::VulkanShared && s->performance.sharedShown == 0))
        return;
      std::cout << "installed=" << s->installedModel->value << " sharedShown=" << s->performance.sharedShown
                << " initial generation=" << s->generation << std::endl;
      lastCompleted = completed;
      if (auto spin = window->findChild<QDoubleSpinBox *>("ambient"))
        spin->setValue(.3);
      if (auto spin = window->findChild<QDoubleSpinBox *>("roughness"))
        spin->setValue(.65);
      {
        QMouseEvent down(QEvent::MouseButtonPress, {20, 20}, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window->viewport(), &down);
        QMouseEvent move(QEvent::MouseMove, {40, 30}, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window->viewport(), &move);
      }
      stage = 1;
      return;
    case 1:
      if (completed <= lastCompleted || s->performance.inFlight)
        return;
      if (s->path == PresentationPath::VulkanShared) {
        capture.emplace(presentation->captureNext());
        session->setCamera(s->camera);
        stage = 12;
        return;
      }
      window->viewport().setFixedSize(209, 145);
      session->setRenderOptions({PipelineKind::Pathtrace, 1, false});
      stage = 2;
      return;
    case 12:
      if (!capture || capture->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
      {
        auto image = capture->get();
        capture.reset();
        if (!screenshot.empty())
          ReadbackAdapter::copyImage(image).save(QString::fromStdString(screenshot + "-presented.png"));
        std::cout << "stable actual presented capture=" << image.extent.x << "x" << image.extent.y
                  << std::endl;
        if (model == "african_head" || model == "diablo" || model == "boggie") {
          normalPixels = image.pixels;
          capture.emplace(presentation->captureNext());
          window->findChild<QCheckBox *>("normal")->setChecked(false);
          stage = 11;
          return;
        }
      }
      window->viewport().setFixedSize(209, 145);
      session->setRenderOptions({PipelineKind::Pathtrace, 1, false});
      stage = 2;
      return;
    case 2:
      if (s->performance.accumulatedSamples < 2)
        return;
      {
        auto c = s->camera;
        c.camera.position.x += .1f;
        cameraGeneration = s->generation;
        cameraHost = window->viewport().nativeWindow() ? window->viewport().nativeWindow()->winId() : 0;
        session->setCamera(c);
        if(session->snapshot()->generation!=cameraGeneration)throw std::runtime_error("camera changed presentation generation");
        sampleOneAtCamera = session->snapshot()->performance.sampleOneFrames;
        phase.restart();
      }
      sawSampleOne = false;
      stage = 3;
      return;
    case 3:
      if (!sawSampleOne || s->performance.accumulatedSamples < 2)
        return;
      if (s->generation != cameraGeneration ||
          (cameraHost &&
           (!window->viewport().nativeWindow() || window->viewport().nativeWindow()->winId() != cameraHost)))
        throw std::runtime_error("PT camera unexpectedly rebuilt presentation");
      std::cout << "PT camera reuse host=" << cameraHost << " generation=" << cameraGeneration << std::endl;
      std::cout << "pathtrace camera sampleOne=" << sawSampleOne << " transitionMs=" << phase.elapsed()
                << " completed=" << completed << std::endl;
      session->setRenderOptions({PipelineKind::Visibility, 1, true});
      lastCompleted = completed;
      stage = 4;
      return;
    case 4:
      if (completed < lastCompleted + 3)
        return;
      {
        auto c = s->camera;
        c.camera.position.x -= .1f;
        cameraGeneration = s->generation;
        cameraHost = window->viewport().nativeWindow() ? window->viewport().nativeWindow()->winId() : 0;
        session->setCamera(c);
      }
      lastCompleted = completed;
      phase.restart();
      stage = 5;
      return;
    case 5:
      if (completed < lastCompleted + 2)
        return;
      if (s->generation != cameraGeneration ||
          (cameraHost &&
           (!window->viewport().nativeWindow() || window->viewport().nativeWindow()->winId() != cameraHost)))
        throw std::runtime_error("TAA camera unexpectedly rebuilt presentation");
      std::cout << "TAA camera reuse host=" << cameraHost << " generation=" << cameraGeneration << std::endl;
      std::cout << "TAA camera transitionMs=" << phase.elapsed() << std::endl;
      session->setRenderOptions({PipelineKind::Visibility, 1, false});
      window->viewport().hide();
      phase.start();
      lastCompleted = completed;
      stage = 6;
      return;
    case 6:
      if (s->performance.inFlight || phase.elapsed() < 100)
        return;
      std::cout << "hidden stopped admission completed=" << completed
                << " delta=" << completed - lastCompleted << std::endl;
      window->viewport().setFixedSize(0, 0);
      window->viewport().show();
      phase.restart();
      lastCompleted = completed;
      stage = 7;
      return;
    case 7:
      if (phase.elapsed() < 100)
        return;
      if (completed != lastCompleted) {
        std::cerr << "zero extent submitted frames" << std::endl;
        code = 1;
        requestStop();
        return;
      }
      window->viewport().setFixedSize(160, 120);
      session->selectModel({"african_head"});
      session->selectModel({"floor"});
      session->selectModel({"diablo"});
      stage = 8;
      return;
    case 8:
      if (s->installedModel->value != "diablo" || !s->model || s->model->value != "diablo")
        return;
      lastCompleted = completed;
      session->setCamera(s->camera); // force one bounded new receipt even if dirty work already settled
      stage = 13;
      return;
    case 13:
      if (s->life != LifeState::Running || s->performance.inFlight || completed <= lastCompleted)
        return;
      if (s->path == PresentationPath::VulkanShared) {
        capture.emplace(presentation->captureNext());
        session->setCamera(s->camera);
        stage = 14;
        return;
      }
      [[fallthrough]];
    case 14:
      if (capture) {
        if (capture->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
          return;
        auto image = capture->get();
        capture.reset();
        if (!screenshot.empty())
          ReadbackAdapter::copyImage(image).save(QString::fromStdString(screenshot + "-latest-diablo.png"));
        std::cout << "actual latest diablo presented capture=" << image.extent.x << "x" << image.extent.y
                  << std::endl;
      }
      if (!screenshot.empty()) {
        window->grab().save(QString::fromStdString(screenshot + "-widgets.png"));
        // QWidget::grab excludes native children. Public Qt capture of this
        // application's own XID, not a desktop/browser automation surface.
        if (auto own = window->viewport().nativeWindow())
          own->screen()
              ->grabWindow(own->winId())
              .save(QString::fromStdString(screenshot + "-native-window.png"));
      }
      std::cout << "performance UI: " << window->findChild<QLabel *>("performance")->text().toStdString()
                << std::endl;
      std::cout << "scenario full latest installed=" << s->installedModel->value
                << " sharedShown=" << s->performance.sharedShown << " GUI ticks=" << beats
                << " DPR=" << window->viewport().devicePixelRatioF() << std::endl;
      if (mode == "shared" && s->performance.sharedShown == 0)
        code = 1;
      if (s->path == PresentationPath::VulkanShared) {
        auto native = window->viewport().nativeWindow();
        if (!native)
          throw std::runtime_error("missing native window");
        cameraGeneration = s->generation;
        lastCompleted = completed;
        QPlatformSurfaceEvent invalid(QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
        QApplication::sendEvent(native, &invalid);
        stage = 10;
        return;
      }
      requestStop();
      return;
    case 11:
      if (!capture || capture->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        return;
      {
        auto image = capture->get();
        capture.reset();
        if (image.pixels.size() != normalPixels.size())
          throw std::runtime_error("normal comparison extent changed");
        std::size_t changed = 0;
        for (std::size_t i = 0; i < normalPixels.size(); ++i)
          changed += image.pixels[i] != normalPixels[i];
        std::cout << "actual normal-map on/off changedPixels=" << changed << " / " << normalPixels.size()
                  << std::endl;
        if (!changed)
          throw std::runtime_error("normal-map shader had no observable effect");
        if (!screenshot.empty())
          ReadbackAdapter::copyImage(image).save(QString::fromStdString(screenshot + "-normal-off.png"));
        window->findChild<QCheckBox *>("normal")->setChecked(true);
      }
      window->viewport().setFixedSize(209, 145);
      session->setRenderOptions({PipelineKind::Pathtrace, 1, false});
      stage = 2;
      return;
    case 10:
      if (s->generation <= cameraGeneration || completed <= lastCompleted)
        return;
      std::cout << "Platform event invalidation rebuilt generation=" << s->generation
                << " prior=" << cameraGeneration << std::endl;
      requestStop();
      return;
    }
  }
};
} // namespace demo
int main(int argc, char **argv) {
  QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  QCommandLineParser args;
  args.addHelpOption();
  args.addOption({"backend", "Renderer backend (cpu only when explicit)", "backend", "vulkan"});
  args.addOption({"presentation", "auto|shared|readback", "presentation", "auto"});
  args.addOption({"asset-root", "TinyRenderer resource root (contains LICENSE.txt and obj)", "path"});
  args.addOption({"model", "Catalog model", "model", "african_head"});
  args.addOption({"self-test", "P9 bounded interactions: full|close-load|close-install", "test"});
  args.addOption({"screenshot", "Local PNG output prefix", "path"});
  args.process(app);
  auto backend = args.value("backend").toStdString(), mode = args.value("presentation").toStdString();
  if ((backend != "cpu" && backend != "vulkan") ||
      (mode != "auto" && mode != "shared" && mode != "readback")) {
    std::cerr << "invalid backend/presentation" << std::endl;
    return 1;
  }
  std::filesystem::path assetRoot;
  try {
    assetRoot = demo::validateAssetRoot(args.isSet("asset-root")
                                            ? std::filesystem::path(args.value("asset-root").toStdString())
                                            : demo::defaultAssetRoot());
  } catch (std::exception const &e) {
    std::cerr << "asset root error: " << e.what() << std::endl;
    return 1;
  }
  demo::Viewer viewer(app, backend, mode, args.value("model").toStdString(),
                      args.value("self-test").toStdString(), args.value("screenshot").toStdString(),
                      assetRoot);
  return app.exec();
}
