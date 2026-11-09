#include "ui/PropertyPanel.h"

#include <QDoubleSpinBox>
#include <QEvent>
#include <QPointer>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <cmath>
#include <variant>

namespace demo {
namespace {
QDoubleSpinBox *spin(QWidget *parent, double minv, double maxv, double step) {
  auto *s = new QDoubleSpinBox(parent);
  s->setRange(minv, maxv);
  s->setSingleStep(step);
  s->setDecimals(4);
  // I2: one valueChanged per committed edit (Enter / step / focus-out), not per keystroke.
  s->setKeyboardTracking(false);
  return s;
}
bool nearlyEqual(double a, double b) {
  return std::abs(a - b) < 1e-9;
}
} // namespace

PropertyPanel::PropertyPanel(PropertyCommands commands, QWidget *parent)
    : QWidget(parent), commands_(std::move(commands)) {
  auto *layout = new QVBoxLayout(this);
  title_ = new QLabel(QStringLiteral("No selection"), this);
  layout->addWidget(title_);
  form_ = new QFormLayout();
  layout->addLayout(form_);
  tx_ = spin(this, -1e6, 1e6, 0.1);
  ty_ = spin(this, -1e6, 1e6, 0.1);
  tz_ = spin(this, -1e6, 1e6, 0.1);
  sx_ = spin(this, 0.001, 1e6, 0.1);
  sy_ = spin(this, 0.001, 1e6, 0.1);
  sz_ = spin(this, 0.001, 1e6, 0.1);
  roughness_ = spin(this, 0.0, 1.0, 0.05);
  metallic_ = spin(this, 0.0, 1.0, 0.05);
  lightIntensity_ = spin(this, 0.0, 1e6, 0.1);
  fov_ = spin(this, 0.01, 3.0, 0.05);
  near_ = spin(this, 0.001, 1e5, 0.01);
  far_ = spin(this, 0.01, 1e6, 1.0);
  form_->addRow(QStringLiteral("Translate X"), tx_);
  form_->addRow(QStringLiteral("Translate Y"), ty_);
  form_->addRow(QStringLiteral("Translate Z"), tz_);
  form_->addRow(QStringLiteral("Scale X"), sx_);
  form_->addRow(QStringLiteral("Scale Y"), sy_);
  form_->addRow(QStringLiteral("Scale Z"), sz_);
  form_->addRow(QStringLiteral("Roughness"), roughness_);
  form_->addRow(QStringLiteral("Metallic"), metallic_);
  form_->addRow(QStringLiteral("Light intensity"), lightIntensity_);
  form_->addRow(QStringLiteral("Camera FOV"), fov_);
  form_->addRow(QStringLiteral("Camera near"), near_);
  form_->addRow(QStringLiteral("Camera far"), far_);
  // I4: entity CameraLens is stored on World but does not drive the viewport camera.
  cameraNote_ = new QLabel(
      QStringLiteral("Entity camera FOV/near/far do not affect the viewport "
                     "(World has no removeCamera; viewport uses session CameraState)."),
      this);
  cameraNote_->setWordWrap(true);
  cameraNote_->setObjectName(QStringLiteral("cameraViewportNote"));
  layout->addWidget(cameraNote_);
  materialSessionNote_ = new QLabel(
      QStringLiteral("Session normal-mapping / roughness apply to the shared original "
                     "material only; entities with a material override keep their override."),
      this);
  materialSessionNote_->setWordWrap(true);
  materialSessionNote_->setObjectName(QStringLiteral("materialSessionNote"));
  layout->addWidget(materialSessionNote_);
  wireSpin(tx_, [this] { emitTransform(); });
  wireSpin(ty_, [this] { emitTransform(); });
  wireSpin(tz_, [this] { emitTransform(); });
  wireSpin(sx_, [this] { emitTransform(); });
  wireSpin(sy_, [this] { emitTransform(); });
  wireSpin(sz_, [this] { emitTransform(); });
  wireSpin(roughness_, [this] { emitMaterial(); });
  wireSpin(metallic_, [this] { emitMaterial(); });
  wireSpin(lightIntensity_, [this] { emitLight(); });
  wireSpin(fov_, [this] { emitCamera(); });
  wireSpin(near_, [this] { emitCamera(); });
  wireSpin(far_, [this] { emitCamera(); });
}

std::vector<QDoubleSpinBox *> PropertyPanel::allSpins() const {
  return {tx_, ty_, tz_, sx_, sy_, sz_, roughness_, metallic_, lightIntensity_, fov_, near_,
          far_};
}

void PropertyPanel::silenceSpins() {
  for (auto *s : allSpins()) {
    if (!s)
      continue;
    s->blockSignals(true);
    s->removeEventFilter(this);
    disconnect(s, nullptr, this, nullptr);
  }
}

PropertyPanel::~PropertyPanel() {
  // I-B: ~QWidget may FocusOut → valueChanged while members are already torn down.
  dying_ = true;
  silenceSpins();
}

void PropertyPanel::wireSpin(QDoubleSpinBox *s, std::function<void()> fn) {
  s->installEventFilter(this);
  connect(s, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, fn](double) {
    if (dying_ || syncing_)
      return;
    fn();
  });
}

