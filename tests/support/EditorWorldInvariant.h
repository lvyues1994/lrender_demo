#pragma once

#include "editor/EditorScene.h"
#include "editor/Components.h"

#include <doctest/doctest.h>
#include <lr/core/Math.h>
#include <lr/scene/World.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace demo::test {

inline bool nearTransform(lr::core::Transform const &a, lr::core::Transform const &b,
                          float eps = 1e-4f) {
  auto near3 = [&](lr::core::vec3 const &x, lr::core::vec3 const &y) {
    return std::abs(x.x - y.x) < eps && std::abs(x.y - y.y) < eps && std::abs(x.z - y.z) < eps;
  };
  auto near4 = [&](lr::core::vec4 const &x, lr::core::vec4 const &y) {
    return std::abs(x.x - y.x) < eps && std::abs(x.y - y.y) < eps && std::abs(x.z - y.z) < eps &&
           std::abs(x.w - y.w) < eps;
  };
  return near3(a.translation, b.translation) && near3(a.scale, b.scale) &&
         near4(a.rotation.quaternion, b.rotation.quaternion);
}

inline std::uint64_t worldKey(lr::core::EntityId id) {
  return (static_cast<std::uint64_t>(id.generation) << 32) | static_cast<std::uint64_t>(id.index);
}

// After sync / remirror: every WorldLink matches World parent/local TRS/mesh/mat,
// and every live World entity is referenced by exactly one editor WorldLink.
inline void assertEditorWorldConsistent(demo::editor::EditorScene const &scene,
                                        lr::scene::World const &world) {
  auto const &reg = scene.registry();
  std::unordered_map<std::uint64_t, entt::entity> byWorld;
  std::size_t links = 0;
  for (auto [e, link] : reg.view<demo::editor::WorldLink>().each()) {
    ++links;
    REQUIRE(link.id.isValid());
    REQUIRE(world.isAlive(link.id));
    auto key = worldKey(link.id);
    REQUIRE_FALSE(byWorld.count(key));
    byWorld.emplace(key, e);

    auto const &local = reg.get<demo::editor::LocalTransform>(e).value;
    CHECK(nearTransform(local, world.transformOf(link.id)));

    auto const &hier = reg.get<demo::editor::Hierarchy>(e);
    auto wp = world.parentOf(link.id);
    if (!wp) {
      CHECK_FALSE(hier.parent.has_value());
    } else {
      REQUIRE(hier.parent.has_value());
      REQUIRE(reg.valid(*hier.parent));
      auto const *plink = reg.try_get<demo::editor::WorldLink>(*hier.parent);
      REQUIRE(plink);
      CHECK(plink->id == *wp);
    }

    // R1/C1: MeshRef matches World mesh. MaterialRef is the original shared id;
    // with MaterialOverride, World renderable uses a CoW copyId (!= MaterialRef).
    auto renderable = world.renderableOf(link.id);
    auto const *mesh = reg.try_get<demo::editor::MeshRef>(e);
    auto const *mat = reg.try_get<demo::editor::MaterialRef>(e);
    auto const *ovr = reg.try_get<demo::editor::MaterialOverride>(e);
    if (renderable) {
      REQUIRE(mesh);
      REQUIRE(mat);
      CHECK(mesh->id == renderable->mesh);
      if (ovr) {
        CHECK(ovr->copyId.isValid());
        CHECK(renderable->material == ovr->copyId);
        CHECK(renderable->material != mat->id);
        // Override desc keeps textures from the base copy (panel must not wipe them).
        if (auto base = scene.materialBaseOf(mat->id)) {
          CHECK(ovr->value.baseColorTexture == base->baseColorTexture);
          CHECK(ovr->value.normalTexture == base->normalTexture);
        }
      } else {
        CHECK(mat->id == renderable->material);
      }
    } else {
      CHECK_FALSE(mesh);
      CHECK_FALSE(mat);
      CHECK_FALSE(ovr);
    }

    // E4: Camera — if present, World::cameraOf succeeds and lens FOV/near/far match.
    // World has no removeCamera; absent Camera does not assert World lens cleared.
    if (auto const *cam = reg.try_get<demo::editor::Camera>(e)) {
      lr::core::CameraDesc worldCam{};
      CHECK_NOTHROW(worldCam = world.cameraOf(link.id));
      CHECK(worldCam.verticalFov == doctest::Approx(cam->lens.verticalFov));
      CHECK(worldCam.nearPlane == doctest::Approx(cam->lens.nearPlane));
      CHECK(worldCam.farPlane == doctest::Approx(cam->lens.farPlane));
    }
    // Light: World has no lightOf query; require component + alive WorldLink only.
    // Sync unit tests assert extractDelta light upserts/removals.
    if (reg.all_of<demo::editor::Light>(e)) {
      CHECK(world.isAlive(link.id));
    }
    if (ovr) {
      REQUIRE(mat);
      CHECK(std::isfinite(ovr->value.roughness));
      CHECK(std::isfinite(ovr->value.metallic));
    }
    CHECK_FALSE(reg.all_of<demo::editor::Dirty>(e));
  }
  // Every editor entity has a WorldLink after sync; no pending destroys left in scene API
  // Name count == WorldLink count; pendingWorldDestroyCount == 0.
  auto names = static_cast<std::size_t>(reg.view<demo::editor::Name>().size());
  auto linksOnly = static_cast<std::size_t>(reg.view<demo::editor::WorldLink>().size());
  CHECK(names == linksOnly);
  CHECK(reg.view<demo::editor::Dirty>().size() == 0);
  CHECK(scene.pendingWorldDestroyCount() == 0);
  CHECK(links == world.entityCount());
}

} // namespace demo::test
