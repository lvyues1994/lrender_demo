#include "editor/commands/CommandHistory.h"
#include "editor/EditorScene.h"
#include "support/EditorWorldInvariant.h"

#include <doctest/doctest.h>
#include <lr/core/Primitives.h>
#include <lr/scene/World.h>

#include <random>
#include <vector>

using demo::editor::CommandHistory;
using demo::editor::EditorScene;
using demo::editor::LocalTransform;
using demo::editor::MeshRef;
using demo::editor::MaterialRef;
using demo::editor::Name;
using demo::editor::Installed;
using demo::editor::WorldLink;
using demo::editor::MergeGuard;
using demo::editor::MaterialOverride;
using demo::editor::Hierarchy;
using demo::test::nearTransform;

namespace {

struct XfSnap {
  entt::entity e{};
  lr::core::Transform xf{};
  lr::core::MeshId mesh{};
  lr::core::MaterialId mat{};
  bool hasMesh = false;
};

std::vector<XfSnap> capture(EditorScene const &s) {
  std::vector<XfSnap> out;
  for (auto e : s.registry().view<Name>()) {
    XfSnap snap;
    snap.e = e;
    snap.xf = s.registry().get<LocalTransform>(e).value;
    if (auto const *m = s.registry().try_get<MeshRef>(e)) {
      snap.mesh = m->id;
      snap.hasMesh = true;
    }
    if (auto const *m = s.registry().try_get<MaterialRef>(e))
      snap.mat = m->id;
    out.push_back(snap);
  }
  return out;
}

void requireSame(std::vector<XfSnap> const &a, std::vector<XfSnap> const &b) {
  REQUIRE(a.size() == b.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    REQUIRE(a[i].e == b[i].e);
    CHECK(nearTransform(a[i].xf, b[i].xf));
    CHECK(a[i].hasMesh == b[i].hasMesh);
    if (a[i].hasMesh) {
      CHECK(a[i].mesh == b[i].mesh);
      CHECK(a[i].mat == b[i].mat);
    }
  }
}

} // namespace

TEST_CASE("undo/transform-roundtrip") {
  EditorScene scene;
  auto w = lr::scene::makeWorld({});
  auto e = scene.create("a", {});
  CommandHistory hist;
  lr::core::Transform after{};
  after.translation = {1.f, 2.f, 3.f};
  hist.executeTransform(scene, e, after);
  CHECK(nearTransform(scene.registry().get<LocalTransform>(e).value, after));
  CHECK(hist.canUndo());
  CHECK_FALSE(hist.canRedo());
  hist.undo(scene);
  CHECK(nearTransform(scene.registry().get<LocalTransform>(e).value,
                      lr::core::Transform::identity()));
  CHECK(hist.canRedo());
  hist.redo(scene);
  CHECK(nearTransform(scene.registry().get<LocalTransform>(e).value, after));
  scene.syncToWorld(*w);
  demo::test::assertEditorWorldConsistent(scene, *w);
}

TEST_CASE("undo/material-roundtrip") {
  EditorScene scene;
  auto w = lr::scene::makeWorld({});
  auto meshA = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
  auto meshB = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({2, 2})));
  auto matA = w->resources().addMaterial({});
  auto matB = w->resources().addMaterial({});
  auto e = scene.create("m", {});
  scene.setMeshMaterial(e, meshA, matA);
  scene.syncToWorld(*w); // clear Dirty from setup
  CommandHistory hist;
  hist.executeMaterial(scene, e, meshB, matB);
  CHECK(scene.registry().get<MeshRef>(e).id == meshB);
  CHECK(scene.registry().get<MaterialRef>(e).id == matB);
  hist.undo(scene);
  CHECK(scene.registry().get<MeshRef>(e).id == meshA);
  CHECK(scene.registry().get<MaterialRef>(e).id == matA);
  hist.redo(scene);
  CHECK(scene.registry().get<MeshRef>(e).id == meshB);
  scene.syncToWorld(*w);
  demo::test::assertEditorWorldConsistent(scene, *w);
}

TEST_CASE("undo/merge-coalesces-transform-drag") {
  EditorScene scene;
  auto e = scene.create("drag", {});
  CommandHistory hist;
  hist.beginMerge();
  for (int i = 1; i <= 5; ++i) {
    lr::core::Transform xf{};
    xf.translation = {float(i), 0.f, 0.f};
    hist.executeTransform(scene, e, xf);
  }
  hist.endMerge();
  CHECK(hist.undoDepth() == 1);
  CHECK(scene.registry().get<LocalTransform>(e).value.translation.x == doctest::Approx(5.f));
  hist.undo(scene);
  CHECK(scene.registry().get<LocalTransform>(e).value.translation.x == doctest::Approx(0.f));
  hist.redo(scene);
  CHECK(scene.registry().get<LocalTransform>(e).value.translation.x == doctest::Approx(5.f));
}

