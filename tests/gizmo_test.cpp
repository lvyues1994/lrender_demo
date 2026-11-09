#include "editor/GizmoMath.h"

#include <doctest/doctest.h>

#include <cmath>

using demo::EditorEntityId;
using demo::ViewportExtent;
using demo::editor::applyWorldTranslationDelta;
using demo::editor::cameraRay;
using demo::editor::hitTranslateHandle;
using demo::editor::intersectAabb;
using demo::editor::localToWorld;
using demo::editor::pickClosest;
using demo::editor::projectToPixel;
using demo::editor::translateDeltaAlongAxis;
using demo::editor::worldToLocal;

namespace {
lr::core::CameraDesc lookCam() {
  lr::core::CameraDesc c{};
  c.position = {0, 0, 5};
  c.target = {0, 0, 0};
  c.up = {0, 1, 0};
  c.verticalFov = 1.0f;
  c.nearPlane = 0.1f;
  c.farPlane = 100.f;
  return c;
}
ViewportExtent extent() { return {{200, 200}, {200, 200}, 1.0}; }
lr::core::vec3 nrm(lr::core::vec3 v) {
  float L = tc::length(v);
  return v / L;
}
} // namespace

TEST_CASE("gizmo/camera-ray-center-hits-origin") {
  auto ray = cameraRay(lookCam(), extent(), 100.f, 100.f);
  CHECK(ray.origin.z == doctest::Approx(5.f));
  CHECK(ray.direction.z == doctest::Approx(-1.f).epsilon(1e-3));
  CHECK(std::fabs(ray.direction.x) < 1e-3f);
  CHECK(std::fabs(ray.direction.y) < 1e-3f);
}

TEST_CASE("gizmo/aabb-ray-hit-and-miss") {
  demo::editor::Ray ray{{0, 0, 5}, {0, 0, -1}};
  lr::core::Aabb box{{-1, -1, -1}, {1, 1, 1}};
  auto t = intersectAabb(ray, box);
  REQUIRE(t);
  CHECK(*t == doctest::Approx(4.f));
  demo::editor::Ray miss{{3, 0, 5}, {0, 0, -1}};
  CHECK_FALSE(intersectAabb(miss, box).has_value());
}

TEST_CASE("gizmo/pick-closest-of-two") {
  demo::editor::Ray ray{{0, 0, 5}, {0, 0, -1}};
  std::vector<std::pair<EditorEntityId, lr::core::Aabb>> boxes;
  boxes.push_back({EditorEntityId{1}, {{-0.5f, -0.5f, -2.f}, {0.5f, 0.5f, -1.f}}});
  boxes.push_back({EditorEntityId{2}, {{-0.5f, -0.5f, 0.f}, {0.5f, 0.5f, 1.f}}});
  auto hit = pickClosest(ray, boxes);
  REQUIRE(hit);
  CHECK(hit->id == EditorEntityId{2});
}

TEST_CASE("gizmo/project-roundtrip-center") {
  auto pix = projectToPixel(lookCam(), extent(), {0, 0, 0});
  REQUIRE(pix);
  CHECK(pix->x == doctest::Approx(100.f).epsilon(1e-2));
  CHECK(pix->y == doctest::Approx(100.f).epsilon(1e-2));
}

TEST_CASE("gizmo/translate-delta-along-x") {
  lr::core::vec3 origin{0, 0, 0};
  lr::core::vec3 axis{1, 0, 0};
  // Rays from (0,2,2) and (3,2,2) toward -Y cross the X axis vicinity.
  demo::editor::Ray r0{{0, 2, 2}, nrm({0, -1, -1})};
  demo::editor::Ray r1{{3, 2, 2}, nrm({0, -1, -1})};
  auto delta = translateDeltaAlongAxis(r0, r1, origin, axis);
  // Closest-axis param difference is signed along the axis direction.
  CHECK(std::fabs(delta.x) == doctest::Approx(3.f).epsilon(1e-3));
  CHECK(delta.x == doctest::Approx(translateDeltaAlongAxis(r1, r0, origin, axis).x * -1.f));
  CHECK(std::fabs(delta.y) < 1e-3f);
  CHECK(std::fabs(delta.z) < 1e-3f);
}

