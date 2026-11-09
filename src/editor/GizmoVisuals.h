#pragma once

#include "editor/GizmoMath.h"
#include "render/SessionTypes.h"

#include <lr/core/Id.h>
#include <lr/core/Math.h>
#include <lr/scene/World.h>

#include <vector>

namespace demo::editor {

// Temporary World-only gizmo meshes (not in EditorScene → excluded from picking).
class GizmoVisuals {
public:
  void clear(lr::scene::World &world);
  void sync(lr::scene::World &world, lr::core::mat4 const &entityWorld,
            GizmoMode mode, GizmoSpace space);

  std::size_t entityCount() const noexcept { return entities_.size(); }

private:
  void ensureResources(lr::scene::World &world);
  void rebuild(lr::scene::World &world, lr::core::mat4 const &entityWorld, GizmoMode mode,
               GizmoSpace space);

  std::vector<lr::core::EntityId> entities_;
  std::vector<lr::core::MeshId> ownedMeshes_;
  std::vector<lr::core::MaterialId> ownedMaterials_;
  lr::core::MeshId shaftMesh_{};
  lr::core::MeshId tipMesh_{};
  lr::core::MeshId ringMesh_{};
  lr::core::MeshId centerMesh_{};
  lr::core::MaterialId matR_{}, matG_{}, matB_{}, matY_{};
  bool resourcesReady_ = false;
  GizmoMode lastMode_ = GizmoMode::Translate;
};

} // namespace demo::editor
