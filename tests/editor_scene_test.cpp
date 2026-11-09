#include "editor/EditorScene.h"
#include "support/EditorWorldInvariant.h"

#include <doctest/doctest.h>
#include <lr/core/Primitives.h>
#include <lr/scene/World.h>

#include <QCoreApplication>
#include <stdexcept>
#include <thread>
#include <random>
#include <optional>
#include <vector>
#include <string>
#include <unordered_set>

using demo::editor::EditorScene;
using demo::editor::Dirty;
using demo::editor::Installed;
using demo::editor::Hierarchy;
using demo::editor::WorldLink;
using demo::editor::LocalTransform;
using demo::editor::Name;

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

TEST_CASE("editor/probe11-forget-installed-rebinds-orphan-to-new-mirror") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  auto mesh = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
  auto mat = w->resources().addMaterial({});
  auto partWorld = w->createEntity({}, std::nullopt);
  w->setRenderable(partWorld, {mesh, mat});
  auto part = s.adoptWorldEntity(partWorld, "part0", {}, mesh, mat);
  auto c = s.create("c", {}, part);
  s.syncToWorld(*w);
  auto cWorld = s.registry().get<WorldLink>(c).id;
  REQUIRE(w->parentOf(cWorld).has_value());
  CHECK(*w->parentOf(cWorld) == partWorld);

  s.forgetInstalledMirrors();
  CHECK_FALSE(s.registry().get<Hierarchy>(c).parent.has_value());
  s.remirrorInstalledFromWorld(*w, {partWorld}, {"part0"}, {mesh}, {mat});
  auto part2 = entt::entity{};
  for (auto [e, link] : s.registry().view<WorldLink, Installed>().each()) {
    if (link.id == partWorld)
      part2 = e;
  }
  REQUIRE(s.registry().valid(part2));
  REQUIRE(s.registry().get<Hierarchy>(c).parent.has_value());
  CHECK(*s.registry().get<Hierarchy>(c).parent == part2);
  bool childDirty = s.registry().all_of<Dirty>(c);
  CHECK_FALSE(childDirty);
  CHECK_THROWS_AS(s.setParent(part2, c), std::invalid_argument);
  demo::test::assertEditorWorldConsistent(s, *w);
}

TEST_CASE("editor/i2-readopt-preserves-installed-under-user") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  auto mesh = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
  auto mat = w->resources().addMaterial({});
  auto p0w = w->createEntity({}, std::nullopt);
  auto p1w = w->createEntity({}, std::nullopt);
  w->setRenderable(p0w, {mesh, mat});
  w->setRenderable(p1w, {mesh, mat});
  auto p0 = s.adoptWorldEntity(p0w, "part0", {}, mesh, mat);
  auto p1 = s.adoptWorldEntity(p1w, "part1", {}, mesh, mat);
  lr::core::Transform uxf{};
  uxf.translation = {7.f, 0.f, 0.f};
  auto u = s.create("U", uxf);
  s.setParent(p0, u);
  lr::core::Transform p1xf{};
  p1xf.translation = {5.f, 0.f, 0.f};
  s.setLocalTransform(p1, p1xf);
  s.syncToWorld(*w);
  demo::test::assertEditorWorldConsistent(s, *w);

  // Cancel/readopt: World remains SoT for parent/transform of installed parts.
  s.forgetInstalledMirrors();
  s.remirrorInstalledFromWorld(*w, {p0w, p1w}, {"part0", "part1"}, {mesh, mesh}, {mat, mat});
  demo::test::assertEditorWorldConsistent(s, *w);

  entt::entity newP0{}, newP1{};
  for (auto [e, link] : s.registry().view<WorldLink, Installed>().each()) {
    if (link.id == p0w)
      newP0 = e;
    if (link.id == p1w)
      newP1 = e;
  }
  REQUIRE(s.registry().valid(newP0));
  REQUIRE(s.registry().valid(newP1));
  REQUIRE(s.registry().get<Hierarchy>(newP0).parent.has_value());
  CHECK(*s.registry().get<Hierarchy>(newP0).parent == u);
  CHECK(s.registry().get<LocalTransform>(newP1).value.translation.x == doctest::Approx(5.f));
  CHECK_FALSE(s.registry().all_of<Dirty>(newP0));
  CHECK_FALSE(s.registry().all_of<Dirty>(newP1));
  // Reverse divergence: parenting U under new part0 is a cycle (World still has part0 under U).
  CHECK_THROWS_AS(s.setParent(u, newP0), std::invalid_argument);
}