TEST_CASE("undo/skips-deleted-entity") {
  EditorScene scene;
  auto a = scene.create("a", {});
  auto b = scene.create("b", {});
  CommandHistory hist;
  lr::core::Transform xa{};
  xa.translation = {4.f, 0.f, 0.f};
  hist.executeTransform(scene, a, xa);
  lr::core::Transform xb{};
  xb.translation = {7.f, 0.f, 0.f};
  hist.executeTransform(scene, b, xb);
  scene.destroy(a);
  // Undo b first (top), then a (stale) — must not crash; b restored, a skipped.
  hist.undo(scene);
  CHECK(nearTransform(scene.registry().get<LocalTransform>(b).value,
                      lr::core::Transform::identity()));
  CHECK_NOTHROW(hist.undo(scene));
  CHECK(hist.undoDepth() == 0);
  CHECK(hist.canRedo());
}

TEST_CASE("undo/clear-discards-stack") {
  EditorScene scene;
  auto e = scene.create("c", {});
  CommandHistory hist;
  lr::core::Transform xf{};
  xf.translation = {1.f, 0.f, 0.f};
  hist.executeTransform(scene, e, xf);
  hist.undo(scene);
  CHECK(hist.canRedo());
  hist.clear();
  CHECK_FALSE(hist.canUndo());
  CHECK_FALSE(hist.canRedo());
  // Entity state unchanged by clear.
  CHECK(nearTransform(scene.registry().get<LocalTransform>(e).value,
                      lr::core::Transform::identity()));
}

TEST_CASE("undo/new-command-clears-redo") {
  EditorScene scene;
  auto e = scene.create("n", {});
  CommandHistory hist;
  lr::core::Transform a{};
  a.translation = {1.f, 0.f, 0.f};
  hist.executeTransform(scene, e, a);
  hist.undo(scene);
  REQUIRE(hist.canRedo());
  lr::core::Transform b{};
  b.translation = {2.f, 0.f, 0.f};
  hist.executeTransform(scene, e, b);
  CHECK_FALSE(hist.canRedo());
  CHECK(hist.undoDepth() == 1);
}

