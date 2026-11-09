#include "app/ApplicationLifetime.h"
#include "assets/AssetLoader.h"
#ifdef DEMO_E2E
#include "e2e_scenarios.h"
#endif
#include "assets/AssetRoot.h"
#include "presentation/VulkanSharedAdapter.h"
#include "render/RenderSession.h"
#include "ui/MainWindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QColor>
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
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <lqtexec/EventLoopContext.h>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
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
  bool loading = true, closing = false, sessionStopped = false,
       desiredCloseObserved = false;
  std::unique_ptr<ApplicationLifetime> lifetime;
#ifdef DEMO_E2E
  E2eScenario scenario;
  bool injectZeroShared = false;
#endif
  QTimer waiting;
  struct DecodeGate {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> entered{false};
    bool open = false;
    void release() {
      std::lock_guard<std::mutex> lock(mutex);
      open = true;
      cv.notify_all();
    }
  } decodeGate;
  std::optional<LoadTicket> extraLoad;
  std::uint64_t releaseDecodeAt = 0;
  std::atomic<unsigned> decodeCheckpoints{0};

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
  struct BlackFlashOp {
    char const *name = "";
    std::uint64_t blackSamples = 0, samples = 0, hostChanges = 0;
    std::uint64_t sharedShownAtStart = 0, sharedShownGap = 0;
    qint64 blackMs = 0, latencyMs = 0;
    WId host = 0;
    QElapsedTimer timer;
    bool armed = false, seenContent = false;
    int expectW = 0, expectH = 0;
  } flash;
  QImage sampleViewport() const {
    if (auto native = window->viewport().nativeWindow()) {
      if (auto screen = native->screen())
        return screen->grabWindow(native->winId()).toImage();
    }
    return window->viewport().readbackImage();
  }
  static bool nearBlack(QImage img) {
    if (img.isNull() || img.width() < 2 || img.height() < 2)
      return true;
    img = img.convertToFormat(QImage::Format_RGB32);
    int x0 = img.width() / 4, y0 = img.height() / 4, x1 = img.width() - x0, y1 = img.height() - y0;
    double sum = 0;
    int count = 0, bright = 0;
    for (int y = y0; y < y1; y += 2)
      for (int x = x0; x < x1; x += 2) {
        auto p = img.pixel(x, y);
        int r = qRed(p), g = qGreen(p), b = qBlue(p);
        int luma = (r + g + b) / 3;
        sum += luma;
        ++count;
        if (luma > 24)
          ++bright;
      }
    return count == 0 || (bright == 0 && sum / count < 8);
  }
  void sampleFlash() {
    if (!flash.armed || !window)
      return;
    ++flash.samples;
    auto const shared =
        session && session->snapshot()->path == PresentationPath::VulkanShared;
    if (shared) {
      auto shown = session->snapshot()->performance.sharedShown;
      if (shown < flash.sharedShownAtStart)
        ++flash.sharedShownGap;
      // Keep the high-water for gap detection without treating monotonic growth as failure.
      flash.sharedShownAtStart = std::max(flash.sharedShownAtStart, shown);
    }
    auto native = window->viewport().nativeWindow();
    auto host = native ? native->winId() : 0;
    if (flash.host && host && host != flash.host)
      ++flash.hostChanges;
    if (shared && !host) {
      // Shared path with no live native host is exactly the black-hole window gap.
      ++flash.blackSamples;
      flash.blackMs = flash.timer.elapsed();
      return;
    }
    if (flash.host == 0 && host)
      flash.host = host;
    // Readback has no native host; sample the owned widget image instead of grabWindow.
    auto img = shared ? (native && native->screen() ? native->screen()->grabWindow(host).toImage()
                                                    : QImage{})
                      : sampleViewport();
    if (nearBlack(img)) {
      ++flash.blackSamples;
      flash.blackMs = flash.timer.elapsed();
    } else {
      flash.seenContent = true;
      if (host)
        flash.host = host;
    }
  }
  void beginFlash(char const *name, int expectW = 0, int expectH = 0) {
    flash = {};
    flash.name = name;
    flash.armed = true;
    flash.expectW = expectW;
    flash.expectH = expectH;
    flash.host = window->viewport().nativeWindow() ? window->viewport().nativeWindow()->winId() : 0;
    if (session)
      flash.sharedShownAtStart = session->snapshot()->performance.sharedShown;
    flash.timer.start();
  }
  void endFlash() {
    flash.armed = false;
    flash.latencyMs = flash.timer.elapsed();
    std::cout << "black-flash op=" << flash.name << " blackSamples=" << flash.blackSamples
              << " blackMs=" << flash.blackMs << " samples=" << flash.samples
              << " latencyMs=" << flash.latencyMs << " hostChanges=" << flash.hostChanges
              << " sharedShownGap=" << flash.sharedShownGap << " host=" << flash.host
              << std::endl;
    if (flash.blackSamples || flash.hostChanges || flash.sharedShownGap) {
      code = 1;
      std::cerr << "black-flash FAIL op=" << flash.name << std::endl;
    }
  }

  struct Joined {
    using receiver_concept = lexec::receiver_t;
    Viewer *owner;
    void set_value() && noexcept {
      QTimer::singleShot(0, &owner->app, [v = owner] { v->rootJoined(); });
    }
  };
  using Join = lexec::connect_result_t<
      decltype(std::declval<lexec::counting_scope &>().join()), Joined>;
  std::unique_ptr<Join> joined;
  Viewer(QApplication &a, std::string b, std::string p, std::string m,
         std::string t, std::string shot, std::filesystem::path assetRoot)
      : app(a), backend(b), mode(p), model(m), test(t), screenshot(shot) {
    pool.setMaxThreadCount(2);
    elapsed.start();
    heartbeat.setInterval(2);
    connect(&heartbeat, &QTimer::timeout, this, [this] {
      ++beats;
      if (releaseDecodeAt && beats >= releaseDecodeAt) {
        releaseDecodeAt = 0;
        decodeGate.release();
      }
      sampleFlash();
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
    commands.undo = [&] {
      if (session)
        session->undo();
    };
    commands.redo = [&] {
      if (session)
        session->redo();
    };
    commands.canUndo = [&] {
      return session && session->canUndo();
    };
    commands.canRedo = [&] {
      return session && session->canRedo();
    };
    commands.setEntityParent = [this](auto id, auto parent) {
      if (session)
        session->setEntityParent(id, parent);
    };
    commands.setEntityTransform = [this](auto id, auto xf) {
      if (session)
        session->setEntityTransform(id, xf);
    };
    commands.setEntityMaterialDesc = [this](auto id, auto d) {
      if (session)
        session->setEntityMaterialDesc(id, d);
    };
    commands.setEntityLight = [this](auto id, auto l) {
      if (session)
        session->setEntityLight(id, l);
    };
    commands.clearEntityLight = [this](auto id) {
      if (session)
        session->clearEntityLight(id);
    };
    commands.setEntityCamera = [this](auto id, auto lens) {
      if (session)
        session->setEntityCamera(id, lens);
    };
    commands.clearEntityCamera = [this](auto id) {
      if (session)
        session->clearEntityCamera(id);
    };
    commands.beginEditMerge = [this]() {
      if (session)
        session->beginEditMerge();
    };
    commands.endEditMerge = [this]() {
      if (session)
        session->endEditMerge();
    };
    commands.stop = [this] { requestStop(); };
    window = std::make_unique<MainWindow>(commands);
    if (!test.empty())
      window->viewport().setFixedSize(160, 120);
    window->show();
    loader = std::make_unique<AssetLoader>(
        &pool, gui.get_scheduler(), root.get_token(),
        [assetRoot, this](auto const &entry, auto stop) {
          if (test == "injected-load-fault")
            throw std::runtime_error(
                "injected decode failure (not missing original resource)");
          if (test == "close-late-loader") {
            std::unique_lock<std::mutex> lock(decodeGate.mutex);
            decodeGate.entered = true;
            if (!decodeGate.cv.wait_for(lock, std::chrono::seconds(5),
                                        [this] { return decodeGate.open; }))
              return Result<CpuAssetPtr>{RenderError{
                  ErrorStage::Load, false, "decode test gate timeout",
                  "bounded fixture, not forced cancellation"}};
          }
          if (test == "close-decode") {
            auto checkpoint = [this, stop] {
              if (++decodeCheckpoints == 8) {
                std::unique_lock<std::mutex> lock(decodeGate.mutex);
                decodeGate.entered = true;
                if (!decodeGate.cv.wait_for(lock, std::chrono::seconds(5),
                                            [this] { return decodeGate.open; }))
                  throw std::runtime_error(
                      "actual parser checkpoint gate timeout");
                std::cout << "actual CPU read/parse checkpoint="
                          << decodeCheckpoints.load() << " cancelled=" << stop()
                          << std::endl;
              }
              return stop();
            };
            return decodeAssetAtRoot(entry, assetRoot, checkpoint);
          }
          return decodeAssetAtRoot(entry, assetRoot, std::move(stop));
        });
    lifetime =
        std::make_unique<ApplicationLifetime>(ApplicationLifetime::Hooks{
            [this] {
              closing = true;
              script.stop();
              stopBeats = beats;
              if (!waiting.isActive())
                waiting.start();
              if (extraLoad)
                extraLoad->cancel();
              if (test == "close-decode")
                releaseDecodeAt = beats + 5;
              if (session)
                session->requestStop();
            },
            [this] {
              if (session) {
                auto s = session->snapshot();
                if (s->life != LifeState::Stopped)
                  qFatal("Session retirement before onStopped");
                std::cout << "final completed=" << s->performance.completed
                          << " shown=" << s->performance.displayed
                          << " sharedShown=" << s->performance.sharedShown
                          << " inFlight=" << s->performance.inFlight
                          << std::endl;
                if (test.rfind("close-", 0) == 0 && !desiredCloseObserved) {
                  code = 1;
                  std::cerr << "requested close phase was NOT observed"
                            << std::endl;
                }
#ifdef DEMO_E2E
                if (test.rfind("e2e-", 0) == 0 && !scenario.passed) {
                  code = 1;
                  std::cerr << "e2e receipt/scenario goal NOT reached"
                            << std::endl;
                }
#endif
                if (test == "close-decode" && decodeCheckpoints.load() < 8) {
                  code = 1;
                  std::cerr
                      << "decode closure did not reach actual parser checkpoint"
                      << std::endl;
                }
                auto observedShared = s->performance.sharedShown;
                bool required = mode == "shared" &&
                                (test == "full" || test.rfind("e2e-", 0) == 0);
#ifdef DEMO_E2E
                if (injectZeroShared) {
                  required = true;
                  observedShared = 0;
                  std::cerr << "injected acceptance sharedShown=0 actual="
                            << s->performance.sharedShown << std::endl;
                }
#endif
                if (!ApplicationLifetime::acceptsSharedEvidence(
                        required, observedShared)) {
                  code = 1;
                  std::cerr << "strict shared acceptance failed: zero observed "
                               "sharedShown"
                            << std::endl;
                }
                window->showSnapshot(s);
                session.reset();
                presentation = nullptr;
              }
              std::cout << "GUI cleanup ticks=" << beats - stopBeats
                        << " maxGapMs=" << maxGuiGap << std::endl;
              if (maxGuiGap > 500)
                code = 1;
            },
            [this] {
              if (loader) {
                loader->assertIdle();
                loader->setIdleHandler({});
                loader.reset();
              }
            },
            [this] {
              statistics.stop();
              auto s = std::make_shared<SessionSnapshot>();
              s->life = LifeState::Stopped;
              window->showSnapshot(s);
              window->close();
            },
            [this] {
              applicationScope.reset();
              root.close();
              joined = std::unique_ptr<Join>(
                  new Join(lexec::connect(root.join(), Joined{this})));
              lexec::start(*joined);
            },
            [] {}});
    lifetime->start();
    loader->setIdleHandler([this] {
      lifetime->onShutdownProgress(loading, sessionStopped, loader->inFlight());
    });
    waiting.setInterval(1000);
    connect(&waiting, &QTimer::timeout, this, [this] {
      std::cerr << "shutdown waiting: sessionStopped=" << sessionStopped
                << " bootstrap=" << loading
                << " loads=" << (loader ? loader->inFlight() : 0)
                << " GUI heartbeats=" << beats
                << " (resources retained; no forced GPU destruction)"
                << std::endl;
    });
    std::cout << "asset root=" << assetRoot << std::endl;
    auto box = std::make_shared<Bootstrap>();
    auto work = lexec::schedule(lqtexec::ThreadPoolScheduler{&pool}) |
                lexec::then([box, b]() noexcept {
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
                    renderer.textureConversions =
                        lr::render::ConversionDelivery::Deterministic;
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
    lexec::spawn(
        lqtexec::then_on(
            std::move(work), *completionScope(),
            [this, box]() noexcept {
              try {
                initialized(box);
              } catch (std::exception const &e) {
                std::cerr << "startup failed " << e.what() << std::endl;
                code = 1;
                auto retire =
                    lexec::schedule(lqtexec::ThreadPoolScheduler{&pool}) |
                    lexec::then([box]() noexcept {
                      box->renderer.reset();
                      box->runtime.reset();
                    });
                lexec::spawn(lqtexec::then_on(std::move(retire),
                                              *completionScope(),
                                              [this]() noexcept {
                                                loading = false;
                                                requestStop();
                                              }),
                             root.get_token());
              }
            }),
        root.get_token());
    statistics.setInterval(500);
    connect(&statistics, &QTimer::timeout, this, [this] {
      if (!closing && session && window)
        window->showSnapshot(session->snapshot());
    });
    statistics.start();
    script.setInterval(20);
    connect(&script, &QTimer::timeout, this, [this] {
      if (elapsed.elapsed() > (test.rfind("e2e-", 0) == 0 ? 120000 : 45000)) {
        std::cerr << "deadline state=" << stage << " closing=" << closing
                  << " (resources retained; external test deadline required)"
                  << std::endl;
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
      applicationScope = std::make_unique<lqtexec::ObjectScope>(
          this, gui.get_scheduler(), root.get_token());
    return applicationScope.get();
  }
  void initialized(std::shared_ptr<Bootstrap> box) {
    loading = false;
    if (box->error) {
      try {
        std::rethrow_exception(box->error);
      } catch (std::exception const &e) {
        std::cerr << "renderer initialization failed: " << e.what()
                  << std::endl;
      }
      code = 1;
      finish();
      return;
    }
    auto world = lr::scene::makeWorld({});
    world->setEnvironment({lr::core::vec3{.25f}});
    auto sun =
        world->createEntity(lr::core::Transform::identity(), std::nullopt);
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
                                std::cerr << "presentation error: " << e.message
                                          << ": " << e.cause << std::endl;
                              }};
    auto sharedAdapter = std::make_unique<VulkanSharedAdapter>(
        info, gui.get_scheduler(), root.get_token(),
        [this] { return window->viewport().createNativeLease(); },
        [this] { window->viewport().retireNative(); }, images);
    presentation = sharedAdapter.get();
    std::unique_ptr<PresentationAdapter> adapter = std::move(sharedAdapter);
    if (test == "injected-present-fault") {
      presentation = nullptr;
      adapter = std::make_unique<ReadbackAdapter>(images, [](std::string stage,
                                                             Done done) {
        QTimer::singleShot(
            0, QCoreApplication::instance(),
            [stage, done = std::move(done)]() mutable {
              if (stage == "prepare")
                done(RenderError{ErrorStage::Surface, false,
                                 "injected present boundary failure",
                                 "simulated platform, not GPU device loss"});
              else
                done(std::monostate{});
            });
      });
    }
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
      if (test == "close-install" && s->assetInstalling &&
          s->installationParts > 0) {
        desiredCloseObserved = true;
        std::cout << "close during installation parts=" << s->installationParts
                  << std::endl;
        requestStop();
      }
      if (test == "close-submit" && s->performance.inFlight > 0) {
        desiredCloseObserved = true;
        std::cout << "close actual submit inFlight=" << s->performance.inFlight
                  << std::endl;
        requestStop();
      }
      if (test == "close-prepare" && s->life == LifeState::Preparing) {
        desiredCloseObserved = true;
        std::cout << "close during prepare" << std::endl;
        requestStop();
      }
      if (test == "close-load" && s->assetLoadsInFlight > 0) {
        desiredCloseObserved = true;
        std::cout << "close during actual load count=" << s->assetLoadsInFlight
                  << std::endl;
        requestStop();
      }
    };
    events.onError = [this](auto e) {
      std::cerr << "session error: " << e.message << ": " << e.cause
                << std::endl;
      if ((test == "injected-load-fault") ||
          (session && session->snapshot()->life == LifeState::Failed)) {
        code = 1;
        requestStop();
      }
    };
    events.onStopped = [this] { finish(); };
    session = std::make_unique<RenderSession>(
        std::move(box->runtime), std::move(box->renderer), std::move(world),
        std::move(adapter),
        test == "injected-submit-fault" ? SubmitFrame{[](auto, auto stamp,
                                                         auto done) {
          done({stamp,
                RenderError{ErrorStage::Submit, false,
                            "injected device/submit failure",
                            "simulated terminal receipt, not hardware loss"}});
          throw std::runtime_error(
              "injected unknown-acceptance submit failure");
        }}
                                        : SubmitFrame{},
        events);
    session->setAssetLoader(*loader);
    lifetime->onShutdownProgress(false, false, loader->inFlight());
    session->setPresentationMode(mode == "readback"
                                     ? PresentationPath::Readback
                                     : PresentationPath::VulkanShared,
                                 mode == "auto");
    CameraState camera;
    camera.camera.position = {0, 0, 3};
    session->setCamera(camera);
    auto &v = window->viewport();
    auto d = v.devicePixelRatioF();
    session->setViewport({{v.width(), v.height()},
                          {qRound(v.width() * d), qRound(v.height() * d)},
                          d});
    session->setVisible(v.isVisible());
    session->start();
    if (test == "close-late-loader") {
      extraLoad = loader->beginLoad(
          std::get<AssetEntry>(AssetCatalog{}.describe({"floor"})), 999999,
          [this](auto) {
            std::cout << "late loader receipt GUI count=" << loader->inFlight()
                      << std::endl;
          });
    } else
      session->selectModel({model});
    std::cout << "viewer backend=" << info.backend
              << " device=" << info.deviceName << " DPR=" << d
              << " Qt=" << qVersion() << std::endl;
    if (closing)
      session->requestStop();
  }
  void rootJoined() {
    if (pool.activeThreadCount() != 0) {
      std::cerr << "root joined; waiting for Qt pool runnable epilogue, active="
                << pool.activeThreadCount() << std::endl;
      QTimer::singleShot(10, this, [this] { rootJoined(); });
      return;
    }
    if (loader || session || applicationScope)
      qFatal("root joined before application owners/scopes retired");
    lifetime->onRootJoined();
    waiting.stop();
    std::cout << "lifetime assertions: SessionStopped=1 loaderIdle=1 "
                 "rootJoined=1 poolActive=0 GUIContextAlive=1"
              << std::endl;
    std::cout << "viewer stopped GUI heartbeats=" << beats
              << " elapsedMs=" << elapsed.elapsed() << std::endl;
    std::cout << "viewer exit code=" << code << std::endl;
    app.exit(code);
  }
  ~Viewer() override {
    if (lifetime)
      try {
        lifetime->assertSafeDestruction();
      } catch (std::exception const &e) {
        qFatal("Viewer lifetime violation: %s", e.what());
      }
  }
  void requestStop() {
    closing = true;
    if (!waiting.isActive())
      waiting.start();
    lifetime->requestExit();
    if (!session && !loading)
      finish();
  }
  void finish() {
    sessionStopped = true;
    std::cout << "session onStopped loaderReceipts="
              << (loader ? loader->inFlight() : 0) << std::endl;
    if (test == "close-late-loader" && loader && loader->inFlight())
      releaseDecodeAt = beats + 5;
    lifetime->onShutdownProgress(loading, true,
                                 loader ? loader->inFlight() : 0);
    lifetime->requestExit();
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
#ifdef DEMO_E2E
    if (test.rfind("e2e-", 0) == 0) {
      if (scenario.tick(*session, *window)) {
        if (!screenshot.empty()) {
          window->grab().save(
              QString::fromStdString(screenshot + "-widgets.png"));
          if (auto native = window->viewport().nativeWindow())
            native->screen()
                ->grabWindow(native->winId())
                .save(
                    QString::fromStdString(screenshot + "-native-window.png"));
          else
            window->viewport().readbackImage().save(
                QString::fromStdString(screenshot + "-readback.png"));
        }
        requestStop();
      }
      return;
    }
#endif
    if ((test == "close-decode" || test == "close-late-loader") &&
        decodeGate.entered.load()) {
      desiredCloseObserved = true;
      std::cout << "close with actual worker entered, loaderReceipts="
                << loader->inFlight() << std::endl;
      requestStop();
      return;
    }
    if (!s->installedModel)
      return;
    if (s->life != LifeState::Running && stage != 6 && stage != 7)
      return;
    if (test == "close-drain" && stage == 0) {
      desiredCloseObserved = true;
      std::cout << "close after requesting actual drain; held-drain unit "
                   "covers pending phase"
                << std::endl;
      stage = 1;
      session->setViewport({{161, 121}, {161, 121}, 1});
      requestStop();
      return;
    }
    if (test == "close-native") {
      desiredCloseObserved = true;
      std::cout << "close on native invalidation" << std::endl;
      if (auto native = window->viewport().nativeWindow()) {
        QPlatformSurfaceEvent invalid(
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
        QApplication::sendEvent(native, &invalid);
      } else
        session->invalidatePresentation({ErrorStage::Surface, true,
                                         "simulated native loss on readback",
                                         "no real native surface on CPU"});
      requestStop();
      return;
    }
    if (test.rfind("close-", 0) == 0)
      return; // wait for the requested event, never silently run full scenario
    if (test == "black-flash") {
      auto completed = s->performance.completed;
      if (stage == 0) {
        if (completed == 0 || (s->path == PresentationPath::VulkanShared &&
                               s->performance.sharedShown == 0))
          return;
        if (nearBlack(sampleViewport()))
          return;
        beginFlash("model");
        session->selectModel({"diablo"});
        lastCompleted = completed;
        stage = 1;
        return;
      }
      if (stage == 1) {
        // Advance on receipt count past baseline; do not wait for a quiet inFlight==0
        // gap (Pathtrace keeps frames in flight almost continuously).
        if (s->installedModel->value != "diablo" || completed <= lastCompleted)
          return;
        if (nearBlack(sampleViewport()))
          return;
        endFlash();
        beginFlash("pipeline");
        session->setRenderOptions({PipelineKind::Pathtrace, 1, false});
        lastCompleted = completed;
        stage = 2;
        return;
      }
      if (stage == 2) {
        // Receipt past baseline proves soft rebuild presented; latency is diagnostic only.
        if (completed <= lastCompleted)
          return;
        if (nearBlack(sampleViewport()))
          return;
        endFlash();
        beginFlash("pipeline-interactive");
        session->setRenderOptions({PipelineKind::PathtraceInteractive, 1, false});
        lastCompleted = completed;
        stage = 3;
        return;
      }
      if (stage == 3) {
        if (completed <= lastCompleted)
          return;
        if (nearBlack(sampleViewport()))
          return;
        endFlash();
        // Leave path tracing before resize so continuous PT does not dominate the wait.
        session->setRenderOptions({PipelineKind::Visibility, 1, false});
        beginFlash("resize", 220, 160);
        window->viewport().setFixedSize(220, 160);
        lastCompleted = completed;
        stage = 4;
        return;
      }
      if (stage == 4) {
        if (completed <= lastCompleted)
          return;
        if (nearBlack(sampleViewport()))
          return;
        if (s->path == PresentationPath::VulkanShared && presentation && !capture) {
          capture.emplace(presentation->captureNext());
          session->setCamera(s->camera);
          return;
        }
        if (capture) {
          if (capture->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;
          auto image = capture->get();
          capture.reset();
          std::cout << "black-flash resize capture=" << image.extent.x << "x" << image.extent.y
                    << " expect=" << flash.expectW << "x" << flash.expectH << std::endl;
          if (image.extent.x != flash.expectW || image.extent.y != flash.expectH) {
            code = 1;
            std::cerr << "black-flash FAIL resize extent mismatch" << std::endl;
          }
        }
        endFlash();
        std::cout << "black-flash GUI maxGapMs=" << maxGuiGap
                  << " sharedShown=" << s->performance.sharedShown << std::endl;
        requestStop();
        return;
      }
      return;
    }
    auto completed = s->performance.completed;
    switch (stage) {
    case 0:
      if (completed == 0 || (s->path == PresentationPath::VulkanShared &&
                             s->performance.sharedShown == 0))
        return;
      std::cout << "installed=" << s->installedModel->value
                << " sharedShown=" << s->performance.sharedShown
                << " initial generation=" << s->generation << std::endl;
      lastCompleted = completed;
      if (auto spin = window->findChild<QDoubleSpinBox *>("ambient"))
        spin->setValue(.3);
      if (auto spin = window->findChild<QDoubleSpinBox *>("roughness"))
        spin->setValue(.65);
      {
        QMouseEvent down(QEvent::MouseButtonPress, {20, 20}, Qt::LeftButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window->viewport(), &down);
        QMouseEvent move(QEvent::MouseMove, {40, 30}, Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
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
      if (!capture || capture->wait_for(std::chrono::seconds(0)) !=
                          std::future_status::ready)
        return;
      {
        auto image = capture->get();
        capture.reset();
        if (!screenshot.empty())
          ReadbackAdapter::copyImage(image).save(
              QString::fromStdString(screenshot + "-presented.png"));
        std::cout << "stable actual presented capture=" << image.extent.x << "x"
                  << image.extent.y << std::endl;
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
        cameraHost = window->viewport().nativeWindow()
                         ? window->viewport().nativeWindow()->winId()
                         : 0;
        session->setCamera(c);
        if (session->snapshot()->generation != cameraGeneration)
          throw std::runtime_error("camera changed presentation generation");
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
           (!window->viewport().nativeWindow() ||
            window->viewport().nativeWindow()->winId() != cameraHost)))
        throw std::runtime_error("PT camera unexpectedly rebuilt presentation");
      std::cout << "PT camera reuse host=" << cameraHost
                << " generation=" << cameraGeneration << std::endl;
      std::cout << "pathtrace camera sampleOne=" << sawSampleOne
                << " transitionMs=" << phase.elapsed()
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
        cameraHost = window->viewport().nativeWindow()
                         ? window->viewport().nativeWindow()->winId()
                         : 0;
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
           (!window->viewport().nativeWindow() ||
            window->viewport().nativeWindow()->winId() != cameraHost)))
        throw std::runtime_error(
            "TAA camera unexpectedly rebuilt presentation");
      std::cout << "TAA camera reuse host=" << cameraHost
                << " generation=" << cameraGeneration << std::endl;
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
      if (s->installedModel->value != "diablo" || !s->model ||
          s->model->value != "diablo")
        return;
      lastCompleted = completed;
      session->setCamera(s->camera); // force one bounded new receipt even if
                                     // dirty work already settled
      stage = 13;
      return;
    case 13:
      if (s->life != LifeState::Running || s->performance.inFlight ||
          completed <= lastCompleted)
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
        if (capture->wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready)
          return;
        auto image = capture->get();
        capture.reset();
        if (!screenshot.empty())
          ReadbackAdapter::copyImage(image).save(
              QString::fromStdString(screenshot + "-latest-diablo.png"));
        std::cout << "actual latest diablo presented capture=" << image.extent.x
                  << "x" << image.extent.y << std::endl;
      }
      if (!screenshot.empty()) {
        window->grab().save(
            QString::fromStdString(screenshot + "-widgets.png"));
        // QWidget::grab excludes native children. Public Qt capture of this
        // application's own XID, not a desktop/browser automation surface.
        if (auto own = window->viewport().nativeWindow())
          own->screen()
              ->grabWindow(own->winId())
              .save(QString::fromStdString(screenshot + "-native-window.png"));
      }
      std::cout
          << "performance UI: "
          << window->findChild<QLabel *>("performance")->text().toStdString()
          << std::endl;
      std::cout << "scenario full latest installed=" << s->installedModel->value
                << " sharedShown=" << s->performance.sharedShown
                << " GUI ticks=" << beats
                << " DPR=" << window->viewport().devicePixelRatioF()
                << std::endl;
      if (mode == "shared" && s->performance.sharedShown == 0)
        code = 1;
      if (s->path == PresentationPath::VulkanShared) {
        auto native = window->viewport().nativeWindow();
        if (!native)
          throw std::runtime_error("missing native window");
        cameraGeneration = s->generation;
        lastCompleted = completed;
        QPlatformSurfaceEvent invalid(
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed);
        QApplication::sendEvent(native, &invalid);
        stage = 10;
        return;
      }
      requestStop();
      return;
    case 11:
      if (!capture || capture->wait_for(std::chrono::seconds(0)) !=
                          std::future_status::ready)
        return;
      {
        auto image = capture->get();
        capture.reset();
        if (image.pixels.size() != normalPixels.size())
          throw std::runtime_error("normal comparison extent changed");
        std::size_t changed = 0;
        for (std::size_t i = 0; i < normalPixels.size(); ++i)
          changed += image.pixels[i] != normalPixels[i];
        std::cout << "actual normal-map on/off changedPixels=" << changed
                  << " / " << normalPixels.size() << std::endl;
        if (!changed)
          throw std::runtime_error(
              "normal-map shader had no observable effect");
        if (!screenshot.empty())
          ReadbackAdapter::copyImage(image).save(
              QString::fromStdString(screenshot + "-normal-off.png"));
        window->findChild<QCheckBox *>("normal")->setChecked(true);
      }
      window->viewport().setFixedSize(209, 145);
      session->setRenderOptions({PipelineKind::Pathtrace, 1, false});
      stage = 2;
      return;
    case 10:
      if (s->generation <= cameraGeneration || completed <= lastCompleted)
        return;
      std::cout << "Platform event invalidation rebuilt generation="
                << s->generation << " prior=" << cameraGeneration << std::endl;
      requestStop();
      return;
    }
  }
};
} // namespace demo

namespace {
// Resolve the loginctl session for *this* DISPLAY — never "first seat0".
// Prefer Display==$DISPLAY among non-Remote sessions; else $XDG_SESSION_ID;
// else the sole active local x11/wayland user session.
std::string resolveLogindSessionId() {
  std::string display = std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "";
  // Strip screen suffix (:1.0 → :1) for comparison.
  auto bare = display;
  if (auto dot = bare.find('.'); dot != std::string::npos)
    bare.resize(dot);
  std::string list;
  if (FILE *pipe = popen("loginctl list-sessions --no-legend 2>/dev/null", "r")) {
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe))
      list += buf;
    pclose(pipe);
  }
  std::vector<std::string> ids;
  {
    std::istringstream ss(list);
    std::string id, rest;
    while (ss >> id) {
      ids.push_back(id);
      std::getline(ss, rest);
    }
  }
  std::string displayMatch;
  std::vector<std::string> graphical;
  for (auto const &id : ids) {
    std::string cmd = "loginctl show-session " + id + " -p Display -p Type -p Class -p Active -p Remote 2>/dev/null";
    std::string props;
    if (FILE *pipe = popen(cmd.c_str(), "r")) {
      char buf[256];
      while (fgets(buf, sizeof(buf), pipe))
        props += buf;
      pclose(pipe);
    }
    auto grab = [&](char const *key) -> std::string {
      auto k = std::string(key) + "=";
      auto pos = props.find(k);
      if (pos == std::string::npos)
        return {};
      auto end = props.find('\n', pos);
      return props.substr(pos + k.size(), end == std::string::npos ? std::string::npos : end - pos - k.size());
    };
    auto sessDisplay = grab("Display");
    auto type = grab("Type");
    auto cls = grab("Class");
    auto active = grab("Active");
    auto remote = grab("Remote");
    if (remote != "yes" && !bare.empty() && !sessDisplay.empty()) {
      auto sd = sessDisplay;
      if (auto dot = sd.find('.'); dot != std::string::npos)
        sd.resize(dot);
      if (sd == bare || sessDisplay == display)
        displayMatch = id;
    }
    if (cls == "user" && remote != "yes" && (type == "x11" || type == "wayland") && active == "yes")
      graphical.push_back(id);
  }
  if (!displayMatch.empty())
    return displayMatch;
  if (const char *id = std::getenv("XDG_SESSION_ID"); id && id[0])
    return id;
  if (graphical.size() == 1)
    return graphical.front();
  return {};
}

// Black-flash requires a live, unlocked display. Lock/blank makes grabWindow
// near-black and is not a product regression — skip with CTest code 77.
// DEMO_FORCE_BLACK_FLASH_RUN=1 forces the run even when locked (manual/debug).
// DEMO_FORCE_BLACK_FLASH_SKIP=1 always skips.
bool displayLockedOrBlank(std::string *detail) {
  if (const char *forceRun = std::getenv("DEMO_FORCE_BLACK_FLASH_RUN");
      forceRun && forceRun[0] && forceRun[0] != '0')
    return false;
  if (const char *forceSkip = std::getenv("DEMO_FORCE_BLACK_FLASH_SKIP");
      forceSkip && forceSkip[0] && forceSkip[0] != '0') {
    if (detail)
      *detail = "DEMO_FORCE_BLACK_FLASH_SKIP set";
    return true;
  }
  auto session = resolveLogindSessionId();
  if (!session.empty()) {
    std::string cmd = "loginctl show-session " + session + " -p LockedHint 2>/dev/null";
    std::string out;
    if (FILE *pipe = popen(cmd.c_str(), "r")) {
      char buf[256];
      while (fgets(buf, sizeof(buf), pipe))
        out += buf;
      pclose(pipe);
    }
    if (out.find("LockedHint=yes") != std::string::npos) {
      if (detail)
        *detail = "loginctl session " + session + " LockedHint=yes (session locked)";
      return true;
    }
  } else if (detail) {
    // No resolvable session: still check monitor; note ambiguity in detail if blank.
  }
  // xset q against $DISPLAY: only explicit monitor-off counts as blank.
  if (FILE *pipe = popen("xset q 2>/dev/null", "r")) {
    char buf[512] = {};
    std::string out;
    while (fgets(buf, sizeof(buf), pipe))
      out += buf;
    pclose(pipe);
    if (out.find("Monitor is Off") != std::string::npos) {
      if (detail)
        *detail = "xset: Monitor is Off";
      return true;
    }
  }
  return false;
}
} // namespace
int main(int argc, char **argv) {
  QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QApplication app(argc, argv);
  app.setQuitOnLastWindowClosed(false);
  QCommandLineParser args;
  args.addHelpOption();
  args.addOption({"backend", "Renderer backend (cpu only when explicit)",
                  "backend", "vulkan"});
  args.addOption(
      {"presentation", "auto|shared|readback", "presentation", "auto"});
  args.addOption({"asset-root",
                  "TinyRenderer resource root (contains LICENSE.txt and obj)",
                  "path"});
  args.addOption({"model", "Catalog model", "model", "african_head"});
  args.addOption({"self-test",
                  "bounded interactions: full|black-flash|close-*",
                  "test"});
  args.addOption({"screenshot", "Local PNG output prefix", "path"});
#ifdef DEMO_E2E
  args.addOption({"scenario", "cycle|resize|close-loading|fallback|faults",
                  "scenario", "cycle"});
  args.addOption({"frames", "Minimum receipts after stress", "frames", "300"});
  args.addOption(
      {"close-stage",
       "load|install|submit|prepare|bootstrap|decode|late-loader|drain|native",
       "stage", "load"});
  args.addOption({"fault-stage", "load|submit|present (explicit simulation)",
                  "stage", "submit"});
  args.addOption({"inject-zero-shared",
                  "Test-only acceptance observation injection (always fails)"});
#endif
  args.process(app);
#ifdef DEMO_E2E
  if (!QStringList{"load", "install", "submit", "prepare", "bootstrap",
                   "decode", "late-loader", "drain", "native"}
           .contains(args.value("close-stage")) ||
      !QStringList{"load", "submit", "present"}.contains(
          args.value("fault-stage"))) {
    std::cerr << "invalid close-stage/fault-stage" << std::endl;
    return 1;
  }
#endif
  auto backend = args.value("backend").toStdString(),
       mode = args.value("presentation").toStdString();
  if ((backend != "cpu" && backend != "vulkan") ||
      (mode != "auto" && mode != "shared" && mode != "readback")) {
    std::cerr << "invalid backend/presentation" << std::endl;
    return 1;
  }
  std::filesystem::path assetRoot;
  try {
    assetRoot = demo::validateAssetRoot(
        args.isSet("asset-root")
            ? std::filesystem::path(args.value("asset-root").toStdString())
            : demo::defaultAssetRoot());
  } catch (std::exception const &e) {
    std::cerr << "asset root error: " << e.what() << std::endl;
    return 1;
  }
  auto selfTest = args.value("self-test").toStdString();
  if (selfTest == "black-flash") {
    std::string detail;
    if (displayLockedOrBlank(&detail)) {
      std::cerr << "black-flash SKIP: display locked or blank — not an acceptance run ("
                << detail << ")" << std::endl;
      std::cerr << "Waiting for unlock to re-run acceptance: black-flash x3, DPR1.25, negatives"
                << std::endl;
      return 77;
    }
  }
  demo::Viewer viewer(app, backend, mode, args.value("model").toStdString(),
                      selfTest,
                      args.value("screenshot").toStdString(), assetRoot);
#ifdef DEMO_E2E
  auto scenario = args.value("scenario").toStdString();
  bool ok = false;
  auto frames = args.value("frames").toUInt(&ok);
  if (!ok || !frames || frames > 10000) {
    viewer.requestStop();
    viewer.code = 1;
  }
  if (scenario == "close-loading") {
    auto stage = args.value("close-stage").toStdString();
    viewer.test = "close-" + stage;
    if (stage == "bootstrap")
      QTimer::singleShot(0, &viewer, [&] {
        viewer.desiredCloseObserved = true;
        std::cout << "close during bootstrap" << std::endl;
        viewer.requestStop();
      });
  } else if (scenario == "faults")
    viewer.test =
        "injected-" + args.value("fault-stage").toStdString() + "-fault";
  else if (scenario == "cycle" || scenario == "resize" ||
           scenario == "fallback") {
    viewer.test = "e2e-" + scenario;
    viewer.scenario.name = scenario;
    viewer.scenario.frames = frames;
  } else {
    viewer.code = 1;
    viewer.requestStop();
  }
  viewer.injectZeroShared = args.isSet("inject-zero-shared");
  viewer.heartbeat.start();
  viewer.script.start();
  viewer.window->viewport().setFixedSize(160, 120);
#endif
  return app.exec();
}
