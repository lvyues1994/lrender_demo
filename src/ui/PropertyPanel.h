#pragma once

#include "editor/SelectionState.h"
#include "render/SessionTypes.h"

#include <QWidget>
#include <functional>
#include <optional>

class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QEvent;

namespace demo {

struct PropertyCommands {
  std::function<void(EditorEntityId, lr::core::Transform)> setTransform;
  std::function<void(EditorEntityId, lr::core::MaterialDesc)> setMaterialDesc;
  std::function<void(EditorEntityId, lr::core::LightDesc)> setLight;
  std::function<void(EditorEntityId)> clearLight;
  std::function<void(EditorEntityId, lr::scene::CameraLens)> setCamera;
  std::function<void(EditorEntityId)> clearCamera;
  std::function<void()> beginEditMerge;
  std::function<void()> endEditMerge;
  std::function<void(QString const &)> tip;
};

class PropertyPanel : public QWidget {
public:
  explicit PropertyPanel(PropertyCommands commands, QWidget *parent = nullptr);
  void showSnapshot(SessionSnapshotPtr snap, editor::SelectionState const &selection);

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  PropertyCommands commands_;
  EditorEntityId current_{};
  bool syncing_ = false;
  std::optional<EditorEntitySnapshot> lastEnt_;
  QLabel *title_ = nullptr;
  QLabel *cameraNote_ = nullptr;
  QDoubleSpinBox *tx_ = nullptr, *ty_ = nullptr, *tz_ = nullptr;
  QDoubleSpinBox *sx_ = nullptr, *sy_ = nullptr, *sz_ = nullptr;
  QDoubleSpinBox *roughness_ = nullptr, *metallic_ = nullptr;
  QDoubleSpinBox *lightIntensity_ = nullptr;
  QDoubleSpinBox *fov_ = nullptr, *near_ = nullptr, *far_ = nullptr;
  QFormLayout *form_ = nullptr;

  void emitTransform();
  void emitMaterial();
  void emitLight();
  void emitCamera();
  void wireSpin(QDoubleSpinBox *s, std::function<void()> fn);
  void setSpinValue(QDoubleSpinBox *s, double v);
  std::optional<EditorEntitySnapshot> findEntity(SessionSnapshotPtr const &snap,
                                                 EditorEntityId id) const;
};

} // namespace demo