TEST_CASE("undo/random-transform-material-roundtrip") {
  // Arbitrary transform/material sequences: full undo returns to initial; full
  // redo returns to end state. Create/destroy/reparent are outside undo scope.
  for (std::uint32_t seed : {1u, 7u, 42u, 99u, 1234u}) {
    std::mt19937 rng(seed);
    EditorScene scene;
    auto w = lr::scene::makeWorld({});
    auto mesh0 = w->resources().addMesh(
        std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
    auto mesh1 = w->resources().addMesh(
        std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
    auto mat0 = w->resources().addMaterial({});
    auto mat1 = w->resources().addMaterial({});
    std::vector<entt::entity> ents;
    for (int i = 0; i < 4; ++i) {
      auto e = scene.create("e" + std::to_string(i), {});
      // I1: keep e0 without mesh so set→undo→clearRenderable is exercised.
      if (i != 0)
        scene.setMeshMaterial(e, mesh0, mat0);
      ents.push_back(e);
    }
    scene.syncToWorld(*w);
    auto initial = capture(scene);
    CommandHistory hist;
    int steps = 40;
    for (int step = 0; step < steps; ++step) {
      auto e = ents[std::uniform_int_distribution<std::size_t>(0, ents.size() - 1)(rng)];
      if (std::uniform_int_distribution<int>(0, 1)(rng) == 0) {
        lr::core::Transform xf = scene.registry().get<LocalTransform>(e).value;
        xf.translation.x = float((step * 3 + int(seed)) % 19);
        xf.translation.y = float((step + int(seed)) % 7);
        hist.executeTransform(scene, e, xf);
      } else {
        bool useB = std::uniform_int_distribution<int>(0, 1)(rng) == 1;
        hist.executeMaterial(scene, e, useB ? mesh1 : mesh0, useB ? mat1 : mat0);
      }
    }
    auto finalState = capture(scene);
    REQUIRE(hist.undoDepth() == static_cast<std::size_t>(steps));
    while (hist.canUndo())
      hist.undo(scene);
    requireSame(capture(scene), initial);
    while (hist.canRedo())
      hist.redo(scene);
    requireSame(capture(scene), finalState);
    scene.syncToWorld(*w);
    demo::test::assertEditorWorldConsistent(scene, *w);
  }
}


TEST_CASE("undo/material-from-none-roundtrip") {
  // I1: entity starts without MeshRef; undo must clear refs (not set invalid ids).
  EditorScene scene;
  auto w = lr::scene::makeWorld({});
  auto mesh = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
  auto mat = w->resources().addMaterial({});
  auto e = scene.create("bare", {});
  REQUIRE_FALSE(scene.registry().all_of<MeshRef>(e));
  CommandHistory hist;
  hist.executeMaterial(scene, e, mesh, mat);
  REQUIRE(scene.registry().all_of<MeshRef>(e));
  hist.undo(scene);
  CHECK_FALSE(scene.registry().all_of<MeshRef>(e));
  CHECK_FALSE(scene.registry().all_of<MaterialRef>(e));
  scene.syncToWorld(*w);
  demo::test::assertEditorWorldConsistent(scene, *w);
  hist.redo(scene);
  CHECK(scene.registry().get<MeshRef>(e).id == mesh);
  scene.syncToWorld(*w);
  demo::test::assertEditorWorldConsistent(scene, *w);
}

TEST_CASE("undo/merge-preserves-pre-merge-command") {
  // I2: standalone command before beginMerge must not coalesce into the drag.
  EditorScene scene;
  auto e = scene.create("drag", {});
  CommandHistory hist;
  lr::core::Transform x1{};
  x1.translation = {1.f, 0.f, 0.f};
  hist.executeTransform(scene, e, x1);
  hist.beginMerge();
  lr::core::Transform x2{};
  x2.translation = {2.f, 0.f, 0.f};
  hist.executeTransform(scene, e, x2);
  lr::core::Transform x3{};
  x3.translation = {3.f, 0.f, 0.f};
  hist.executeTransform(scene, e, x3);
  hist.endMerge();
  CHECK(hist.undoDepth() == 2);
  hist.undo(scene);
  CHECK(scene.registry().get<LocalTransform>(e).value.translation.x == doctest::Approx(1.f));
  hist.undo(scene);
  CHECK(scene.registry().get<LocalTransform>(e).value.translation.x == doctest::Approx(0.f));
}

TEST_CASE("undo/destroy-purges-stack-entries") {
  // M3: destroying an entity removes its commands so Ctrl+Z is not a no-op.
  EditorScene scene;
  auto a = scene.create("a", {});
  auto b = scene.create("b", {});
  CommandHistory hist;
  lr::core::Transform xa{};
  xa.translation = {1.f, 0.f, 0.f};
  hist.executeTransform(scene, a, xa);
  lr::core::Transform xb{};
  xb.translation = {2.f, 0.f, 0.f};
  hist.executeTransform(scene, b, xb);
  hist.forgetEntity(a);
  scene.destroy(a);
  REQUIRE(hist.undoDepth() == 1);
  hist.undo(scene);
  CHECK(scene.registry().get<LocalTransform>(b).value.translation.x == doctest::Approx(0.f));
  CHECK_FALSE(hist.canUndo());
}

TEST_CASE("undo/merge-guard-raii-and-undo-ends-merge") {
  EditorScene scene;
  auto e = scene.create("g", {});
  CommandHistory hist;
  {
    MergeGuard guard(hist);
    CHECK(hist.isMerging());
    lr::core::Transform xf{};
    xf.translation = {1.f, 0.f, 0.f};
    hist.executeTransform(scene, e, xf);
  }
  CHECK_FALSE(hist.isMerging());
  hist.beginMerge();
  lr::core::Transform xf2{};
  xf2.translation = {2.f, 0.f, 0.f};
  hist.executeTransform(scene, e, xf2);
  CHECK(hist.isMerging());
  hist.undo(scene); // ends merge (M2)
  CHECK_FALSE(hist.isMerging());
}

TEST_CASE("undo/readopt-reuses-installed-entity-ids") {
  // I3 preferred: remirror keeps the same entt id for the same World entity.
  EditorScene scene;
  auto w = lr::scene::makeWorld({});
  auto mesh = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
  auto mat = w->resources().addMaterial({});
  auto wid = w->createEntity({}, std::nullopt);
  w->setRenderable(wid, {mesh, mat});
  auto part = scene.adoptWorldEntity(wid, "part0", {}, mesh, mat);
  scene.syncToWorld(*w);
  CommandHistory hist;
  lr::core::Transform xf{};
  xf.translation = {5.f, 0.f, 0.f};
  hist.executeTransform(scene, part, xf);
  scene.readoptInstalledFromWorld(*w, {wid}, {"part0"}, {mesh}, {mat});
  REQUIRE(scene.registry().valid(part));
  CHECK(scene.registry().all_of<Installed>(part));
  CHECK(scene.registry().get<WorldLink>(part).id == wid);
  CHECK(hist.undoDepth() == 1);
  hist.undo(scene);
  CHECK(scene.registry().get<LocalTransform>(part).value.translation.x == doctest::Approx(0.f));
  scene.syncToWorld(*w);
  demo::test::assertEditorWorldConsistent(scene, *w);
}


TEST_CASE("undo/readopt-preserves-edited-mesh-from-world") {
  // R1: part0 uses mesh0; user switches to mesh1; readopt must keep mesh1 (World SoT),
  // not reinstall mesh0 from the installer catalog arrays.
  EditorScene scene;
  auto w = lr::scene::makeWorld({});
  auto mesh0 = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
  auto mesh1 = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({2, 2})));
  auto mat = w->resources().addMaterial({});
  auto w0 = w->createEntity({}, std::nullopt);
  auto w1 = w->createEntity({}, std::nullopt);
  w->setRenderable(w0, {mesh0, mat});
  w->setRenderable(w1, {mesh1, mat}); // second part already different mesh
  auto p0 = scene.adoptWorldEntity(w0, "part0", {}, mesh0, mat);
  auto p1 = scene.adoptWorldEntity(w1, "part1", {}, mesh1, mat);
  scene.syncToWorld(*w);
  scene.setMeshMaterial(p0, mesh1, mat);
  scene.syncToWorld(*w);
  REQUIRE(w->renderableOf(w0)->mesh == mesh1);
  // Simulate cancel remirror: catalog arrays still list original mesh0 for part0.
  scene.readoptInstalledFromWorld(*w, {w0, w1}, {"part0", "part1"}, {mesh0, mesh1}, {mat, mat});
  REQUIRE(scene.registry().valid(p0));
  CHECK(scene.registry().get<MeshRef>(p0).id == mesh1);
  lr::core::Transform xf{};
  xf.translation = {4.f, 0.f, 0.f};
  scene.setLocalTransform(p0, xf);
  scene.syncToWorld(*w);
  CHECK(w->renderableOf(w0)->mesh == mesh1);
  demo::test::assertEditorWorldConsistent(scene, *w);
}

