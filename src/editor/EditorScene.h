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
//
// Destroy semantics: CASCADE — destroying an entity also destroys all registry
// descendants (matching lr::scene::World::destroyEntity). Children are not
// reparented. Installed (AssetInstall) entities' World ids are forgotten here;
// the installer journal must release with isAlive checks.
class EditorScene {
public:
  EditorScene();

  entt::registry &registry() noexcept;
  entt::registry const &registry() const noexcept;

  entt::entity create(std::string name, lr::core::Transform local,
                      std::optional<entt::entity> parent = std::nullopt);

  void setName(entt::entity e, std::string name);
  void setLocalTransform(entt::entity e, lr::core::Transform local);
  // Rejects cycles (ancestor check) at edit time — never deferred to sync/frame.
  void setParent(entt::entity e, std::optional<entt::entity> parent);
  void setMeshMaterial(entt::entity e, lr::core::MeshId mesh, lr::core::MaterialId material);
  // Remove MeshRef/MaterialRef (marks Dirty); sync calls World::removeRenderable.
  void clearMeshMaterial(entt::entity e);

  // E4: optional Light / Camera / MaterialOverride. Clearing removes component + Dirty.
  void setLight(entt::entity e, lr::core::LightDesc light);
  void clearLight(entt::entity e);
  void setCamera(entt::entity e, lr::scene::CameraLens lens);
  void clearCamera(entt::entity e);
  // Requires MaterialRef; sync calls WorldResources::replaceMaterial.
  void setMaterialOverride(entt::entity e, lr::core::MaterialDesc desc);
  void clearMaterialOverride(entt::entity e);

  // Cascade-destroy e and all descendants in the registry. Queues a single World
  // destroy for the root WorldLink (World already cascade-destroys descendants).
  void destroy(entt::entity e);

  // Adopt an AssetInstall World entity (tagged Installed, not Dirty).
  entt::entity adoptWorldEntity(lr::core::EntityId worldEntity, std::string name,
                                lr::core::Transform local, lr::core::MeshId mesh,
                                lr::core::MaterialId material,
                                std::optional<entt::entity> parent = std::nullopt);

  // Drop authoring state. destroyWorldEntities=true queues World destroys for
  // non-Installed links and forgets Installed links (installer releases those).
  void clear(bool destroyWorldEntities);
  // Model switch helper: forget Installed links, destroy editor-created World entities.
  void clearForModelSwitch();
  // Drop Installed editor mirrors only (no World destroy). Keeps editor-created entities.
  // Clears Hierarchy.parent that pointed at dropped mirrors (no Dirty).
  void forgetInstalledMirrors();

  // World-SoT remirror after cancel/fail rollback (also used on commit adopt):
  // Reuse Installed editor entities that already mirror the same World EntityId
  // (stable entt id across rollback — E3 undo + E4 selection); create only for
  // new World ids; destroy Installed no longer in the list. Then restore every
  // WorldLink parent/local from World. Never marks Dirty.
  // meshes/materials: length must match worldEntities (installer catalog size check
  // only). MeshRef/MaterialRef content always comes from World::renderableOf (R1);
  // catalog ids are never written into the editor.
  void remirrorInstalledFromWorld(lr::scene::World &world,
                                  std::vector<lr::core::EntityId> const &worldEntities,
                                  std::vector<std::string> const &names,
                                  std::vector<lr::core::MeshId> const &meshes,
                                  std::vector<lr::core::MaterialId> const &materials);

  // Cancel/fail remirror: sync Dirty while Installed mirrors still exist, then
  // remirrorInstalledFromWorld (reuse-in-place). Same entry used by
  // RenderSession::readoptInstalledAfterRollback and random op6.
  void readoptInstalledFromWorld(lr::scene::World &world,
                                 std::vector<lr::core::EntityId> const &worldEntities,
                                 std::vector<std::string> const &names,
                                 std::vector<lr::core::MeshId> const &meshes,
                                 std::vector<lr::core::MaterialId> const &materials);

  void syncToWorld(lr::scene::World &world);

  std::size_t entityCount() const noexcept;
  std::size_t dirtyCount() const noexcept;
  std::size_t pendingWorldDestroyCount() const noexcept;

  bool wouldCreateCycle(entt::entity e, entt::entity newParent) const;
  // Cascade set for destroy / undo purge (M3).
  std::vector<entt::entity> collectSubtree(entt::entity root) const;

private:
  void requireGui() const;
  void markDirty(entt::entity e);
  std::vector<entt::entity> topologicalDirty() const;
  void restoreHierarchyFromWorld(lr::scene::World &world);

  QThread *owner_ = nullptr;
  entt::registry registry_{};
  std::vector<lr::core::EntityId> pendingWorldDestroys_{};
};

} // namespace demo::editor
