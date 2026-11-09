#include "ui/PropertyPanel.h"

#include <QDoubleSpinBox>
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
  return s;
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
  auto bind = [this](QDoubleSpinBox *s, auto fn) {
    connect(s, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, fn](double) {
      if (!syncing_)
        fn();
    });
  };
  bind(tx_, [this] { emitTransform(); });
  bind(ty_, [this] { emitTransform(); });
  bind(tz_, [this] { emitTransform(); });
  bind(sx_, [this] { emitTransform(); });
  bind(sy_, [this] { emitTransform(); });
  bind(sz_, [this] { emitTransform(); });
  bind(roughness_, [this] { emitMaterial(); });
  bind(metallic_, [this] { emitMaterial(); });
  bind(lightIntensity_, [this] { emitLight(); });
  bind(fov_, [this] { emitCamera(); });
  bind(near_, [this] { emitCamera(); });
  bind(far_, [this] { emitCamera(); });
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
  syncing_ = true;
  current_ = selection.primary().value_or(EditorEntityId{});
  auto ent = findEntity(snap, current_);
  if (!ent) {
    title_->setText(QStringLiteral("No selection"));
    setEnabled(false);
    syncing_ = false;
    return;
  }
  setEnabled(true);
  title_->setText(QString::fromStdString(ent->name));
  tx_->setValue(ent->local.translation.x);
  ty_->setValue(ent->local.translation.y);
  tz_->setValue(ent->local.translation.z);
  sx_->setValue(ent->local.scale.x);
  sy_->setValue(ent->local.scale.y);
  sz_->setValue(ent->local.scale.z);
  if (ent->materialOverride) {
    roughness_->setValue(ent->materialOverride->roughness);
    metallic_->setValue(ent->materialOverride->metallic);
  } else {
    roughness_->setValue(0.5);
    metallic_->setValue(0.0);
  }
  if (ent->light) {
    if (auto const *pl = std::get_if<lr::core::PointLight>(&*ent->light))
      lightIntensity_->setValue(pl->intensity);
    else if (auto const *dl = std::get_if<lr::core::DirectionalLight>(&*ent->light))
      lightIntensity_->setValue(dl->illuminance);
  } else {
    lightIntensity_->setValue(0.0);
  }
  if (ent->camera) {
    fov_->setValue(ent->camera->verticalFov);
    near_->setValue(ent->camera->nearPlane);
    far_->setValue(ent->camera->farPlane);
  } else {
    fov_->setValue(0.8);
    near_->setValue(0.05);
    far_->setValue(500.0);
  }
  syncing_ = false;
}

void PropertyPanel::emitTransform() {
  if (!commands_.setTransform || !current_.isValid())
    return;
  lr::core::Transform xf = lr::core::Transform::identity();
  xf.translation = {static_cast<float>(tx_->value()), static_cast<float>(ty_->value()),
                    static_cast<float>(tz_->value())};
  xf.scale = {static_cast<float>(sx_->value()), static_cast<float>(sy_->value()),
              static_cast<float>(sz_->value())};
  commands_.setTransform(current_, xf);
}

void PropertyPanel::emitMaterial() {
  if (!commands_.setMaterialDesc || !current_.isValid())
    return;
  lr::core::MaterialDesc desc{};
  desc.roughness = static_cast<float>(roughness_->value());
  desc.metallic = static_cast<float>(metallic_->value());
  commands_.setMaterialDesc(current_, desc);
}

void PropertyPanel::emitLight() {
  if (!commands_.setLight || !current_.isValid())
    return;
  lr::core::PointLight pl{};
  pl.intensity = static_cast<float>(lightIntensity_->value());
  if (pl.intensity <= 0.f) {
    if (commands_.clearLight)
      commands_.clearLight(current_);
    return;
  }
  commands_.setLight(current_, pl);
}

void PropertyPanel::emitCamera() {
  if (!commands_.setCamera || !current_.isValid())
    return;
  lr::scene::CameraLens lens{};
  lens.verticalFov = static_cast<float>(fov_->value());
  lens.nearPlane = static_cast<float>(near_->value());
  lens.farPlane = static_cast<float>(far_->value());
  commands_.setCamera(current_, lens);
}

} // namespace demo
