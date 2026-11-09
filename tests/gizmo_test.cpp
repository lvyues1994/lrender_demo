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
