#pragma once

#include <entt/entity/entity.hpp>
#include <lr/core/Id.h>
#include <lr/core/Math.h>

#include <optional>
#include <string>

namespace demo::editor {

// Editor-owned components. GUI thread exclusive via EditorScene.
// E3 Undo: snapshot LocalTransform + material params (hook only).
// E4 Selection/Hierarchy/Inspector: Name + Hierarchy (+ future Selection).
// E7 dual viewport: one shared registry; sync once per tick into session World.

struct Name {
  std::string value;
};

struct LocalTransform {
  lr::core::Transform value = lr::core::Transform::identity();
};

struct Hierarchy {
  std::optional<entt::entity> parent{};
};

struct MeshRef {
  lr::core::MeshId id{};
};

struct MaterialRef {
  lr::core::MaterialId id{};
};

struct WorldLink {
  lr::core::EntityId id{};
};

struct Dirty {};

} // namespace demo::editor
