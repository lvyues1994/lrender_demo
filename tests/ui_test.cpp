#include "presentation/ReadbackAdapter.h"
#include "ui/MainWindow.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMouseEvent>
#include <QWheelEvent>
#include <doctest/doctest.h>
using namespace demo;
TEST_CASE("ui/orbit-produces-absolute-valid-camera-and-wheel-bounded") {
  CameraState latest;
  unsigned commands = 0;
  UiCommands c;
  c.camera = [&](auto value) {
    latest = value;
    ++commands;
  };
  ViewportHost view(c);
  view.resize(200, 100);
  CameraState original;
  view.showCamera(original);
  QMouseEvent down(QEvent::MouseButtonPress, {20, 20}, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&view, &down);
  QMouseEvent move(QEvent::MouseMove, {60, 30}, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&view, &move);
  CHECK(commands == 1);
  CHECK(latest.camera.position.x != original.camera.position.x);
  CHECK(latest.camera.target.x == original.camera.target.x);
  auto old = latest;
  view.wheelSteps(10000);
  CHECK(tc::length(latest.camera.position - latest.camera.target) > latest.camera.nearPlane);
  view.wheelSteps(-10000);
  CHECK(tc::length(latest.camera.position - latest.camera.target) < latest.camera.farPlane);
  CHECK(commands == 3);
}
TEST_CASE("ui/reset-frames-bounds-without-snapshot-feedback") {
  unsigned count = 0;
  CameraState camera;
  UiCommands c;
  c.camera = [&](auto v) {
    camera = v;
    ++count;
  };
  ViewportHost view(c);
  view.showCamera({});
  CHECK(count == 0);
  view.resetCamera({{-1, -2, -3}, {1, 2, 3}});
  CHECK(count == 1);
  CHECK(camera.camera.target.y == 0);
  CHECK(tc::length(camera.camera.position - camera.camera.target) > 3);
}
TEST_CASE("ui/options-and-model-commands-snapshot-does-not-loop") {
  unsigned options = 0, models = 0;
  RenderOptions last;
  ModelId id;
  UiCommands c;
  c.options = [&](auto v) {
    last = v;
    ++options;
  };
  c.model = [&](auto v) {
    id = v;
    ++models;
  };
  MainWindow window(c);
  auto pipeline = window.findChild<QComboBox *>("pipeline");
  auto exposure = window.findChild<QDoubleSpinBox *>("exposure");
  auto model = window.findChild<QComboBox *>("model");
  REQUIRE(pipeline);
  REQUIRE(exposure);
  REQUIRE(model);
  pipeline->setCurrentIndex(1);
  CHECK(last.pipeline == PipelineKind::Pathtrace);
  exposure->setValue(2);
  CHECK(last.exposure == 2);
  model->setCurrentIndex(1);
  CHECK(models == 1);
  CHECK(id.value == "diablo");
  auto count = options;
  auto snap = std::make_shared<SessionSnapshot>();
  snap->options.exposure = 3;
  snap->model = ModelId{"floor"};
  window.showSnapshot(snap);
  CHECK(options == count);
  CHECK(models == 1);
}
TEST_CASE("ui/owned-rgba-is-copied-and-close-retains-host-until-stop") {
  unsigned stops = 0;
  UiCommands c;
  c.stop = [&] { ++stops; };
  MainWindow window(c);
  auto image = std::make_shared<lr::render::ImageRgba8>();
  image->extent = {1, 1};
  image->pixels = {0xff0000ff};
  window.viewport().showFrame({{}, image});
  image.reset();
  CHECK(window.viewport().readbackImage().pixelColor(0, 0) == QColor(255, 0, 0));
  window.show();
  window.close();
  CHECK(stops == 1);
  CHECK(window.isVisible());
  window.close();
  CHECK(stops == 1);
  auto snap = std::make_shared<SessionSnapshot>();
  snap->life = LifeState::Stopped;
  window.showSnapshot(snap);
  window.close();
  CHECK_FALSE(window.isVisible());
}

