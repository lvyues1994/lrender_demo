#include "editor/EditorScene.h"

#include <doctest/doctest.h>
#include <lr/core/Primitives.h>
#include <lr/scene/World.h>

#include <QCoreApplication>
#include <stdexcept>
#include <thread>

using demo::editor::EditorScene;
using demo::editor::Installed;
using demo::editor::Hierarchy;
using demo::editor::WorldLink;

// --- /tmp/e2probe regressions ---

TEST_CASE("editor/probe1-destroy-parent-cascades-child-no-resurrect") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  auto p = s.create("p", {});
  auto c = s.create("c", {}, p);
  s.syncToWorld(*w);
  auto cid = s.registry().get<WorldLink>(c).id;
  auto countBefore = w->entityCount();
  s.destroy(p);
  s.syncToWorld(*w);
  CHECK_FALSE(s.registry().valid(c));
  CHECK_FALSE(w->isAlive(cid));
  CHECK(w->entityCount() < countBefore);
  CHECK(s.entityCount() == 0);
}

TEST_CASE("editor/probe2-setParent-rejects-cycle-at-edit-time") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  auto a = s.create("a", {});
  auto b = s.create("b", {}, a);
  s.syncToWorld(*w);
  CHECK_THROWS_AS(s.setParent(a, b), std::invalid_argument);
  CHECK(s.dirtyCount() == 0);
  CHECK_NOTHROW(s.syncToWorld(*w));
}

TEST_CASE("editor/probe3-destroy-installed-world-gone-alive-check-safe") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  auto wid = w->createEntity({}, std::nullopt);
  auto e = s.adoptWorldEntity(wid, "part0", {}, {}, {});
  REQUIRE(s.registry().all_of<Installed>(e));
  s.destroy(e);
  s.syncToWorld(*w);
  CHECK_FALSE(w->isAlive(wid));
  // Installer-style cleanup must tolerate already-destroyed ids.
  if (w->isAlive(wid))
    w->destroyEntity(wid);
  CHECK_FALSE(w->isAlive(wid));
}

TEST_CASE("editor/probe4-clearForModelSwitch-destroys-editor-created-world-entities") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  s.create("user", {});
  s.syncToWorld(*w);
  REQUIRE(w->entityCount() >= 1);
  s.clearForModelSwitch();
  s.syncToWorld(*w);
  CHECK(s.entityCount() == 0);
  CHECK(w->entityCount() == 0);
}

TEST_CASE("editor/adopt-clearForModelSwitch-leaves-installed-world-for-installer") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  auto wid = w->createEntity({}, std::nullopt);
  s.adoptWorldEntity(wid, "inst", {}, {}, {});
  s.clearForModelSwitch();
  s.syncToWorld(*w);
  CHECK(s.entityCount() == 0);
  CHECK(w->isAlive(wid));
  w->destroyEntity(wid);
}

TEST_CASE("editor/deep-hierarchy-topo-sync-beyond-8") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  std::optional<entt::entity> prev;
  for (int i = 0; i < 12; ++i) {
    auto e = s.create("n" + std::to_string(i), {}, prev);
    prev = e;
  }
  CHECK_NOTHROW(s.syncToWorld(*w));
  CHECK(s.dirtyCount() == 0);
  CHECK(w->entityCount() == 12);
}

TEST_CASE("editor/mesh-material-sync") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  auto mesh =
      w->resources().addMesh(std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
  auto mat = w->resources().defaultMaterial();
  auto e = s.create("m", {});
  s.setMeshMaterial(e, mesh, mat);
  s.syncToWorld(*w);
  auto id = s.registry().get<WorldLink>(e).id;
  auto r = w->renderableOf(id);
  REQUIRE(r);
  CHECK(r->mesh == mesh);
  CHECK(r->material == mat);
}

TEST_CASE("editor/first-sync-idempotent-no-extra-entities") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  s.create("once", {});
  s.syncToWorld(*w);
  auto n = w->entityCount();
  s.syncToWorld(*w);
  CHECK(w->entityCount() == n);
  CHECK(s.dirtyCount() == 0);
}

