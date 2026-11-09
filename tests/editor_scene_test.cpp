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
#include <cstdlib>
#include <string_view>

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


TEST_CASE("editor/probe12-two-valid-reparents-same-frame") {
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  auto a = s.create("A", {});
  auto x = s.create("X", {}, a);
  auto b = s.create("B", {}, x);
  s.syncToWorld(*w);
  // World: B → X → A. Two legal editor reparents in one Dirty batch:
  // X → root, then A → B yields final A → B → X (no cycle). Applying A before X
  // in World would temporarily form A → B → X → A.
  s.setParent(x, std::nullopt);
  s.setParent(a, b);
  CHECK_NOTHROW(s.syncToWorld(*w));
  demo::test::assertEditorWorldConsistent(s, *w);
  CHECK_FALSE(s.registry().get<Hierarchy>(x).parent.has_value());
  REQUIRE(s.registry().get<Hierarchy>(a).parent.has_value());
  CHECK(*s.registry().get<Hierarchy>(a).parent == b);
  REQUIRE(s.registry().get<Hierarchy>(b).parent.has_value());
  CHECK(*s.registry().get<Hierarchy>(b).parent == x);
  auto aw = s.registry().get<WorldLink>(a).id;
  auto xw = s.registry().get<WorldLink>(x).id;
  auto bw = s.registry().get<WorldLink>(b).id;
  CHECK_FALSE(w->parentOf(xw).has_value());
  CHECK(*w->parentOf(bw) == xw);
  CHECK(*w->parentOf(aw) == bw);
}

TEST_CASE("editor/i3-dirty-edits-survive-readopt") {
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
  auto c = s.create("C", {}, p0);
  s.syncToWorld(*w);
  demo::test::assertEditorWorldConsistent(s, *w);

  // Install-window edits stay Dirty (no frame / no sync yet).
  lr::core::Transform cxf{};
  cxf.translation = {3.f, 0.f, 0.f};
  s.setLocalTransform(c, cxf);
  auto d = s.create("D", {}, p0);
  lr::core::Transform p1xf{};
  p1xf.translation = {9.f, 0.f, 0.f};
  s.setLocalTransform(p1, p1xf);
  REQUIRE(s.dirtyCount() >= 2);

  // Same sequence as RenderSession::readoptInstalledAfterRollback (I3 order).
  s.readoptInstalledFromWorld(*w, {p0w, p1w}, {"part0", "part1"}, {mesh, mesh}, {mat, mat});
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
  REQUIRE(s.registry().valid(c));
  REQUIRE(s.registry().valid(d));
  REQUIRE(s.registry().get<Hierarchy>(c).parent.has_value());
  CHECK(*s.registry().get<Hierarchy>(c).parent == newP0);
  CHECK(s.registry().get<LocalTransform>(c).value.translation.x == doctest::Approx(3.f));
  REQUIRE(s.registry().get<Hierarchy>(d).parent.has_value());
  CHECK(*s.registry().get<Hierarchy>(d).parent == newP0);
  CHECK(s.registry().get<LocalTransform>(newP1).value.translation.x == doctest::Approx(9.f));
}