#include "presentation/VulkanSharedAdapter.h"
#include "support/QtWait.h"
#include <QThread>
#include <atomic>
#include <lqtexec/EventLoopContext.h>
namespace {
struct UiJoin {
  using receiver_concept = lexec::receiver_t;
  bool *done;
  void set_value() && noexcept { *done = true; }
};
struct UiScope {
  lqtexec::EventLoopContext gui;
  lexec::counting_scope root;
  ~UiScope() {
    root.close();
    bool done = false;
    auto op = lexec::connect(root.join(), UiJoin{&done});
    lexec::start(op);
    if (!test::until([&] { return done; }))
      std::_Exit(2);
  }
};
} // namespace
TEST_CASE("ui/shared-capability-failure-does-not-create-native-host-and-can-retire") {
  UiScope app;
  unsigned native = 0, retired = 0, done = 0;
  tc::DeviceInfo info;
  info.backend = "cpu";
  VulkanSharedAdapter adapter(
      info, app.gui.get_scheduler(), app.root.get_token(),
      [&]() -> std::shared_ptr<NativeHostLease> {
        ++native;
        throw std::runtime_error("must not create");
      },
      [&] { ++retired; }, {});
  std::optional<RenderError> error;
  adapter.preparePath(PresentationPath::VulkanShared, {}, [&](auto r) {
    if (auto e = std::get_if<RenderError>(&r))
      error = *e;
    ++done;
  });
  REQUIRE(test::until([&] { return done == 1; }));
  REQUIRE(error);
  CHECK(error->stage == ErrorStage::Probe);
  CHECK(native == 0);
  adapter.beginDrain([&](auto r) {
    CHECK(std::holds_alternative<std::monostate>(r));
    ++done;
  });
  REQUIRE(test::until([&] { return done == 2; }));
  adapter.releasePath([&](auto r) {
    CHECK(std::holds_alternative<std::monostate>(r));
    ++done;
  });
  REQUIRE(test::until([&] { return done == 3; }));
  adapter.preparePath(PresentationPath::Readback, {}, [&](auto) { ++done; });
  REQUIRE(test::until([&] { return done == 4; }));
  CHECK(std::holds_alternative<lr::render::ImageTargetDesc>(adapter.describeTarget({{8, 8}, {8, 8}, 1})));
  adapter.beginDrain([&](auto) { ++done; });
  REQUIRE(test::until([&] { return done == 5; }));
  adapter.releasePath([&](auto) { ++done; });
  REQUIRE(test::until([&] { return done == 6; }));
  CHECK(native == 0);
}

TEST_CASE("ui/basic-light-and-material-controls-emit-value-commands") {
  float ambient = -1, roughness = -1;
  UiCommands commands;
  commands.ambient = [&](float v) { return ambient = v; };
  commands.roughness = [&](float v) { return roughness = v; };
  MainWindow window(commands);
  auto a = window.findChild<QDoubleSpinBox *>("ambient");
  auto r = window.findChild<QDoubleSpinBox *>("roughness");
  REQUIRE(a);
  REQUIRE(r);
  a->setValue(.5);
  r->setValue(.8);
  CHECK(ambient == doctest::Approx(.5));
  CHECK(roughness == doctest::Approx(.8));
}

TEST_CASE("ui/real-gui-surface-handshake-serializes-injected-driver-boundary") {
  UiScope app;
  lexec::static_thread_pool worker{1};
  bool gui = false;
  auto instance = reinterpret_cast<VkInstance>(std::uintptr_t{1});
  auto surface = reinterpret_cast<VkSurfaceKHR>(std::uintptr_t{2});
  NativeSurfaceFactory factory(
      [&](VkInstance) {
        gui = QThread::currentThread() == QCoreApplication::instance()->thread();
        return surface;
      },
      nullptr, true);
  auto lease = factory.lease();
  std::atomic<bool> done{false};
  std::exception_ptr error;
  struct Empty {
    void flush() {}
  };
  lexec::spawn(lexec::schedule(worker.get_scheduler()) | lexec::then([&]() noexcept {
                 try {
                   auto presenter = NativePresenterCleanup::construct(lease->policy, [&] {
                     lease->createSurface(instance);
                     return std::make_shared<Empty>();
                   });
                   NativePresenterCleanup::run(lease->policy, presenter, [] {}, [] {});
                 } catch (...) {
                   error = std::current_exception();
                 }
                 done = true;
               }),
               app.root.get_token());
  REQUIRE(test::until([&] { return done.load(); }));
  CHECK_FALSE(error);
  CHECK(gui);
  CHECK(lease->policy->canRelease());
  factory.releaseHost();
}