TEST_CASE("editor/non-gui-thread-rejected") {
  EditorScene s;
  std::exception_ptr ep;
  std::thread t([&] {
    try {
      s.create("bg", {});
    } catch (...) {
      ep = std::current_exception();
    }
  });
  t.join();
  REQUIRE(ep);
  try {
    std::rethrow_exception(ep);
    FAIL("expected logic_error");
  } catch (std::logic_error const &) {
  }
}

TEST_CASE("editor/create-sync-creates-world-entity") {
  EditorScene scene;
  auto world = lr::scene::makeWorld({});
  auto e = scene.create("root", lr::core::Transform::identity());
  REQUIRE(scene.dirtyCount() == 1);
  scene.syncToWorld(*world);
  REQUIRE(scene.dirtyCount() == 0);
  REQUIRE(world->isAlive(scene.registry().get<WorldLink>(e).id));
}

TEST_CASE("editor/transform-dirty-sync-updates-world") {
  EditorScene scene;
  auto world = lr::scene::makeWorld({});
  auto e = scene.create("mover", {});
  scene.syncToWorld(*world);
  lr::core::Transform next{};
  next.translation = {1.f, 2.f, 3.f};
  scene.setLocalTransform(e, next);
  scene.syncToWorld(*world);
  auto got = world->transformOf(scene.registry().get<WorldLink>(e).id);
  CHECK(got.translation.x == doctest::Approx(1.f));
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

TEST_CASE("editor/probe5-unsynced-reparent-then-destroy-old-parent") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  auto p = s.create("p", {});
  auto c = s.create("c", {}, p);
  s.syncToWorld(*w);
  auto cid = s.registry().get<WorldLink>(c).id;
  s.setParent(c, std::nullopt);
  s.destroy(p);
  CHECK_NOTHROW(s.syncToWorld(*w));
  REQUIRE(s.registry().valid(c));
  CHECK(w->isAlive(cid));
  CHECK(s.entityCount() == 1);
  CHECK(w->entityCount() == 1);
  auto parentOfC = s.registry().get<Hierarchy>(c).parent;
  CHECK_FALSE(parentOfC.has_value());
}

TEST_CASE("editor/probe6-unsynced-reparent-under-q-then-destroy-q") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  auto p = s.create("p", {});
  auto q = s.create("q", {});
  auto c = s.create("c", {}, p);
  s.syncToWorld(*w);
  auto cid = s.registry().get<WorldLink>(c).id;
  s.setParent(c, q);
  s.destroy(q);
  CHECK_NOTHROW(s.syncToWorld(*w));
  CHECK(s.entityCount() == 1); // only p
  CHECK(w->entityCount() == 1);
  CHECK_FALSE(w->isAlive(cid)); // c's world entity flushed via subtree queue
  CHECK(s.registry().valid(p));
}

TEST_CASE("editor/probe7-stale-entity-setters-throw-not-abort") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  auto e = s.create("x", {});
  s.destroy(e);
  auto e2 = s.create("y", {});
  (void)e2;
  CHECK_FALSE(s.registry().valid(e));
  CHECK_THROWS_AS(s.setLocalTransform(e, {}), std::invalid_argument);
  CHECK_THROWS_AS(s.setName(e, "z"), std::invalid_argument);
  CHECK_THROWS_AS(s.setMeshMaterial(e, {}, {}), std::invalid_argument);
  CHECK_NOTHROW(s.syncToWorld(*w));
}

TEST_CASE("editor/n3-readopt-after-staging-rollback-restores-installed") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  auto wid = w->createEntity({}, std::nullopt);
  s.adoptWorldEntity(wid, "part0", {}, {}, {});
  auto keep = s.create("keep", {});
  REQUIRE(s.entityCount() == 2);
  // N3: rollback remirror drops Installed only — editor-created survives.
  s.forgetInstalledMirrors();
  s.syncToWorld(*w);
  CHECK(s.entityCount() == 1);
  CHECK(s.registry().valid(keep));
  CHECK(w->isAlive(wid)); // installed world kept for installer
  auto e = s.adoptWorldEntity(wid, "part0", {}, {}, {});
  CHECK(s.entityCount() == 2);
  CHECK(s.registry().all_of<Installed>(e));
  CHECK(s.registry().get<WorldLink>(e).id == wid);
  CHECK(s.registry().valid(keep));
}