TEST_CASE("gizmo/hit-translate-handle-near-axis") {
  auto cam = lookCam();
  auto ext = extent();
  lr::core::vec3 tip{1.5f, 0.f, 0.f};
  auto tipPix = projectToPixel(cam, ext, tip);
  REQUIRE(tipPix);
  // Exact ray through the handle tip (not only cameraRay approximation).
  demo::editor::Ray rayX{cam.position, nrm(tip - cam.position)};
  auto hit = hitTranslateHandle(rayX, {0, 0, 0}, {1, 0, 0}, 2.f, cam, ext, 15.f);
  CHECK(hit.axis == demo::editor::GizmoAxis::X);
  demo::editor::Ray missRay{cam.position, nrm(lr::core::vec3{-2.f, 2.f, -5.f})};
  auto miss = hitTranslateHandle(missRay, {0, 0, 0}, {1, 0, 0}, 2.f, cam, ext, 8.f);
  CHECK(miss.axis == demo::editor::GizmoAxis::None);
}

TEST_CASE("gizmo/world-to-local-with-parent-translation") {
  lr::core::Transform parent{};
  parent.translation = {10.f, 0.f, 0.f};
  auto parentWorld = lr::core::matrixOf(parent);
  lr::core::Transform world{};
  world.translation = {12.f, 3.f, 0.f};
  auto local = worldToLocal(world, parentWorld);
  CHECK(local.translation.x == doctest::Approx(2.f));
  CHECK(local.translation.y == doctest::Approx(3.f));
  auto round = localToWorld(local, parentWorld);
  CHECK(round[3][0] == doctest::Approx(12.f));
  CHECK(round[3][1] == doctest::Approx(3.f));
}

TEST_CASE("gizmo/apply-world-delta-under-parent") {
  lr::core::Transform parent{};
  parent.translation = {5.f, 0.f, 0.f};
  auto parentWorld = lr::core::matrixOf(parent);
  lr::core::Transform local{};
  local.translation = {1.f, 2.f, 0.f};
  auto next = applyWorldTranslationDelta(local, parentWorld, {0.f, 4.f, 0.f});
  CHECK(next.translation.x == doctest::Approx(1.f));
  CHECK(next.translation.y == doctest::Approx(6.f));
}

#include "ui/ViewportHost.h"
#include <QApplication>
#include <QCoreApplication>
#include <QMouseEvent>

using demo::CameraState;
using demo::EditorEntitySnapshot;
using demo::SessionSnapshot;

TEST_CASE("gizmo/viewport-click-picks-entity") {
  REQUIRE(QApplication::instance());
  EditorEntityId picked{};
  demo::UiCommands cmd;
  demo::ViewportHost host(cmd);
  host.resize(200, 200);
  host.show();
  host.setPickHandler([&](EditorEntityId id) { picked = id; });
  CameraState cam;
  cam.camera = lookCam();
  host.showCamera(cam);
  auto snap = std::make_shared<SessionSnapshot>();
  snap->editorSequence = 1;
  EditorEntitySnapshot ent;
  ent.id = EditorEntityId{42};
  ent.name = "box";
  ent.local = lr::core::Transform::identity();
  ent.worldMatrix = lr::core::mat4{1.f};
  ent.worldBounds = lr::core::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
  snap->entities.push_back(ent);
  demo::editor::SelectionState sel;
  host.showSnapshot(snap, sel);
  // Click center (should ray toward origin and hit unit box).
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(100, 100), Qt::LeftButton, Qt::LeftButton,
                    Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, QPointF(100, 100), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
  QCoreApplication::sendEvent(&host, &press);
  QCoreApplication::sendEvent(&host, &release);
  CHECK(picked == EditorEntityId{42});
}