TEST_CASE("ui/p10-appearance-sync-and-rejected-command-no-feedback") {
  int commands = 0;
  UiCommands c;
  c.ambient = [&](float) { ++commands; return .8f; };
  c.roughness = [&](float) { ++commands; return .2f; };
  c.normalMapping = [&](bool) { ++commands; return false; };
  MainWindow window(c);
  auto snap = std::make_shared<SessionSnapshot>();
  snap->ambient = .8f;
  snap->roughness = .2f;
  snap->normalMapping = false;
  window.showSnapshot(snap);
  auto a = window.findChild<QDoubleSpinBox *>("ambient");
  auto r = window.findChild<QDoubleSpinBox *>("roughness");
  auto n = window.findChild<QCheckBox *>("normal");
  CHECK(a->value() == doctest::Approx(.8));
  CHECK(r->value() == doctest::Approx(.2));
  CHECK_FALSE(n->isChecked());
  CHECK(commands == 0);
  a->setValue(1.5);
  r->setValue(.9);
  n->setChecked(true);
  CHECK(commands == 3);
  CHECK(a->value() == doctest::Approx(.8));
  CHECK(r->value() == doctest::Approx(.2));
  CHECK_FALSE(n->isChecked());
}

TEST_CASE("ui/p10-performance-is-labelled-not-fake-gpu-or-scanout") {
  MainWindow w({});
  auto s = std::make_shared<SessionSnapshot>();
  s->backend = "vulkan";
  s->device = "test GPU";
  s->performance.completed = 10;
  s->performance.displayed = 8;
  s->performance.dropped = 2;
  s->performance.inFlight = 1;
  s->performance.accumulatedSamples = 3;
  s->performance.completionFps = 20;
  s->performance.hostPrepareMs = 1.5;
  s->performance.frameWallMs = 8;
  s->ambient = 12;
  w.showSnapshot(s);
  CHECK(w.findChild<QDoubleSpinBox *>("ambient")->value() == 12);
  auto label = w.findChild<QLabel *>("performance");
  REQUIRE(label);
  auto text = label->text();
  CHECK(text.contains("completion FPS"));
  CHECK(text.contains("CPU GUI prepare"));
  CHECK(text.contains("submit-to-renderer"));
  CHECK(text.contains("GPU unavailable"));
  CHECK(text.contains("inflight=1"));
  CHECK(text.contains("dropped=2"));
  CHECK(text.contains("samples=3"));
  s->performance.gpuPassTotalMs = 4;
  w.showSnapshot(s);
  CHECK(label->text().contains("GPU pass sum"));
  CHECK(label->text().contains("profiling serializes"));
  CHECK_FALSE(label->text().contains("GPU unavailable"));
}