TEST_CASE("undo/readopt-preserves-cleared-mesh-from-world") {
  // R1: clearMeshMaterial → World removeRenderable → readopt must not restore MeshRef.
  EditorScene scene;
  auto w = lr::scene::makeWorld({});
  auto mesh0 = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
  auto mat = w->resources().addMaterial({});
  auto wid = w->createEntity({}, std::nullopt);
  w->setRenderable(wid, {mesh0, mat});
  auto part = scene.adoptWorldEntity(wid, "part0", {}, mesh0, mat);
  scene.syncToWorld(*w);
  scene.clearMeshMaterial(part);
  scene.syncToWorld(*w);
  CHECK_FALSE(w->renderableOf(wid).has_value());
  scene.readoptInstalledFromWorld(*w, {wid}, {"part0"}, {mesh0}, {mat});
  CHECK_FALSE(scene.registry().all_of<MeshRef>(part));
  CHECK_FALSE(scene.registry().all_of<MaterialRef>(part));
  lr::core::Transform xf{};
  xf.translation = {1.f, 0.f, 0.f};
  scene.setLocalTransform(part, xf);
  scene.syncToWorld(*w);
  CHECK_FALSE(w->renderableOf(wid).has_value());
  demo::test::assertEditorWorldConsistent(scene, *w);
}

TEST_CASE("undo/merge-guard-epoch-survives-undo-end") {
  // Stale MergeGuard after undo/endMergeFully must not close a newer merge window.
  EditorScene scene;
  auto e = scene.create("g", {});
  CommandHistory hist;
  {
    MergeGuard outer(hist);
    CHECK(hist.isMerging());
    lr::core::Transform a{};
    a.translation = {1.f, 0.f, 0.f};
    hist.executeTransform(scene, e, a);
    hist.undo(scene); // ends merge, bumps epoch — outer becomes stale
    CHECK_FALSE(hist.isMerging());
    {
      MergeGuard inner(hist);
      CHECK(hist.isMerging());
      lr::core::Transform b{};
      b.translation = {2.f, 0.f, 0.f};
      hist.executeTransform(scene, e, b);
      lr::core::Transform c{};
      c.translation = {3.f, 0.f, 0.f};
      hist.executeTransform(scene, e, c);
      CHECK(hist.undoDepth() == 1); // coalesced inside new window
    } // inner ends merge
    CHECK_FALSE(hist.isMerging());
    hist.beginMerge();
    CHECK(hist.isMerging());
  } // outer dtor is stale (epoch mismatch) → no-op; manual beginMerge above stays open.
  CHECK(hist.isMerging());
  hist.endMerge();
  CHECK_FALSE(hist.isMerging());
}