TEST_CASE("editor/random-ops-editor-world-invariant") {
  auto runSeed = [](std::uint32_t seed, int steps) {
    std::mt19937 rng(seed);
    EditorScene s;
    auto w = lr::scene::makeWorld({});
    auto mesh = w->resources().addMesh(
        std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
    auto mat = w->resources().addMaterial({});
    std::vector<entt::entity> live;
    std::vector<lr::core::EntityId> installedWorld;
    auto refreshLive = [&] {
      live.clear();
      for (auto e : s.registry().view<Name>())
        live.push_back(e);
    };
    auto pick = [&]() -> std::optional<entt::entity> {
      refreshLive();
      if (live.empty())
        return std::nullopt;
      return live[std::uniform_int_distribution<std::size_t>(0, live.size() - 1)(rng)];
    };
    // Seed scene with two installed parts.
    for (int i = 0; i < 2; ++i) {
      auto wid = w->createEntity({}, std::nullopt);
      w->setRenderable(wid, {mesh, mat});
      installedWorld.push_back(wid);
      s.adoptWorldEntity(wid, "part" + std::to_string(i), {}, mesh, mat);
    }
    s.syncToWorld(*w);
    demo::test::assertEditorWorldConsistent(s, *w);

    for (int step = 0; step < steps; ++step) {
      int op = std::uniform_int_distribution<int>(0, 7)(rng);
      try {
        if (op == 0) { // create
          lr::core::Transform xf{};
          xf.translation = {float(step % 11), 0.f, 0.f};
          auto parent = pick();
          s.create("e" + std::to_string(step), xf, parent);
        } else if (op == 1) { // destroy
          if (auto e = pick()) {
            // Skip if Installed or any Installed descendant (cascade would drop
            // installer-owned World entities from this fixture).
            bool skip = false;
            std::vector<entt::entity> stack{*e};
            std::unordered_set<std::uint32_t> seen;
            while (!stack.empty() && !skip) {
              auto n = stack.back();
              stack.pop_back();
              auto key = static_cast<std::uint32_t>(entt::to_integral(n));
              if (!seen.insert(key).second)
                continue;
              if (s.registry().all_of<Installed>(n)) {
                skip = true;
                break;
              }
              for (auto [child, hier] : s.registry().view<Hierarchy>().each())
                if (hier.parent && *hier.parent == n)
                  stack.push_back(child);
            }
            if (!skip)
              s.destroy(*e);
          }
        } else if (op == 2) { // reparent
          auto a = pick();
          auto b = pick();
          if (a && b && *a != *b)
            s.setParent(*a, *b);
          else if (a)
            s.setParent(*a, std::nullopt);
        } else if (op == 3) { // move
          if (auto e = pick()) {
            lr::core::Transform xf = s.registry().get<LocalTransform>(*e).value;
            xf.translation.x = float((step * 3) % 17);
            s.setLocalTransform(*e, xf);
          }
        } else if (op == 4) { // mesh/mat on non-installed
          if (auto e = pick()) {
            if (!s.registry().all_of<Installed>(*e))
              s.setMeshMaterial(*e, mesh, mat);
          }
        } else if (op == 5) { // sync
          s.syncToWorld(*w);
          demo::test::assertEditorWorldConsistent(s, *w);
        } else if (op == 6) { // remirror (cancel path)
          s.syncToWorld(*w);
          s.forgetInstalledMirrors();
          std::vector<std::string> names;
          std::vector<lr::core::MeshId> meshes;
          std::vector<lr::core::MaterialId> mats;
          for (std::size_t i = 0; i < installedWorld.size(); ++i) {
            if (!w->isAlive(installedWorld[i]))
              continue;
            names.push_back("part" + std::to_string(i));
            meshes.push_back(mesh);
            mats.push_back(mat);
          }
          std::vector<lr::core::EntityId> alive;
          for (auto id : installedWorld)
            if (w->isAlive(id))
              alive.push_back(id);
          if (!alive.empty())
            s.remirrorInstalledFromWorld(*w, alive, names, meshes, mats);
          demo::test::assertEditorWorldConsistent(s, *w);
        } else { // create under installed
          refreshLive();
          entt::entity inst = entt::null;
          for (auto e : live)
            if (s.registry().all_of<Installed>(e)) {
              inst = e;
              break;
            }
          if (s.registry().valid(inst))
            s.create("c" + std::to_string(step), {}, inst);
        }
      } catch (std::invalid_argument const &) {
        // Illegal edits rejected at edit time — OK.
      } catch (std::logic_error const &) {
        // Cycle / sync guards — OK if caught here; must not leave divergence after sync.
      }
    }
    s.syncToWorld(*w);
    demo::test::assertEditorWorldConsistent(s, *w);
  };

  for (std::uint32_t seed : {1u, 2u, 42u, 99u, 12345u})
    runSeed(seed, 800);
}