#include <QKeyEvent>
#include <QLineEdit>
#include <QTimer>
namespace {
void appearanceKey(QWidget *w, int key, QString text = {},
                   Qt::KeyboardModifiers mods = Qt::NoModifier) {
  QKeyEvent press(QEvent::KeyPress, key, mods, text);
  QApplication::sendEvent(w, &press);
  QKeyEvent release(QEvent::KeyRelease, key, mods, text);
  QApplication::sendEvent(w, &release);
}
void appearanceText(QLineEdit *edit, QString text) {
  appearanceKey(edit, Qt::Key_A, {}, Qt::ControlModifier);
  for (auto ch : text)
    appearanceKey(edit, ch.unicode(), QString(ch));
}
} // namespace
TEST_CASE(
    "ui/"
    "p10-fix-keyboard-draft-survives-periodic-snapshot-and-delayed-publish") {
  auto confirmed = std::make_shared<SessionSnapshot>();
  confirmed->ambient = .25f;
  unsigned commands = 0, publications = 0, refreshes = 0;
  MainWindow *window = nullptr;
  UiCommands c;
  c.ambient = [&](float v) {
    ++commands;
    confirmed = std::make_shared<SessionSnapshot>(*confirmed);
    confirmed->ambient =
        v; // actual GUI command applies synchronously, publication is delayed
    auto receipt = confirmed;
    QTimer::singleShot(80, window, [&, receipt] {
      window->showSnapshot(receipt);
      ++publications;
    });
    return confirmed->ambient;
  };
  MainWindow w(c);
  window = &w;
  auto old = std::make_shared<SessionSnapshot>(*confirmed);
  w.showSnapshot(old);
  w.show();
  QApplication::processEvents();
  auto spin = w.findChild<QDoubleSpinBox *>("ambient");
  auto edit = spin->findChild<QLineEdit *>();
  REQUIRE(edit);
  spin->setFocus();
  QApplication::processEvents();
  appearanceText(edit, "1.");
  CHECK(commands == 0);
  QTimer periodic;
  QObject::connect(&periodic, &QTimer::timeout, &w, [&] {
    w.showSnapshot(old);
    ++refreshes;
  });
  periodic.start(500);
  REQUIRE(test::until([&] { return refreshes == 2; }));
  CHECK(edit->text() == "1.");
  appearanceKey(edit, Qt::Key_5, "5");
  CHECK(edit->text() == "1.5");
  CHECK(commands == 0);
  periodic.stop();
  appearanceKey(edit, Qt::Key_Return, "\r");
  CHECK(commands == 1);
  CHECK(confirmed->ambient == doctest::Approx(1.5));
  w.showSnapshot(old); // stale refresh before queued accepted publication
                       // cannot roll back the acknowledgement
  CHECK(spin->value() == doctest::Approx(1.5));
  REQUIRE(test::until([&] { return publications == 1; }));
  CHECK(spin->value() == doctest::Approx(1.5));
  CHECK(commands == 1);
}
TEST_CASE("ui/p10-fix-arrows-wheel-rejection-and-normal-acknowledgement") {
  auto state = std::make_shared<SessionSnapshot>();
  state->roughness = .5f;
  state->normalMapping = true;
  bool blocked = false;
  unsigned commands = 0, publications = 0;
  MainWindow *window = nullptr;
  auto publish = [&] {
    auto receipt = std::make_shared<SessionSnapshot>(*state);
    QTimer::singleShot(60, window, [&, receipt] {
      window->showSnapshot(receipt);
      ++publications;
    });
  };
  UiCommands c;
  c.roughness = [&](float v) {
    ++commands;
    if (!blocked)
      state->roughness = v;
    publish();
    return state->roughness;
  };
  c.normalMapping = [&](bool v) {
    ++commands;
    if (!blocked)
      state->normalMapping = v;
    publish();
    return state->normalMapping;
  };
  MainWindow w(c);
  window = &w;
  w.showSnapshot(std::make_shared<SessionSnapshot>(*state));
  w.show();
  QApplication::processEvents();
  auto spin = w.findChild<QDoubleSpinBox *>("roughness");
  auto edit = spin->findChild<QLineEdit *>();
  auto normal = w.findChild<QCheckBox *>("normal");
  spin->setFocus();
  QApplication::processEvents();
  appearanceKey(spin, Qt::Key_Up);
  appearanceKey(spin, Qt::Key_Up);
  CHECK(state->roughness ==
        doctest::Approx(.6)); // no stale .50 -> .55 replay on second key
  QWheelEvent wheel(QPointF(5, 5), QPointF(5, 5), QPoint(), QPoint(0, 120),
                    Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
  QApplication::sendEvent(spin, &wheel);
  CHECK(state->roughness == doctest::Approx(.65));
  normal->click();
  CHECK_FALSE(normal->isChecked());
  CHECK_FALSE(state->normalMapping);
  auto stale = std::make_shared<SessionSnapshot>();
  stale->roughness = .5f;
  stale->normalMapping = true;
  w.showSnapshot(stale);
  CHECK_FALSE(normal->isChecked());
  CHECK(spin->value() == doctest::Approx(.65));
  CHECK(commands == 4);
  REQUIRE(test::until([&] { return publications == 4; }));
  CHECK(spin->value() == doctest::Approx(.65));
  CHECK(commands == 4);
  blocked = true;
  spin->setFocus();
  appearanceText(edit, "0.9");
  appearanceKey(edit, Qt::Key_Return, "\r");
  CHECK(spin->value() == doctest::Approx(.65));
  CHECK(edit->text().toDouble() == doctest::Approx(.65));
  normal->click();
  CHECK_FALSE(normal->isChecked());
  REQUIRE(test::until([&] { return publications == 6; }));
  CHECK(spin->value() == doctest::Approx(.65));
  CHECK(commands == 6);
  // Focus-out commits a valid keyboard draft as well (even when the command is
  // blocked).
  spin->setFocus();
  appearanceText(edit, "0.8");
  normal->setFocus();
  QApplication::processEvents();
  REQUIRE(test::until([&] { return publications == 7; }));
  CHECK(spin->value() == doctest::Approx(.65));
  CHECK(commands == 7);
}