bool PropertyPanel::eventFilter(QObject *watched, QEvent *event) {
  if (dying_)
    return QWidget::eventFilter(watched, event);
  if (qobject_cast<QDoubleSpinBox *>(watched)) {
    if (event->type() == QEvent::FocusIn) {
      if (commands_.beginEditMerge)
        commands_.beginEditMerge();
    } else if (event->type() == QEvent::FocusOut) {
      if (commands_.endEditMerge)
        commands_.endEditMerge();
      // Minor1: refill from latest snapshot after blur (e.g. undo while focused).
      if (lastSnap_)
        showSnapshot(lastSnap_, lastSelection_);
    }
  }
  return QWidget::eventFilter(watched, event);
}

void PropertyPanel::setSpinValue(QDoubleSpinBox *s, double v) {
  // I3: do not clobber in-progress typing; skip no-op writes.
  if (s->hasFocus())
    return;
  if (nearlyEqual(s->value(), v))
    return;
  s->setValue(v);
}

std::optional<EditorEntitySnapshot>
PropertyPanel::findEntity(SessionSnapshotPtr const &snap, EditorEntityId id) const {
  if (!snap || !id.isValid())
    return std::nullopt;
  for (auto const &e : snap->entities)
    if (e.id == id)
      return e;
  return std::nullopt;
}

void PropertyPanel::showSnapshot(SessionSnapshotPtr snap,
                                 editor::SelectionState const &selection) {
  if (dying_)
    return;
  syncing_ = true;
  lastSnap_ = snap;
  lastSelection_ = selection;
  current_ = selection.primary().value_or(EditorEntityId{});
  auto ent = findEntity(snap, current_);
  lastEnt_ = ent;
  if (!ent) {
    title_->setText(QStringLiteral("No selection"));
    setEnabled(false);
    syncing_ = false;
    return;
  }
  setEnabled(true);
  title_->setText(QString::fromStdString(ent->name));
  setSpinValue(tx_, ent->local.translation.x);
  setSpinValue(ty_, ent->local.translation.y);
  setSpinValue(tz_, ent->local.translation.z);
  setSpinValue(sx_, ent->local.scale.x);
  setSpinValue(sy_, ent->local.scale.y);
  setSpinValue(sz_, ent->local.scale.z);
  // Minor3: show real material values (override or install-time base), not hard-coded 0.5/0.0.
  if (ent->materialOverride) {
    setSpinValue(roughness_, ent->materialOverride->roughness);
    setSpinValue(metallic_, ent->materialOverride->metallic);
  } else if (ent->materialBase) {
    setSpinValue(roughness_, ent->materialBase->roughness);
    setSpinValue(metallic_, ent->materialBase->metallic);
  } else {
    setSpinValue(roughness_, 0.5);
    setSpinValue(metallic_, 0.0);
  }
  if (ent->light) {
    if (auto const *pl = std::get_if<lr::core::PointLight>(&*ent->light))
      setSpinValue(lightIntensity_, pl->intensity);
    else if (auto const *dl = std::get_if<lr::core::DirectionalLight>(&*ent->light))
      setSpinValue(lightIntensity_, dl->illuminance);
  } else {
    setSpinValue(lightIntensity_, 0.0);
  }
  if (ent->camera) {
    setSpinValue(fov_, ent->camera->verticalFov);
    setSpinValue(near_, ent->camera->nearPlane);
    setSpinValue(far_, ent->camera->farPlane);
  } else {
    setSpinValue(fov_, 0.8);
    setSpinValue(near_, 0.05);
    setSpinValue(far_, 500.0);
  }
  syncing_ = false;
}

