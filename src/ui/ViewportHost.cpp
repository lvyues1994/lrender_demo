#include "ViewportHost.h"
#include "presentation/ReadbackAdapter.h"
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>
namespace demo {
ViewportHost::ViewportHost(UiCommands c, QWidget *p)
    : QWidget(p), commands_(std::move(c)) {
  setMinimumSize(1, 1);
  setFocusPolicy(Qt::StrongFocus);
}
void ViewportHost::showCamera(CameraState c) { camera_ = c; }
void ViewportHost::wheelSteps(double steps) {
  auto &c = camera_.camera;
  auto d = c.position - c.target;
  auto distance = tc::length(d);
  auto next = std::clamp(double(distance) *
                             std::exp(std::clamp(-steps * .15, -50., 50.)),
                         double(c.nearPlane) * 2, double(c.farPlane) * .8);
  c.position = c.target + tc::normalize(d) * float(next);
  if (commands_.camera)
    commands_.camera(camera_);
}
void ViewportHost::resetCamera(lr::core::Aabb const &b) {
  auto &c = camera_.camera;
  c.target = (b.min + b.max) * .5f;
  auto r = std::max(tc::length(b.max - b.min) * .5f, c.nearPlane * 2);
  c.position =
      c.target + lr::core::vec3{0, 0, r / std::tan(c.verticalFov * .5f) * 1.2f};
  if (commands_.camera)
    commands_.camera(camera_);
}
void ViewportHost::showFrame(OwnedFrame f) {
  if (f.image)
    image_ = ReadbackAdapter::copyImage(*f.image);
  update();
}
QImage ViewportHost::readbackImage() const { return image_; }
void ViewportHost::mousePressEvent(QMouseEvent *e) { drag_ = e->pos(); }
void ViewportHost::mouseMoveEvent(QMouseEvent *e) {
  if (!(e->buttons() & Qt::LeftButton))
    return;
  auto delta = e->pos() - drag_;
  drag_ = e->pos();
  auto &c = camera_.camera;
  auto offset = c.position - c.target;
  auto distance = tc::length(offset);
  auto yaw = std::atan2(offset.x, offset.z) - delta.x() * .01f;
  auto pitch = std::clamp(
      std::asin(std::clamp(offset.y / distance, -1.f, 1.f)) + delta.y() * .01f,
      -1.55f, 1.55f);
  c.position = c.target + lr::core::vec3{std::sin(yaw) * std::cos(pitch),
                                         std::sin(pitch),
                                         std::cos(yaw) * std::cos(pitch)} *
                              distance;
  if (commands_.camera)
    commands_.camera(camera_);
}
void ViewportHost::wheelEvent(QWheelEvent *e) {
  wheelSteps(e->angleDelta().y() / 120.);
}
void ViewportHost::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), QColor(24, 25, 30));
  if (!image_.isNull())
    p.drawImage(rect(), image_);
}
void ViewportHost::notifyExtent() {
  auto d = devicePixelRatioF();
  if (commands_.viewport)
    commands_.viewport(
        {{width(), height()}, {qRound(width() * d), qRound(height() * d)}, d},
        isVisible() && width() > 0 && height() > 0);
}
void ViewportHost::resizeEvent(QResizeEvent *) {
  if (native_)
    native_->setViewport(size());
  notifyExtent();
}
void ViewportHost::showEvent(QShowEvent *) { notifyExtent(); }
void ViewportHost::hideEvent(QHideEvent *) { notifyExtent(); }
ViewportHost::~ViewportHost() = default;
std::shared_ptr<NativeHostLease> ViewportHost::createNativeLease() {
  if (native_)
    retireNative();
  native_ = std::make_unique<NativeSurfaceFactory>(SurfaceCreator{}, this);
  native_->setViewport(size());
  native_->nativeWindow()->installEventFilter(this);
  nativeWasExposed_ = native_->exposed();
  auto lease = native_->lease();
  lease->policy->setInvalidationHandler([this](auto) {
    if (commands_.invalidated)
      commands_.invalidated({ErrorStage::Surface, true,
                             "Platform: native surface lost",
                             "retire old lease before reconstruction"});
  });
  return lease;
}
void ViewportHost::retireNative() {
  if (!native_)
    return;
  auto policy = native_->lease()->policy;
  if (!policy->canRelease())
    throw std::logic_error("host retirement before presenter cleanup");
  policy->setInvalidationHandler({});
  native_->nativeWindow()->removeEventFilter(this);
  native_->releaseHost();
  native_.reset();
  nativeWasExposed_ = false;
  update();
}
bool ViewportHost::eventFilter(QObject *object, QEvent *event) {
  if (native_ && object == native_->nativeWindow()) {
    if (event->type() == QEvent::MouseButtonPress) {
      mousePressEvent(static_cast<QMouseEvent *>(event));
      return true;
    }
    if (event->type() == QEvent::MouseMove) {
      mouseMoveEvent(static_cast<QMouseEvent *>(event));
      return true;
    }
    if (event->type() == QEvent::Wheel) {
      wheelEvent(static_cast<QWheelEvent *>(event));
      return true;
    }
    if (event->type() == QEvent::Expose) {
      bool exposed = native_->exposed();
      if (exposed)
        nativeWasExposed_ = true;
      if (nativeWasExposed_ && commands_.viewport) {
        auto d = devicePixelRatioF();
        auto pixels = native_->physicalExtent();
        commands_.viewport(
            {{width(), height()}, {pixels.width(), pixels.height()}, d},
            exposed && isVisible() && width() > 0 && height() > 0 &&
                pixels.width() > 0 && pixels.height() > 0);
      }
    }
  }
  return QWidget::eventFilter(object, event);
}
} // namespace demo
