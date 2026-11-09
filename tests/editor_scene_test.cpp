#include "editor/EditorScene.h"

#include <doctest/doctest.h>
#include <lr/scene/World.h>

#include <QCoreApplication>

using demo::editor::EditorScene;
using demo::editor::Hierarchy;
using demo::editor::LocalTransform;
using demo::editor::Name;
using demo::editor::WorldLink;

TEST_CASE("editor/create-sync-creates-world-entity") {
  EditorScene scene;
  auto world = lr::scene::makeWorld({});
  auto e = scene.create("root", lr::core::Transform::identity());
  REQUIRE(scene.dirtyCount() == 1);
  scene.syncToWorld(*world);
  REQUIRE(scene.dirtyCount() == 0);
  auto const &link = scene.registry().get<WorldLink>(e);
  REQUIRE(world->isAlive(link.id));
  REQUIRE(world->entityCount() == 1);
}

TEST_CASE("editor/transform-dirty-sync-updates-world") {
  EditorScene scene;
  auto world = lr::scene::makeWorld({});
  auto e = scene.create("mover", {});
  scene.syncToWorld(*world);
  lr::core::Transform next{};
  next.translation = {1.f, 2.f, 3.f};
  scene.setLocalTransform(e, next);
  REQUIRE(scene.dirtyCount() == 1);
  scene.syncToWorld(*world);
  auto id = scene.registry().get<WorldLink>(e).id;
  auto got = world->transformOf(id);
  CHECK(got.translation.x == doctest::Approx(1.f));
  CHECK(got.translation.y == doctest::Approx(2.f));
  CHECK(got.translation.z == doctest::Approx(3.f));
}

TEST_CASE("editor/parent-sync-order") {
  EditorScene scene;
  auto world = lr::scene::makeWorld({});
  auto root = scene.create("root", {});
  auto child = scene.create("child", {}, root);
  scene.syncToWorld(*world);
  auto rootId = scene.registry().get<WorldLink>(root).id;
  auto childId = scene.registry().get<WorldLink>(child).id;
  auto parent = world->parentOf(childId);
  REQUIRE(parent);
  CHECK(*parent == rootId);
}

TEST_CASE("editor/destroy-sync-removes-world-entity") {
  EditorScene scene;
  auto world = lr::scene::makeWorld({});
  auto e = scene.create("tmp", {});
  scene.syncToWorld(*world);
  auto id = scene.registry().get<WorldLink>(e).id;
  scene.destroy(e);
  scene.syncToWorld(*world);
  CHECK_FALSE(world->isAlive(id));
  CHECK(scene.entityCount() == 0);
}

TEST_CASE("editor/adopt-world-entity-not-dirty") {
  EditorScene scene;
  auto world = lr::scene::makeWorld({});
  auto wid = world->createEntity({}, std::nullopt);
  auto e = scene.adoptWorldEntity(wid, "adopted", {}, {}, {}, std::nullopt);
  CHECK(scene.dirtyCount() == 0);
  scene.syncToWorld(*world);
  CHECK(world->isAlive(wid));
  CHECK(scene.registry().get<WorldLink>(e).id == wid);
  CHECK(scene.registry().get<Name>(e).value == "adopted");
}

TEST_CASE("editor/gui-thread-exclusive") {
  EditorScene scene;
  // Same thread as QCoreApplication — requireGui passes.
  CHECK_NOTHROW(scene.create("ok", {}));
}
