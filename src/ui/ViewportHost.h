#include <optional>
#pragma once
#include "editor/GizmoMath.h"
#include "editor/SelectionState.h"
#include "presentation/NativeSurfaceFactory.h"
#include "render/SessionTypes.h"
#include <functional>
#include <QImage>
#include <QWidget>
namespace demo {
struct UiCommands {
  std::function<void(ModelId)> model;
  std::function<void(CameraState)> camera;
  std::function<void(RenderOptions)> options;
  std::function<void(ViewportExtent, bool)> viewport;
  std::function<void()> stop;
  std::function<float(float)> ambient, roughness;
  std::function<bool(bool)> normalMapping;
  std::function<void(RenderError)> invalidated;
  std::function<void()> undo;
  std::function<void()> redo;
  std::function<bool()> canUndo;
  std::function<bool()> canRedo;
  std::function<void(EditorEntityId)> selectEntity;
  std::function<void(EditorEntityId, std::optional<EditorEntityId>)> setEntityParent;
  std::function<void(EditorEntityId, lr::core::Transform)> setEntityTransform;
  std::function<void(EditorEntityId, lr::core::MaterialDesc)> setEntityMaterialDesc;
  std::function<void(EditorEntityId, lr::core::LightDesc)> setEntityLight;
  std::function<void(EditorEntityId)> clearEntityLight;
  std::function<void(EditorEntityId, lr::scene::CameraLens)> setEntityCamera;
  std::function<void(EditorEntityId)> clearEntityCamera;
  std::function<void()> beginEditMerge;
  std::function<void()> endEditMerge;
};
class ViewportHost : public QWidget {
public:
  explicit ViewportHost(UiCommands, QWidget *parent = nullptr);
  void showCamera(CameraState);
  void showSnapshot(SessionSnapshotPtr snap, editor::SelectionState const &selection);
  void wheelSteps(double);
  void resetCamera(lr::core::Aabb const &);
  void showFrame(OwnedFrame);
  QImage readbackImage() const;
  std::shared_ptr<NativeHostLease> createNativeLease();
  void retireNative();
  QWindow *nativeWindow() const {
    return native_ ? native_->nativeWindow() : nullptr;
  }
  void setPickHandler(std::function<void(EditorEntityId)> fn) { onPick_ = std::move(fn); }
  void setGizmoMode(editor::GizmoMode mode);
  void setGizmoSpace(editor::GizmoSpace space);
  editor::GizmoMode gizmoMode() const noexcept { return gizmoMode_; }
  editor::GizmoSpace gizmoSpace() const noexcept { return gizmoSpace_; }
  // For tests: last projected handle origins / active axis.
  editor::GizmoAxis activeAxis() const noexcept { return dragAxis_; }
  ~ViewportHost() override;

protected:
  bool eventFilter(QObject *, QEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void mouseReleaseEvent(QMouseEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void paintEvent(QPaintEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

private:
  UiCommands commands_;
  std::function<void(EditorEntityId)> onPick_;
  CameraState camera_;
  QPoint drag_;
  QPoint pressPos_;
  bool pressing_ = false;
  bool orbiting_ = false;
  bool gizmoDragging_ = false;
  bool mergeOpen_ = false;
  editor::GizmoAxis dragAxis_ = editor::GizmoAxis::None;
  editor::GizmoMode gizmoMode_ = editor::GizmoMode::Translate;
  editor::GizmoSpace gizmoSpace_ = editor::GizmoSpace::Local;
  editor::Ray pressRay_{};
  lr::core::Transform dragLocal0_{};
  lr::core::mat4 dragParentWorld_{1.f};
  lr::core::vec3 dragOrigin_{};
  lr::core::vec3 dragAxisDir_{1, 0, 0};
  EditorEntityId primary_{};
  SessionSnapshotPtr snap_;
  QImage image_;
  std::unique_ptr<NativeSurfaceFactory> native_;
  bool nativeWasExposed_ = false;
  // Shared-path display-only overlay (native child covers Qt siblings).
  QWidget *sharedOverlay_ = nullptr;

  void notifyExtent();
  ViewportExtent currentExtent() const;
  void ensureSharedOverlay();
  void syncSharedOverlayGeometry();
  void paintGizmo(QPainter &p);
  void cancelGizmoDrag();
  bool hitGizmo(QPoint const &pos, editor::GizmoAxis *outAxis) const;
  void pickAt(QPoint const &pos);
  lr::core::vec3 axisDirection(editor::GizmoAxis axis) const;
};
} // namespace demo
