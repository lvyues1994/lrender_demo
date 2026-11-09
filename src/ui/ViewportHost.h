#pragma once
#include "presentation/NativeSurfaceFactory.h"
#include "render/SessionTypes.h"
#include <QImage>
#include <QWidget>
namespace demo {
struct UiCommands {
  std::function<void(ModelId)> model;
  std::function<void(CameraState)> camera;
  std::function<void(RenderOptions)> options;
  std::function<void(ViewportExtent, bool)> viewport;
  std::function<void()> stop;
  // GUI-serialized commands return the authoritative applied value, including
  // rejection. Publication may be queued; a requested value is never treated as
  // confirmation.
  std::function<float(float)> ambient, roughness;
  std::function<bool(bool)> normalMapping;
  std::function<void(RenderError)> invalidated;
  // E3: Edit menu / Ctrl+Z / Ctrl+Shift+Z (Ctrl+Y). Domain history lives in session.
  std::function<void()> undo;
  std::function<void()> redo;
  std::function<bool()> canUndo;
  std::function<bool()> canRedo;
};
class ViewportHost : public QWidget {
public:
  explicit ViewportHost(UiCommands, QWidget *parent = nullptr);
  void showCamera(CameraState);
  void wheelSteps(double);
  void resetCamera(lr::core::Aabb const &);
  void showFrame(OwnedFrame);
  QImage readbackImage() const;
  std::shared_ptr<NativeHostLease> createNativeLease();
  void retireNative();
  QWindow *nativeWindow() const {
    return native_ ? native_->nativeWindow() : nullptr;
  }
  ~ViewportHost() override;

protected:
  bool eventFilter(QObject *, QEvent *) override;
  void mousePressEvent(QMouseEvent *) override;
  void mouseMoveEvent(QMouseEvent *) override;
  void wheelEvent(QWheelEvent *) override;
  void paintEvent(QPaintEvent *) override;
  void resizeEvent(QResizeEvent *) override;
  void showEvent(QShowEvent *) override;
  void hideEvent(QHideEvent *) override;

private:
  UiCommands commands_;
  CameraState camera_;
  QPoint drag_;
  QImage image_;
  std::unique_ptr<NativeSurfaceFactory> native_;
  bool nativeWasExposed_ = false;
  void notifyExtent();
};
} // namespace demo