TEST_CASE("gizmo/viewport-drag-emits-merged-transform") {
  REQUIRE(QApplication::instance());
  int transforms = 0;
  int begins = 0, ends = 0;
  lr::core::Transform last{};
  demo::UiCommands cmd;
  cmd.beginEditMerge = [&] { ++begins; };
  cmd.endEditMerge = [&] { ++ends; };
  cmd.setEntityTransform = [&](EditorEntityId, lr::core::Transform xf) {
    ++transforms;
    last = xf;
  };
  demo::ViewportHost host(cmd);
  host.resize(200, 200);
  host.show();
  CameraState cam;
  cam.camera = lookCam();
  host.showCamera(cam);
  auto snap = std::make_shared<SessionSnapshot>();
  EditorEntitySnapshot ent;
  ent.id = EditorEntityId{7};
  ent.name = "g";
  ent.local = lr::core::Transform::identity();
  ent.worldMatrix = lr::core::mat4{1.f};
  ent.worldBounds = lr::core::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
  snap->entities.push_back(ent);
  demo::editor::SelectionState sel;
  sel.setPrimary(ent.id);
  host.showSnapshot(snap, sel);
  // Press on projected +X handle tip, drag right, release.
  auto tip = projectToPixel(cam.camera, {{200, 200}, {200, 200}, 1.0}, {1.2f, 0.f, 0.f});
  REQUIRE(tip);
  QPoint tipPt(int(tip->x), int(tip->y));
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(tipPt), Qt::LeftButton, Qt::LeftButton,
                    Qt::NoModifier);
  QCoreApplication::sendEvent(&host, &press);
  CHECK(begins >= 1);
  QMouseEvent move(QEvent::MouseMove, QPointF(tipPt + QPoint(40, 0)), Qt::LeftButton, Qt::LeftButton,
                   Qt::NoModifier);
  QCoreApplication::sendEvent(&host, &move);
  QMouseEvent release(QEvent::MouseButtonRelease, QPointF(tipPt + QPoint(40, 0)), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&host, &release);
  CHECK(transforms >= 1);
  CHECK(ends >= 1);
}

TEST_CASE("gizmo/rotate-angle-around-y-from-plane-hits") {
  lr::core::vec3 origin{0, 0, 0};
  lr::core::vec3 axis{0, 1, 0};
  // Hit plane y=0 at (1,0,0) then at (0,0,-1) → +90° about +Y (right-hand).
  demo::editor::Ray r0{{1, 5, 0}, nrm({0, -1, 0})};
  demo::editor::Ray r1{{0, 5, -1}, nrm({0, -1, 0})};
  auto ang = demo::editor::rotateAngleAroundAxis(r0, r1, origin, axis);
  REQUIRE(ang);
  CHECK(*ang == doctest::Approx(lr::core::kPi * 0.5f).epsilon(1e-3));
}

TEST_CASE("gizmo/apply-world-rotation-under-parent") {
  lr::core::Transform parent{};
  parent.translation = {10.f, 0.f, 0.f};
  auto parentWorld = lr::core::matrixOf(parent);
  lr::core::Transform local = lr::core::Transform::identity();
  local.translation = {1.f, 0.f, 0.f};
  // Gizmo rotates about the entity origin: world translation stays, orientation gains +90° Y.
  auto next = demo::editor::applyWorldRotationDelta(local, parentWorld, {0, 1, 0},
                                                   lr::core::kPi * 0.5f);
  auto world = lr::core::decomposed(demo::editor::localToWorld(next, parentWorld));
  CHECK(world.translation.x == doctest::Approx(11.f).epsilon(1e-3));
  CHECK(world.translation.z == doctest::Approx(0.f).epsilon(1e-3));
  // Local +X basis in world should now point near -Z after +Y 90° CCW.
  auto R = lr::core::rotationMatrix(world.rotation);
  lr::core::vec3 xAxis{R[0][0], R[0][1], R[0][2]};
  CHECK(xAxis.x == doctest::Approx(0.f).epsilon(1e-3));
  CHECK(xAxis.z == doctest::Approx(-1.f).epsilon(1e-3));
  // Parent translation-only → local translation unchanged.
  CHECK(next.translation.x == doctest::Approx(1.f).epsilon(1e-3));
}

