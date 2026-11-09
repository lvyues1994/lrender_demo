#include "ViewportHost.h"
#include "presentation/ReadbackAdapter.h"
#include <lr/core/Primitives.h>
#include <QMouseEvent>
#include <QPainter>
#include <QtGui/QPolygonF>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>
namespace demo {
namespace {
constexpr int kClickSlop = 4;
constexpr float kHandleLen = 1.25f;
constexpr float kRingRadius = 1.0f;
constexpr float kHandlePixels = 14.f;

editor::GizmoAxis axisFromIndex(int i) {
  switch (i) {
  case 0:
    return editor::GizmoAxis::X;
  case 1:
    return editor::GizmoAxis::Y;
  case 2:
    return editor::GizmoAxis::Z;
  default:
    return editor::GizmoAxis::None;
  }
}
} // namespace

ViewportHost::ViewportHost(UiCommands c, QWidget *p)
    : QWidget(p), commands_(std::move(c)) {
  setMinimumSize(1, 1);
  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
}
void ViewportHost::showCamera(CameraState c) { camera_ = c; update(); }
void ViewportHost::showSnapshot(SessionSnapshotPtr snap,
                                editor::SelectionState const &selection) {
  snap_ = std::move(snap);
  primary_ = selection.primary().value_or(EditorEntityId{});
  notifyGizmoVisuals();
  // Session blocked/Failed: cancel in-progress gizmo merge safely.
  if (snap_ && (snap_->life == LifeState::Failed || snap_->life == LifeState::Stopping ||
                snap_->life == LifeState::Stopped))
    cancelGizmoDrag();
  update();
}
void ViewportHost::notifyGizmoVisuals() {
  if (!commands_.syncGizmoVisuals)
    return;
  commands_.syncGizmoVisuals(primary_, static_cast<std::uint32_t>(gizmoMode_),
                             static_cast<std::uint32_t>(gizmoSpace_));
}
void ViewportHost::setGizmoMode(editor::GizmoMode mode) {
  gizmoMode_ = mode;
  notifyGizmoVisuals();
  update();
}
void ViewportHost::setGizmoSpace(editor::GizmoSpace space) {
  gizmoSpace_ = space;
  notifyGizmoVisuals();
  update();
}
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

ViewportExtent ViewportHost::currentExtent() const {
  auto d = devicePixelRatioF();
  lr::core::ivec2 physical{qRound(width() * d), qRound(height() * d)};
  if (native_) {
    auto pixels = native_->physicalExtent();
    if (pixels.width() > 0 && pixels.height() > 0)
      physical = {pixels.width(), pixels.height()};
  }
  return {{width(), height()}, physical, d};
}

lr::core::vec3 ViewportHost::axisDirection(editor::GizmoAxis axis) const {
  lr::core::vec3 local{1, 0, 0};
  if (axis == editor::GizmoAxis::Y)
    local = {0, 1, 0};
  else if (axis == editor::GizmoAxis::Z)
    local = {0, 0, 1};
  else if (axis == editor::GizmoAxis::Screen || axis == editor::GizmoAxis::None)
    return {1, 0, 0};
  if (gizmoSpace_ == editor::GizmoSpace::World || !snap_ || !primary_.isValid())
    return local;
  for (auto const &e : snap_->entities) {
    if (e.id != primary_)
      continue;
    // Local axes from worldMatrix basis vectors.
    auto const &m = e.worldMatrix;
    if (axis == editor::GizmoAxis::X)
      return tc::normalize(lr::core::vec3{m[0][0], m[0][1], m[0][2]});
    if (axis == editor::GizmoAxis::Y)
      return tc::normalize(lr::core::vec3{m[1][0], m[1][1], m[1][2]});
    return tc::normalize(lr::core::vec3{m[2][0], m[2][1], m[2][2]});
  }
  return local;
}

bool ViewportHost::hitGizmo(QPoint const &pos, editor::GizmoAxis *outAxis) const {
  if (!primary_.isValid() || !snap_)
    return false;
  lr::core::vec3 origin{};
  bool found = false;
  for (auto const &e : snap_->entities) {
    if (e.id != primary_)
      continue;
    origin = {e.worldMatrix[3][0], e.worldMatrix[3][1], e.worldMatrix[3][2]};
    found = true;
    break;
  }
  if (!found)
    return false;
  auto ext = currentExtent();
  auto ray = editor::cameraRay(camera_.camera, ext, float(pos.x()), float(pos.y()));
  float best = 1e9f;
  editor::GizmoAxis bestAxis = editor::GizmoAxis::None;
  for (int i = 0; i < 3; ++i) {
    auto ax = axisFromIndex(i);
    auto dir = axisDirection(ax);
    editor::GizmoHandleHit hit{};
    if (gizmoMode_ == editor::GizmoMode::Translate)
      hit = editor::hitTranslateHandle(ray, origin, dir, kHandleLen, camera_.camera, ext,
                                       kHandlePixels);
    else if (gizmoMode_ == editor::GizmoMode::Rotate)
      hit = editor::hitRotateHandle(ray, origin, dir, kRingRadius, camera_.camera, ext,
                                    kHandlePixels);
    else
      hit = editor::hitScaleHandle(ray, origin, dir, kHandleLen, camera_.camera, ext,
                                   kHandlePixels);
    if (hit.axis == editor::GizmoAxis::None)
      continue;
    if (hit.t < best) {
      best = hit.t;
      bestAxis = ax;
    }
  }
  // Uniform scale: small center handle.
  if (gizmoMode_ == editor::GizmoMode::Scale) {
    auto center = editor::hitTranslateHandle(ray, origin, {1, 0, 0}, 0.35f, camera_.camera, ext,
                                             kHandlePixels * 1.2f);
    // Prefer a dedicated screen-axis if ray is close to origin in pixels.
    auto op = editor::projectToPixel(camera_.camera, ext, origin);
    if (op) {
      float dx = float(pos.x()) - op->x, dy = float(pos.y()) - op->y;
      if (std::sqrt(dx * dx + dy * dy) <= kHandlePixels) {
        bestAxis = editor::GizmoAxis::Screen;
      }
    }
    (void)center;
  }
  if (bestAxis == editor::GizmoAxis::None)
    return false;
  if (outAxis)
    *outAxis = bestAxis;
  return true;
}

void ViewportHost::pickAt(QPoint const &pos) {
  if (!onPick_ || !snap_)
    return;
  auto ext = currentExtent();
  auto ray = editor::cameraRay(camera_.camera, ext, float(pos.x()), float(pos.y()));
  std::vector<std::pair<EditorEntityId, lr::core::Aabb>> boxes;
  boxes.reserve(snap_->entities.size());
  for (auto const &e : snap_->entities) {
    if (!e.worldBounds)
      continue;
    boxes.emplace_back(e.id, *e.worldBounds);
  }
  auto hit = editor::pickClosest(ray, boxes);
  onPick_(hit ? hit->id : EditorEntityId{});
}

void ViewportHost::cancelGizmoDrag() {
  if (mergeOpen_ && commands_.endEditMerge) {
    commands_.endEditMerge();
    mergeOpen_ = false;
  }
  gizmoDragging_ = false;
  dragAxis_ = editor::GizmoAxis::None;
}

void ViewportHost::mousePressEvent(QMouseEvent *e) {
  pressPos_ = e->pos();
  drag_ = e->pos();
  pressing_ = true;
  orbiting_ = false;
  gizmoDragging_ = false;
  if (e->button() == Qt::LeftButton) {
    editor::GizmoAxis axis = editor::GizmoAxis::None;
    if (hitGizmo(e->pos(), &axis)) {
      gizmoDragging_ = true;
      dragAxis_ = axis;
      dragUniform_ = (axis == editor::GizmoAxis::Screen) ||
                     (gizmoMode_ == editor::GizmoMode::Scale && axis == editor::GizmoAxis::Screen);
      auto ext = currentExtent();
      pressRay_ = editor::cameraRay(camera_.camera, ext, float(e->pos().x()), float(e->pos().y()));
      dragAxisDir_ = axisDirection(axis);
      dragParentWorld_ = lr::core::mat4{1.f};
      for (auto const &ent : snap_ ? snap_->entities
                                   : std::vector<EditorEntitySnapshot>{}) {
        if (ent.id != primary_)
          continue;
        dragLocal0_ = ent.local;
        dragOrigin_ = {ent.worldMatrix[3][0], ent.worldMatrix[3][1], ent.worldMatrix[3][2]};
        // Parent world = world * inv(local)
        dragParentWorld_ = ent.worldMatrix * tc::inverse(lr::core::matrixOf(ent.local));
        break;
      }
      if (commands_.beginEditMerge) {
        commands_.beginEditMerge();
        mergeOpen_ = true;
      }
      return;
    }
  }
}
void ViewportHost::mouseMoveEvent(QMouseEvent *e) {
  if (!pressing_)
    return;
  if (gizmoDragging_) {
    if (!commands_.setEntityTransform || !primary_.isValid())
      return;
    auto ext = currentExtent();
    auto ray1 = editor::cameraRay(camera_.camera, ext, float(e->pos().x()), float(e->pos().y()));
    lr::core::Transform next = dragLocal0_;
    if (gizmoMode_ == editor::GizmoMode::Translate) {
      auto worldDelta =
          editor::translateDeltaAlongAxis(pressRay_, ray1, dragOrigin_, dragAxisDir_);
      next = editor::applyWorldTranslationDelta(dragLocal0_, dragParentWorld_, worldDelta);
    } else if (gizmoMode_ == editor::GizmoMode::Rotate) {
      auto ang = editor::rotateAngleAroundAxis(pressRay_, ray1, dragOrigin_, dragAxisDir_);
      if (ang)
        next = editor::applyWorldRotationDelta(dragLocal0_, dragParentWorld_, dragAxisDir_, *ang);
    } else {
      auto axis = dragUniform_ ? lr::core::vec3{1, 0, 0} : dragAxisDir_;
      float factor = editor::scaleFactorAlongAxis(pressRay_, ray1, dragOrigin_, axis);
      next = editor::applyScaleDelta(dragLocal0_, dragParentWorld_, axis, factor, dragUniform_);
    }
    commands_.setEntityTransform(primary_, next);
    update();
    return;
  }
  auto delta = e->pos() - pressPos_;
  if (!orbiting_ && (std::abs(delta.x()) > kClickSlop || std::abs(delta.y()) > kClickSlop))
    orbiting_ = true;
  if (!orbiting_ || !(e->buttons() & Qt::LeftButton))
    return;
  auto step = e->pos() - drag_;
  drag_ = e->pos();
  auto &c = camera_.camera;
  auto offset = c.position - c.target;
  auto distance = tc::length(offset);
  auto yaw = std::atan2(offset.x, offset.z) - step.x() * .01f;
  auto pitch = std::clamp(
      std::asin(std::clamp(offset.y / distance, -1.f, 1.f)) + step.y() * .01f,
      -1.55f, 1.55f);
  c.position = c.target + lr::core::vec3{std::sin(yaw) * std::cos(pitch),
                                         std::sin(pitch),
                                         std::cos(yaw) * std::cos(pitch)} *
                              distance;
  if (commands_.camera)
    commands_.camera(camera_);
}
void ViewportHost::mouseReleaseEvent(QMouseEvent *e) {
  if (e->button() != Qt::LeftButton) {
    pressing_ = false;
    return;
  }
  if (gizmoDragging_) {
    cancelGizmoDrag();
    pressing_ = false;
    return;
  }
  if (pressing_ && !orbiting_)
    pickAt(e->pos());
  pressing_ = false;
  orbiting_ = false;
}
void ViewportHost::wheelEvent(QWheelEvent *e) {
  wheelSteps(e->angleDelta().y() / 120.);
}

void ViewportHost::paintGizmo(QPainter &p) {
  if (!primary_.isValid() || !snap_)
    return;
  lr::core::vec3 origin{};
  bool found = false;
  for (auto const &e : snap_->entities) {
    if (e.id != primary_)
      continue;
    origin = {e.worldMatrix[3][0], e.worldMatrix[3][1], e.worldMatrix[3][2]};
    found = true;
    break;
  }
  if (!found)
    return;
  auto ext = currentExtent();
  auto o = editor::projectToPixel(camera_.camera, ext, origin);
  if (!o)
    return;
  QColor colors[3] = {QColor(220, 60, 60), QColor(60, 200, 80), QColor(60, 100, 230)};
  for (int i = 0; i < 3; ++i) {
    auto ax = axisFromIndex(i);
    auto dir = axisDirection(ax);
    QPen pen(colors[i], dragAxis_ == ax ? 3.0 : 2.0);
    p.setPen(pen);
    p.setBrush(colors[i]);
    if (gizmoMode_ == editor::GizmoMode::Rotate) {
      // Approximate ring as 32-segment polyline in the plane ⊥ axis.
      lr::core::vec3 a = std::fabs(dir.y) < 0.9f ? tc::cross(dir, {0, 1, 0})
                                                 : tc::cross(dir, {1, 0, 0});
      a = tc::normalize(a);
      lr::core::vec3 b = tc::normalize(tc::cross(dir, a));
      QPolygonF poly;
      for (int s = 0; s <= 32; ++s) {
        float ang = float(s) / 32.f * 6.2831853f;
        auto pt = origin + (a * std::cos(ang) + b * std::sin(ang)) * kRingRadius;
        if (auto pix = editor::projectToPixel(camera_.camera, ext, pt))
          poly << QPointF(pix->x, pix->y);
      }
      if (poly.size() > 1)
        p.drawPolyline(poly);
    } else {
      auto tip = editor::projectToPixel(camera_.camera, ext, origin + dir * kHandleLen);
      if (!tip)
        continue;
      p.drawLine(QPointF(o->x, o->y), QPointF(tip->x, tip->y));
      if (gizmoMode_ == editor::GizmoMode::Scale)
        p.drawRect(QRectF(tip->x - 4, tip->y - 4, 8, 8));
      else
        p.drawEllipse(QPointF(tip->x, tip->y), 4, 4);
    }
  }
  if (gizmoMode_ == editor::GizmoMode::Scale) {
    p.setPen(QPen(QColor(230, 230, 80), 2.0));
    p.setBrush(QColor(230, 230, 80));
    p.drawEllipse(QPointF(o->x, o->y), 5, 5);
  }
}

void ViewportHost::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.fillRect(rect(), QColor(24, 25, 30));
  if (!image_.isNull())
    p.drawImage(rect(), image_);
  // Readback HUD. Shared path draws gizmos as World meshes (see RenderSession).
  if (!native_)
    paintGizmo(p);
}



void ViewportHost::notifyExtent() {
  auto d = devicePixelRatioF();
  if (!commands_.viewport)
    return;
  lr::core::ivec2 physical{qRound(width() * d), qRound(height() * d)};
  if (native_) {
    auto pixels = native_->physicalExtent();
    if (pixels.width() > 0 && pixels.height() > 0)
      physical = {pixels.width(), pixels.height()};
  }
  commands_.viewport({{width(), height()}, physical, d},
                     isVisible() && width() > 0 && height() > 0 &&
                         physical.x > 0 && physical.y > 0);
}
void ViewportHost::resizeEvent(QResizeEvent *) {
  if (native_)
    native_->setViewport(size());
  notifyExtent();
}
void ViewportHost::showEvent(QShowEvent *) { notifyExtent(); }
void ViewportHost::hideEvent(QHideEvent *) { notifyExtent(); }
ViewportHost::~ViewportHost() { cancelGizmoDrag(); }
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
    if (event->type() == QEvent::MouseButtonRelease) {
      mouseReleaseEvent(static_cast<QMouseEvent *>(event));
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
