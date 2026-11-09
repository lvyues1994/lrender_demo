#include "editor/EditorScene.h"

#include <QCoreApplication>
#include <stdexcept>

namespace demo::editor {
EditorScene::EditorScene() {
  if (!QCoreApplication::instance())
    throw std::logic_error("EditorScene requires QCoreApplication");
  owner_ = QCoreApplication::instance()->thread();
}

entt::registry &EditorScene::registry() noexcept {
  requireGui();
  return registry_;
}

entt::registry const &EditorScene::registry() const noexcept {
  requireGui();
  return registry_;
}

void EditorScene::requireGui() const {
  if (QThread::currentThread() != owner_)
    throw std::logic_error("EditorScene requires GUI thread owner");
}

void EditorScene::markDirty(entt::entity e) {
  if (!registry_.valid(e))
    throw std::invalid_argument("EditorScene: invalid entity");
  if (!registry_.all_of<Dirty>(e))
    registry_.emplace<Dirty>(e);
}

entt::entity EditorScene::create(std::string name, lr::core::Transform local,
                                 std::optional<entt::entity> parent) {
  requireGui();
  if (parent && !registry_.valid(*parent))
    throw std::invalid_argument("EditorScene: invalid parent");
  auto e = registry_.create();
  registry_.emplace<Name>(e, Name{std::move(name)});
  registry_.emplace<LocalTransform>(e, LocalTransform{local});
  registry_.emplace<Hierarchy>(e, Hierarchy{parent});
  markDirty(e);
  return e;
}

void EditorScene::setName(entt::entity e, std::string name) {
  requireGui();
  registry_.get<Name>(e).value = std::move(name);
  // Name is editor-only; no World sync required.
}

void EditorScene::setLocalTransform(entt::entity e, lr::core::Transform local) {
  requireGui();
  registry_.get<LocalTransform>(e).value = local;
  markDirty(e);
}

void EditorScene::setParent(entt::entity e, std::optional<entt::entity> parent) {
  requireGui();
  if (parent) {
    if (!registry_.valid(*parent))
      throw std::invalid_argument("EditorScene: invalid parent");
    if (*parent == e)
      throw std::invalid_argument("EditorScene: entity cannot parent itself");
  }
  registry_.get<Hierarchy>(e).parent = parent;
  markDirty(e);
}

void EditorScene::setMeshMaterial(entt::entity e, lr::core::MeshId mesh,
                                  lr::core::MaterialId material) {
  requireGui();
  registry_.emplace_or_replace<MeshRef>(e, MeshRef{mesh});
  registry_.emplace_or_replace<MaterialRef>(e, MaterialRef{material});
  markDirty(e);
}

void EditorScene::destroy(entt::entity e) {
  requireGui();
  if (!registry_.valid(e))
    return;
  if (auto const *link = registry_.try_get<WorldLink>(e))
    pendingWorldDestroys_.push_back(link->id);
  // Clear child parent pointers that referenced e.
  for (auto [child, hier] : registry_.view<Hierarchy>().each()) {
    if (hier.parent && *hier.parent == e) {
      hier.parent = std::nullopt;
      markDirty(child);
    }
  }
  registry_.destroy(e);
}

entt::entity EditorScene::adoptWorldEntity(lr::core::EntityId worldEntity, std::string name,
                                           lr::core::Transform local, lr::core::MeshId mesh,
                                           lr::core::MaterialId material,
                                           std::optional<entt::entity> parent) {
  requireGui();
  if (!worldEntity.isValid())
    throw std::invalid_argument("EditorScene: invalid world entity");
  auto e = registry_.create();
  registry_.emplace<Name>(e, Name{std::move(name)});
  registry_.emplace<LocalTransform>(e, LocalTransform{local});
  registry_.emplace<Hierarchy>(e, Hierarchy{parent});
  registry_.emplace<MeshRef>(e, MeshRef{mesh});
  registry_.emplace<MaterialRef>(e, MaterialRef{material});
  registry_.emplace<WorldLink>(e, WorldLink{worldEntity});
  // Already present in World — not Dirty.
  return e;
}

void EditorScene::clear(bool destroyWorld) {
  requireGui();
  if (destroyWorld) {
    for (auto [e, link] : registry_.view<WorldLink>().each())
      pendingWorldDestroys_.push_back(link.id);
  }
  registry_.clear();
}

void EditorScene::resetAuthoring() { clear(false); }

void EditorScene::syncToWorld(lr::scene::World &world) {
  requireGui();
  for (auto id : pendingWorldDestroys_) {
    if (world.isAlive(id))
      world.destroyEntity(id);
  }
  pendingWorldDestroys_.clear();

  // Parents before children: iterate until no Dirty remains or progress stalls.
  for (int pass = 0; pass < 8 && dirtyCount() > 0; ++pass) {
    std::vector<entt::entity> batch;
    for (auto e : registry_.view<Dirty>())
      batch.push_back(e);
    for (auto e : batch) {
      if (!registry_.all_of<Dirty>(e))
        continue;
      auto const &local = registry_.get<LocalTransform>(e).value;
      auto const parent = registry_.get<Hierarchy>(e).parent;
      if (parent && !registry_.all_of<WorldLink>(*parent))
        continue; // wait for parent sync

      std::optional<lr::core::EntityId> worldParent;
      if (parent) {
        worldParent = registry_.get<WorldLink>(*parent).id;
      }

      if (auto *link = registry_.try_get<WorldLink>(e)) {
        if (!world.isAlive(link->id)) {
          // Recreate if World entity was lost.
          auto created = world.createEntity(local, worldParent);
          link->id = created;
        } else {
          world.setParent(link->id, worldParent);
          world.setTransform(link->id, local);
        }
        if (auto const *mesh = registry_.try_get<MeshRef>(e)) {
          if (auto const *mat = registry_.try_get<MaterialRef>(e))
            world.setRenderable(link->id, {mesh->id, mat->id});
        }
      } else {
        auto created = world.createEntity(local, worldParent);
        registry_.emplace<WorldLink>(e, WorldLink{created});
        if (auto const *mesh = registry_.try_get<MeshRef>(e)) {
          if (auto const *mat = registry_.try_get<MaterialRef>(e))
            world.setRenderable(created, {mesh->id, mat->id});
        }
      }
      registry_.remove<Dirty>(e);
    }
  }
  if (dirtyCount() > 0)
    throw std::logic_error("EditorScene::syncToWorld: unresolved dirty entities (hierarchy cycle?)");
}

std::size_t EditorScene::entityCount() const noexcept {
  return static_cast<std::size_t>(registry_.view<Name>().size());
}

std::size_t EditorScene::dirtyCount() const noexcept {
  return registry_.view<Dirty>().size();
}

} // namespace demo::editor
