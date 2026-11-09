#pragma once

#include <entt/entity/entity.hpp>
#include <lr/core/Id.h>
#include <lr/core/Math.h>
#include <lr/core/Scene.h>
#include <lr/scene/World.h>

#include <optional>
#include <string>

namespace demo::editor {

// Editor-owned components. GUI thread exclusive via EditorScene.
// E3 Undo: snapshot LocalTransform + material params (hook only).
// E4 Selection/Hierarchy/Inspector: Name + Hierarchy (+ future Selection).
// E7 dual viewport: one shared registry; sync once per tick into session World.
// Light / Camera / MaterialOverride: E4 inspector + Dirty→World sync.

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

// Adopted from AssetInstall — World lifetime owned by the installer journal.
// Destroying these goes through forgetInstalled / installer release (isAlive-safe).
struct Installed {};

struct Light {
  lr::core::LightDesc value{};
};

struct Camera {
  lr::scene::CameraLens lens{};
};

// Authoritative MaterialDesc override for MaterialRef::id (WorldResources::replaceMaterial).
// Full MaterialDesc (textures preserved). World renderable uses `copyId` (CoW);
// MaterialRef always keeps the original shared material (E3 reference semantics).
struct MaterialOverride {
  lr::core::MaterialDesc value{};
  lr::core::MaterialId copyId{}; // invalid until first sync creates the copy
};


} // namespace demo::editor
