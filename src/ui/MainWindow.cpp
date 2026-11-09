#include "MainWindow.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <limits>
namespace demo {
namespace {
// Text is a draft until Qt commits (Return/focus-out); arrows/wheel commit
// steps. A command's synchronous GUI acknowledgement is authoritative even if
// its snapshot publication is delayed. Stale refreshes cannot roll it back.
class AppearanceSpinBox : public QDoubleSpinBox {
public:
  AppearanceSpinBox() { setKeyboardTracking(false); }
  void acknowledge(double value) {
    awaiting_ = value;
    QSignalBlocker block(this);
    setValue(value);
    lineEdit()->setModified(false);
  }
  void observe(double value) {
    if (awaiting_) {
      if (float(*awaiting_) != float(value))
        return;
      awaiting_.reset();
    }
    if (lineEdit()->hasFocus() && lineEdit()->isModified())
      return;
    QSignalBlocker block(this);
    setValue(value);
  }

private:
  std::optional<double> awaiting_;
};
} // namespace

MainWindow::MainWindow(UiCommands c) : commands_(std::move(c)) {
  auto central = new QWidget;
  auto layout = new QVBoxLayout(central);
  auto controls = new QHBoxLayout;
  model_ = new QComboBox;
  model_->setObjectName("model");
  model_->addItems({"african_head", "diablo", "boggie", "floor"});
  pipeline_ = new QComboBox;
  pipeline_->setObjectName("pipeline");
  pipeline_->addItems({"Visibility", "Pathtrace", "PathtraceInteractive"});
  exposure_ = new QDoubleSpinBox;
  exposure_->setObjectName("exposure");
  exposure_->setRange(.01, 100);
  exposure_->setValue(1);
  exposure_->setSingleStep(.1);
  taa_ = new QCheckBox("TAA");
  auto reset = new QPushButton("Reset view");
  controls->addWidget(model_);
  controls->addWidget(pipeline_);
  controls->addWidget(new QLabel("Exposure"));
  controls->addWidget(exposure_);
  controls->addWidget(taa_);
  controls->addWidget(reset);
  auto ambient = new AppearanceSpinBox;
  ambient->setObjectName("ambient");
  ambient->setRange(0, std::numeric_limits<float>::max());
  ambient->setValue(.25);
  ambient->setSingleStep(.05);
  auto roughness = new AppearanceSpinBox;
  roughness->setObjectName("roughness");
  roughness->setRange(0, 1);
  roughness->setValue(.5);
  roughness->setSingleStep(.05);
  controls->addWidget(new QLabel("Ambient"));
  controls->addWidget(ambient);
  controls->addWidget(new QLabel("Roughness"));
  controls->addWidget(roughness);
  auto normals = new QCheckBox("Normal map");
  normals->setObjectName("normal");
  normals->setChecked(true);
  controls->addWidget(normals);
  connect(normals, &QCheckBox::toggled, this, [this, normals](bool enabled) {
    auto applied = commands_.normalMapping ? commands_.normalMapping(enabled)
                   : lastSnapshot_         ? lastSnapshot_->normalMapping
                                           : true;
    normalAcknowledgement_ = applied;
    QSignalBlocker block(normals);
    normals->setChecked(applied);
  });
  connect(ambient, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
          [this, ambient](double v) {
            auto applied = commands_.ambient ? commands_.ambient(float(v))
                           : lastSnapshot_   ? lastSnapshot_->ambient
                                             : .25f;
            ambient->acknowledge(applied);
          });
  connect(roughness, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
          [this, roughness](double v) {
            auto applied = commands_.roughness ? commands_.roughness(float(v))
                           : lastSnapshot_     ? lastSnapshot_->roughness
                                               : .5f;
            roughness->acknowledge(applied);
          });
  layout->addLayout(controls);
  viewport_ = new ViewportHost(commands_);
  layout->addWidget(viewport_, 1);
  status_ = new QLabel("Preparing");
  status_->setObjectName("status");
  status_->setWordWrap(true);
  layout->addWidget(status_);
  performance_ = new QLabel;
  performance_->setObjectName("performance");
  performance_->setWordWrap(true);
  layout->addWidget(performance_);
  setCentralWidget(central);
  resize(720, 560);
  setWindowTitle("lrender · Qt Vulkan viewer");
  connect(model_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int) {
            if (commands_.model)
              commands_.model({model_->currentText().toStdString()});
          });
  connect(pipeline_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          [this](int) { optionsChanged(); });
  connect(exposure_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
          [this](double) { optionsChanged(); });
  connect(taa_, &QCheckBox::toggled, this, [this](bool) { optionsChanged(); });
  connect(reset, &QPushButton::clicked, this,
          [this] { viewport_->resetCamera(bounds_); });
}
void MainWindow::optionsChanged() {
  if (!commands_.options)
    return;
  auto pipeline = PipelineKind::Visibility;
  if (pipeline_->currentIndex() == 1)
    pipeline = PipelineKind::Pathtrace;
  else if (pipeline_->currentIndex() == 2)
    pipeline = PipelineKind::PathtraceInteractive;
  commands_.options({pipeline, float(exposure_->value()), taa_->isChecked()});
}
void MainWindow::restoreAppearance() {
  if (!lastSnapshot_)
    return;
  auto a = findChild<QDoubleSpinBox *>("ambient"),
       r = findChild<QDoubleSpinBox *>("roughness");
  auto n = findChild<QCheckBox *>("normal");
  static_cast<AppearanceSpinBox *>(a)->observe(lastSnapshot_->ambient);
  static_cast<AppearanceSpinBox *>(r)->observe(lastSnapshot_->roughness);
  if (normalAcknowledgement_ &&
      *normalAcknowledgement_ == lastSnapshot_->normalMapping)
    normalAcknowledgement_.reset();
  if (!normalAcknowledgement_) {
    QSignalBlocker block(n);
    n->setChecked(lastSnapshot_->normalMapping);
  }
}
void MainWindow::showSnapshot(SessionSnapshotPtr s) {
  lastSnapshot_ = s;
  restoreAppearance();
  QSignalBlocker a(model_), b(pipeline_), c(exposure_), d(taa_);
  if (s->model)
    model_->setCurrentText(QString::fromStdString(s->model->value));
  pipeline_->setCurrentIndex(s->options.pipeline == PipelineKind::PathtraceInteractive ? 2
                                 : s->options.pipeline == PipelineKind::Pathtrace       ? 1
                                                                                       : 0);
  exposure_->setValue(s->options.exposure);
  taa_->setChecked(s->options.taa);
  viewport_->showCamera(s->camera);
  if (s->assetBounds)
    bounds_ = *s->assetBounds;
  status_->setText(QString::fromStdString(
      s->backend + " / " + s->device + " | " +
      (s->path == PresentationPath::VulkanShared ? "Shared" : "Readback") +
      " | requested=" + (s->model ? s->model->value : "none") +
      " installed=" + (s->installedModel ? s->installedModel->value : "none") +
      (s->error ? " | " + s->error->message + ": " + s->error->cause : "") +
      (s->warnings.empty() ? "" : " | " + s->warnings.back())));
  QString notices;
  for (auto const &n : s->assetNotices)
    notices += QString::fromStdString(n) + "\n";
  status_->setToolTip(notices);
  auto number = [](std::optional<double> v) {
    return v ? QString::number(*v, 'f', 3) : QString("unavailable");
  };
  auto p = s->performance;
  auto gpu = p.gpuPassTotalMs
                 ? "GPU pass sum=" + number(p.gpuPassTotalMs) +
                       " ms (profiling serializes dispatches)"
                 : QString("GPU unavailable (timers disabled/unsupported or "
                           "partial timestamps)");
  performance_->setText(
      QString("completion FPS=%1 (window %2s), completed=%3, display "
              "events=%4, dropped=%5, inflight=%6, "
              "samples=%7\nCPU GUI prepare=%8 ms | submit-to-renderer finish "
              "wall=%9 ms | %10 | presenter "
              "handover=%11 ms (not photon latency)")
          .arg(p.completionFps, 0, 'f', 1)
          .arg(p.windowSeconds, 0, 'f', 2)
          .arg(p.completed)
          .arg(p.displayed)
          .arg(p.dropped)
          .arg(p.inFlight)
          .arg(p.accumulatedSamples)
          .arg(number(p.hostPrepareMs))
          .arg(number(p.frameWallMs))
          .arg(gpu)
          .arg(number(p.presenterLatencyMs)));
  performance_->setToolTip(
      "Completion FPS counts renderer receipts, not scanout. Readback display "
      "events "
      "count owned-image handoffs, not widget paints or monitor refresh. GPU "
      "pass sums "
      "may overlap neither wall time nor end-to-end latency.");
  if (s->life == LifeState::Stopped)
    stopped_ = true;
}
void MainWindow::closeEvent(QCloseEvent *e) {
  if (stopped_) {
    e->accept();
    return;
  }
  e->ignore();
  if (!closing_) {
    closing_ = true;
    model_->setEnabled(false);
    pipeline_->setEnabled(false);
    if (commands_.stop)
      commands_.stop();
  }
}
} // namespace demo