TEST_CASE("editor/random-ops-editor-world-invariant") {
  struct Counts {
    std::uint64_t op[8]{};
    std::uint64_t editRejected = 0;
  };
  auto runSeed = [](std::uint32_t seed, int steps, Counts &counts) {
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
      ++counts.op[op];
      auto edit = [&](auto &&fn) {
        try {
          fn();
        } catch (std::invalid_argument const &) {
          ++counts.editRejected;
        }
      };
      if (op == 0) {
        edit([&] {
          lr::core::Transform xf{};
          xf.translation = {float(step % 11), 0.f, 0.f};
          s.create("e" + std::to_string(step), xf, pick());
        });
      } else if (op == 1) {
        edit([&] {
          if (auto e = pick()) {
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
        });
      } else if (op == 2) {
        edit([&] {
          auto a = pick();
          auto b = pick();
          if (a && b && *a != *b)
            s.setParent(*a, *b);
          else if (a)
            s.setParent(*a, std::nullopt);
        });
      } else if (op == 3) {
        edit([&] {
          if (auto e = pick()) {
            lr::core::Transform xf = s.registry().get<LocalTransform>(*e).value;
            xf.translation.x = float((step * 3) % 17);
            s.setLocalTransform(*e, xf);
          }
        });
      } else if (op == 4) {
        edit([&] {
          if (auto e = pick()) {
            if (!s.registry().all_of<Installed>(*e))
              s.setMeshMaterial(*e, mesh, mat);
          }
        });
      } else if (op == 5) {
        // Sync must not throw (I4); do not swallow.
        s.syncToWorld(*w);
        demo::test::assertEditorWorldConsistent(s, *w);
      } else if (op == 6) {
        // Same function as RenderSession readopt path (I3 order).
        std::vector<lr::core::EntityId> alive;
        std::vector<std::string> names;
        std::vector<lr::core::MeshId> meshes;
        std::vector<lr::core::MaterialId> mats;
        for (std::size_t i = 0; i < installedWorld.size(); ++i) {
          if (!w->isAlive(installedWorld[i]))
            continue;
          alive.push_back(installedWorld[i]);
          names.push_back("part" + std::to_string(i));
          meshes.push_back(mesh);
          mats.push_back(mat);
        }
        if (!alive.empty())
          s.readoptInstalledFromWorld(*w, alive, names, meshes, mats);
        demo::test::assertEditorWorldConsistent(s, *w);
      } else {
        edit([&] {
          refreshLive();
          entt::entity inst = entt::null;
          for (auto e : live)
            if (s.registry().all_of<Installed>(e)) {
              inst = e;
              break;
            }
          if (s.registry().valid(inst))
            s.create("c" + std::to_string(step), {}, inst);
        });
      }
    }
    s.syncToWorld(*w);
    demo::test::assertEditorWorldConsistent(s, *w);
  };

  Counts total{};
  for (std::uint32_t seed : {1u, 2u, 42u, 99u, 12345u})
    runSeed(seed, 800, total);
  MESSAGE("random gate op counts: ",
          total.op[0], " create, ", total.op[1], " destroy, ", total.op[2], " reparent, ",
          total.op[3], " move, ", total.op[4], " mesh, ", total.op[5], " sync, ",
          total.op[6], " readopt, ", total.op[7], " create-under; editRejected=",
          total.editRejected);
  CHECK(total.op[5] + total.op[6] > 0);

  // Optional long stress: DEMO_EDITOR_STRESS=1 (also CTest label editor_stress).
  if (const char *stressEnv = std::getenv("DEMO_EDITOR_STRESS");
      stressEnv && stressEnv[0] == '1' && stressEnv[1] == '\0') {
    Counts stress{};
    for (std::uint32_t seed = 1; seed <= 40; ++seed)
      runSeed(seed, 3000, stress);
    MESSAGE("random stress op counts: ",
            stress.op[0], " create, ", stress.op[1], " destroy, ", stress.op[2], " reparent, ",
            stress.op[3], " move, ", stress.op[4], " mesh, ", stress.op[5], " sync, ",
            stress.op[6], " readopt, ", stress.op[7], " create-under; editRejected=",
            stress.editRejected);
  }
}

TEST_CASE("editor/light-camera-material-override-sync") {
  using demo::editor::Camera;
  using demo::editor::Light;
  using demo::editor::MaterialOverride;
  using demo::editor::MaterialRef;
  using demo::editor::MeshRef;
  EditorScene s;
  auto w = lr::scene::makeWorld({});
  auto mesh = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
  auto mat = w->resources().addMaterial(lr::core::MaterialDesc{});
  auto e = s.create("L", {});
  s.setMeshMaterial(e, mesh, mat);
  lr::core::PointLight pl{};
  pl.intensity = 3.f;
  s.setLight(e, pl);
  lr::scene::CameraLens lens{};
  lens.verticalFov = 1.1f;
  lens.nearPlane = 0.1f;
  lens.farPlane = 100.f;
  s.setCamera(e, lens);
  lr::core::MaterialDesc desc{};
  desc.roughness = 0.25f;
  desc.metallic = 0.8f;
  s.setMaterialOverride(e, desc);
  s.syncToWorld(*w);
  auto wid = s.registry().get<demo::editor::WorldLink>(e).id;
  auto cam = w->cameraOf(wid);
  CHECK(cam.verticalFov == doctest::Approx(1.1f));
  auto delta = w->extractDelta();
  CHECK_FALSE(delta.lights.empty());
  // MaterialDesc is write-only on World; override stays on editor.
  REQUIRE(s.registry().all_of<MaterialOverride>(e));
  CHECK(s.registry().get<MaterialOverride>(e).value.roughness == doctest::Approx(0.25f));
  demo::test::assertEditorWorldConsistent(s, *w);

  s.clearLight(e);
  s.syncToWorld(*w);
  auto delta2 = w->extractDelta();
  CHECK_FALSE(delta2.removed.lights.empty());
  demo::test::assertEditorWorldConsistent(s, *w);
}

TEST_CASE("editor/material-override-requires-material-ref") {
  EditorScene s;
  auto e = s.create("bare", {});
  CHECK_THROWS_AS(s.setMaterialOverride(e, lr::core::MaterialDesc{}), std::invalid_argument);
}
