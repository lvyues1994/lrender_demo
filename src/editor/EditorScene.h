#pragma once

#include "editor/Components.h"

#include <entt/entt.hpp>
#include <lr/scene/World.h>

#include <QThread>
#include <string>
#include <vector>

namespace demo::editor {

// GUI-thread-exclusive editor registry. RenderSession owns the World; this owns
// authoring state and pushes incremental dirty updates into World.
class EditorScene {
public:
  EditorScene();

  entt::registry &registry() noexcept;
  entt::registry const &registry() const noexcept;

  // Create a root/child editor entity; marks Dirty (World entity created on sync).
  entt::entity create(std::string name, lr::core::Transform local,
                      std::optional<entt::entity> parent = std::nullopt);

  void setName(entt::entity e, std::string name);
  void setLocalTransform(entt::entity e, lr::core::Transform local);
  void setParent(entt::entity e, std::optional<entt::entity> parent);
  void setMeshMaterial(entt::entity e, lr::core::MeshId mesh, lr::core::MaterialId material);

  // Destroy editor entity; World destroy is deferred until syncToWorld.
  void destroy(entt::entity e);

  // Adopt an existing World entity (e.g. after AssetInstall) as already-synced.
  entt::entity adoptWorldEntity(lr::core::EntityId worldEntity, std::string name,
                                lr::core::Transform local, lr::core::MeshId mesh,
                                lr::core::MaterialId material,
                                std::optional<entt::entity> parent = std::nullopt);

  // Drop all editor entities. If destroyWorld is true, World entities are
  // queued for removal on the next syncToWorld; if false, only authoring
  // state is forgotten (AssetInstall already released World entities).
  void clear(bool destroyWorld = true);
  void resetAuthoring(); // clear(false)

  // Apply pending destroys + Dirty components into World. Idempotent when clean.
  void syncToWorld(lr::scene::World &world);

  std::size_t entityCount() const noexcept;
  std::size_t dirtyCount() const noexcept;

private:
  void requireGui() const;
  void markDirty(entt::entity e);

  QThread *owner_ = nullptr;
  entt::registry registry_{};
  std::vector<lr::core::EntityId> pendingWorldDestroys_{};
};

} // namespace demo::editor