TEST_CASE("gizmo/scale-factor-along-axis-and-clamp") {
  lr::core::vec3 origin{0, 0, 0};
  lr::core::vec3 axis{1, 0, 0};
  demo::editor::Ray r0{{2, 2, 0}, nrm({0, -1, 0})}; // hits x≈2
  demo::editor::Ray r1{{4, 2, 0}, nrm({0, -1, 0})}; // hits x≈4 → factor 2
  auto factor = demo::editor::scaleFactorAlongAxis(r0, r1, origin, axis);
  CHECK(factor == doctest::Approx(2.f).epsilon(1e-3));

  lr::core::Transform local = lr::core::Transform::identity();
  auto grown = demo::editor::applyScaleDelta(local, lr::core::mat4{1.f}, axis, 2.f,
                                             /*uniform=*/false);
  CHECK(grown.scale.x == doctest::Approx(2.f));
  CHECK(grown.scale.y == doctest::Approx(1.f));

  auto crushed = demo::editor::applyScaleDelta(local, lr::core::mat4{1.f}, axis, 0.f,
                                               /*uniform=*/false);
  CHECK(crushed.scale.x >= demo::editor::kMinScale);
  CHECK(crushed.scale.x > 0.f);

  auto neg = demo::editor::applyScaleDelta(local, lr::core::mat4{1.f}, axis, -2.f,
                                           /*uniform=*/false);
  CHECK(neg.scale.x >= demo::editor::kMinScale);

  auto uni = demo::editor::applyScaleDelta(local, lr::core::mat4{1.f}, axis, 3.f,
                                           /*uniform=*/true);
  CHECK(uni.scale.x == doctest::Approx(3.f));
  CHECK(uni.scale.y == doctest::Approx(3.f));
  CHECK(uni.scale.z == doctest::Approx(3.f));
}

TEST_CASE("gizmo/hit-rotate-ring-near-plane") {
  auto cam = lookCam();
  cam.position = {0.f, 0.f, 5.f};
  cam.target = {0.f, 0.f, 0.f};
  auto ext = extent();
  // Ring in XY plane (about Z) so the camera ray is not parallel to the plane.
  lr::core::vec3 onRing{1.f, 0.f, 0.f};
  demo::editor::Ray ray{cam.position, nrm(onRing - cam.position)};
  auto hit = demo::editor::hitRotateHandle(ray, {0, 0, 0}, {0, 0, 1}, 1.f, cam, ext, 20.f);
  CHECK(hit.axis != demo::editor::GizmoAxis::None);
}

TEST_CASE("gizmo/viewport-rotate-drag-emits-merged-transform") {
  REQUIRE(QApplication::instance());
  int transforms = 0, begins = 0, ends = 0;
  lr::core::Transform last{};
  demo::UiCommands cmd;
  cmd.beginEditMerge = [&] { ++begins; };
  cmd.endEditMerge = [&] { ++ends; };
  cmd.setEntityTransform = [&](EditorEntityId, lr::core::Transform xf) {
    ++transforms;
    last = xf;
  };
  demo::ViewportHost host(cmd);
  host.resize(200, 200);
  host.show();
  host.setGizmoMode(demo::editor::GizmoMode::Rotate);
  CameraState cam;
  cam.camera = lookCam();
  host.showCamera(cam);
  auto snap = std::make_shared<SessionSnapshot>();
  EditorEntitySnapshot ent;
  ent.id = EditorEntityId{8};
  ent.name = "r";
  ent.local = lr::core::Transform::identity();
  ent.worldMatrix = lr::core::mat4{1.f};
  ent.worldBounds = lr::core::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
  snap->entities.push_back(ent);
  demo::editor::SelectionState sel;
  sel.setPrimary(ent.id);
  host.showSnapshot(snap, sel);
  // Press on +X of Z-ring (XY plane), drag toward +Y.
  auto tip = projectToPixel(cam.camera, {{200, 200}, {200, 200}, 1.0}, {1.f, 0.f, 0.f});
  REQUIRE(tip);
  QPoint tipPt(int(tip->x), int(tip->y));
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(tipPt), Qt::LeftButton, Qt::LeftButton,
                    Qt::NoModifier);
  QCoreApplication::sendEvent(&host, &press);
  CHECK(begins >= 1);
  auto tip2 = projectToPixel(cam.camera, {{200, 200}, {200, 200}, 1.0}, {0.f, 1.f, 0.f});
  REQUIRE(tip2);
  QMouseEvent move(QEvent::MouseMove, QPointF(int(tip2->x), int(tip2->y)), Qt::LeftButton,
                   Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&host, &move);
  QMouseEvent release(QEvent::MouseButtonRelease, QPointF(int(tip2->x), int(tip2->y)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&host, &release);
  CHECK(transforms >= 1);
  CHECK(ends >= 1);
}