void PropertyPanel::emitTransform() {
  if (dying_ || !commands_.setTransform || !current_.isValid())
    return;
  // I1: copy local from snapshot (preserves rotation); only overwrite TRS fields we edit.
  lr::core::Transform xf =
      lastEnt_ ? lastEnt_->local : lr::core::Transform::identity();
  xf.translation = {static_cast<float>(tx_->value()), static_cast<float>(ty_->value()),
                    static_cast<float>(tz_->value())};
  xf.scale = {static_cast<float>(sx_->value()), static_cast<float>(sy_->value()),
              static_cast<float>(sz_->value())};
  if (lastEnt_)
    lastEnt_->local = xf;
  commands_.setTransform(current_, xf);
}

void PropertyPanel::emitMaterial() {
  if (dying_ || !commands_.setMaterialDesc || !current_.isValid())
    return;
  // C1: seed from override or materialBase so textures survive; only edit factors.
  lr::core::MaterialDesc desc{};
  if (lastEnt_ && lastEnt_->materialOverride)
    desc = *lastEnt_->materialOverride;
  else if (lastEnt_ && lastEnt_->materialBase)
    desc = *lastEnt_->materialBase;
  desc.roughness = static_cast<float>(roughness_->value());
  desc.metallic = static_cast<float>(metallic_->value());
  if (lastEnt_)
    lastEnt_->materialOverride = desc;
  commands_.setMaterialDesc(current_, desc);
}

void PropertyPanel::emitLight() {
  if (dying_ || !commands_.setLight || !current_.isValid())
    return;
  float intensity = static_cast<float>(lightIntensity_->value());
  if (intensity <= 0.f) {
    if (commands_.clearLight)
      commands_.clearLight(current_);
    if (lastEnt_)
      lastEnt_->light = std::nullopt;
    return;
  }
  // Minor2: preserve light type, color, and direction; only edit intensity/illuminance.
  lr::core::LightDesc light = lr::core::PointLight{};
  if (lastEnt_ && lastEnt_->light)
    light = *lastEnt_->light;
  if (auto *pl = std::get_if<lr::core::PointLight>(&light)) {
    pl->intensity = intensity;
  } else if (auto *dl = std::get_if<lr::core::DirectionalLight>(&light)) {
    dl->illuminance = intensity;
  } else {
    lr::core::PointLight pl{};
    pl.intensity = intensity;
    light = pl;
  }
  if (lastEnt_)
    lastEnt_->light = light;
  commands_.setLight(current_, light);
}

void PropertyPanel::emitCamera() {
  if (dying_ || !commands_.setCamera || !current_.isValid())
    return;
  lr::scene::CameraLens lens{};
  if (lastEnt_ && lastEnt_->camera)
    lens = *lastEnt_->camera;
  lens.verticalFov = static_cast<float>(fov_->value());
  lens.nearPlane = static_cast<float>(near_->value());
  lens.farPlane = static_cast<float>(far_->value());
  if (lastEnt_)
    lastEnt_->camera = lens;
  commands_.setCamera(current_, lens);
}

} // namespace demo