TEST_CASE("undo/readopt-adopt-branch-uses-world-mesh-not-installer") {
  // E3 Minor: World entity never mirrored before; World has meshB, installer lists meshA.
  // New-branch adopt must take World SoT (meshB), not installer catalog meshA.
  EditorScene scene;
  auto w = lr::scene::makeWorld({});
  auto meshA = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
  auto meshB = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({2, 2})));
  auto mat = w->resources().addMaterial({});
  auto wid = w->createEntity({}, std::nullopt);
  w->setRenderable(wid, {meshB, mat});
  CHECK(scene.registry().view<Installed>().begin() ==
        scene.registry().view<Installed>().end()); // no prior mirrors
  scene.readoptInstalledFromWorld(*w, {wid}, {"part0"}, {meshA}, {mat});
  auto view = scene.registry().view<Installed, MeshRef, WorldLink>();
  REQUIRE(view.begin() != view.end());
  entt::entity e = *view.begin();
  CHECK(scene.registry().get<MeshRef>(e).id == meshB);
  CHECK(w->renderableOf(wid)->mesh == meshB);
  demo::test::assertEditorWorldConsistent(scene, *w);
}

TEST_CASE("undo/readopt-adopt-branch-clears-when-world-has-no-renderable") {
  // E3 Minor: World entity never mirrored; World has no renderable; installer lists meshA.
  // Adopt must leave MeshRef/MaterialRef absent (not stamp installer meshA).
  EditorScene scene;
  auto w = lr::scene::makeWorld({});
  auto meshA = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
  auto mat = w->resources().addMaterial({});
  auto wid = w->createEntity({}, std::nullopt);
  CHECK_FALSE(w->renderableOf(wid).has_value());
  scene.readoptInstalledFromWorld(*w, {wid}, {"part0"}, {meshA}, {mat});
  auto view = scene.registry().view<Installed, WorldLink>();
  REQUIRE(view.begin() != view.end());
  entt::entity e = *view.begin();
  CHECK_FALSE(scene.registry().all_of<MeshRef>(e));
  CHECK_FALSE(scene.registry().all_of<MaterialRef>(e));
  CHECK_FALSE(w->renderableOf(wid).has_value());
  demo::test::assertEditorWorldConsistent(scene, *w);
}

TEST_CASE("undo/hierarchy-reparent-roundtrip") {
  EditorScene scene;
  auto parent = scene.create("parent", {});
  auto child = scene.create("child", {});
  CommandHistory hist;
  hist.executeHierarchy(scene, child, parent);
  CHECK(scene.registry().get<Hierarchy>(child).parent == parent);
  hist.undo(scene);
  CHECK_FALSE(scene.registry().get<Hierarchy>(child).parent.has_value());
  hist.redo(scene);
  CHECK(scene.registry().get<Hierarchy>(child).parent == parent);
}

TEST_CASE("undo/material-desc-override-roundtrip") {
  EditorScene scene;
  auto w = lr::scene::makeWorld({});
  auto mesh = w->resources().addMesh(
      std::make_shared<lr::core::MeshData const>(lr::core::makeQuad({1, 1})));
  auto mat = w->resources().addMaterial({});
  auto e = scene.create("m", {});
  scene.setMeshMaterial(e, mesh, mat);
  CommandHistory hist;
  lr::core::MaterialDesc desc{};
  desc.roughness = 0.2f;
  desc.metallic = 0.9f;
  hist.executeMaterialDesc(scene, e, desc);
  REQUIRE(scene.registry().all_of<MaterialOverride>(e));
  CHECK(scene.registry().get<MaterialOverride>(e).value.roughness == doctest::Approx(0.2f));
  hist.undo(scene);
  CHECK_FALSE(scene.registry().all_of<MaterialOverride>(e));
  hist.redo(scene);
  CHECK(scene.registry().get<MaterialOverride>(e).value.metallic == doctest::Approx(0.9f));
}