TEST_CASE("gizmo/viewport-scale-drag-clamps-positive") {
  REQUIRE(QApplication::instance());
  int transforms = 0;
  lr::core::Transform last = lr::core::Transform::identity();
  demo::UiCommands cmd;
  cmd.beginEditMerge = [] {};
  cmd.endEditMerge = [] {};
  cmd.setEntityTransform = [&](EditorEntityId, lr::core::Transform xf) {
    ++transforms;
    last = xf;
  };
  demo::ViewportHost host(cmd);
  host.resize(200, 200);
  host.show();
  host.setGizmoMode(demo::editor::GizmoMode::Scale);
  CameraState cam;
  cam.camera = lookCam();
  host.showCamera(cam);
  auto snap = std::make_shared<SessionSnapshot>();
  EditorEntitySnapshot ent;
  ent.id = EditorEntityId{9};
  ent.name = "s";
  ent.local = lr::core::Transform::identity();
  ent.worldMatrix = lr::core::mat4{1.f};
  ent.worldBounds = lr::core::Aabb{{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
  snap->entities.push_back(ent);
  demo::editor::SelectionState sel;
  sel.setPrimary(ent.id);
  host.showSnapshot(snap, sel);
  auto tip = projectToPixel(cam.camera, {{200, 200}, {200, 200}, 1.0}, {1.2f, 0.f, 0.f});
  REQUIRE(tip);
  QPoint tipPt(int(tip->x), int(tip->y));
  QMouseEvent press(QEvent::MouseButtonPress, QPointF(tipPt), Qt::LeftButton, Qt::LeftButton,
                    Qt::NoModifier);
  QCoreApplication::sendEvent(&host, &press);
  // Drag toward origin → factor < 1 but must stay >= kMinScale; also drag past origin.
  QMouseEvent move(QEvent::MouseMove, QPointF(tipPt + QPoint(-80, 0)), Qt::LeftButton,
                   Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&host, &move);
  QMouseEvent release(QEvent::MouseButtonRelease, QPointF(tipPt + QPoint(-80, 0)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QCoreApplication::sendEvent(&host, &release);
  CHECK(transforms >= 1);
  CHECK(last.scale.x >= demo::editor::kMinScale);
  CHECK(last.scale.x > 0.f);
  CHECK(last.scale.y > 0.f);
  CHECK(last.scale.z > 0.f);
}

TEST_CASE("gizmo/mode-shortcuts-wer-and-space-toggle-api") {
  REQUIRE(QApplication::instance());
  demo::UiCommands cmd;
  demo::ViewportHost host(cmd);
  CHECK(host.gizmoMode() == demo::editor::GizmoMode::Translate);
  host.setGizmoMode(demo::editor::GizmoMode::Rotate);
  CHECK(host.gizmoMode() == demo::editor::GizmoMode::Rotate);
  host.setGizmoMode(demo::editor::GizmoMode::Scale);
  CHECK(host.gizmoMode() == demo::editor::GizmoMode::Scale);
  host.setGizmoSpace(demo::editor::GizmoSpace::World);
  CHECK(host.gizmoSpace() == demo::editor::GizmoSpace::World);
  host.setGizmoSpace(demo::editor::GizmoSpace::Local);
  CHECK(host.gizmoSpace() == demo::editor::GizmoSpace::Local);
}
