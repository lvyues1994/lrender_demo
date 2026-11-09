#include "editor/EditorScene.h"

#include <QCoreApplication>
#include <algorithm>
#include <cstdint>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

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

bool EditorScene::wouldCreateCycle(entt::entity e, entt::entity newParent) const {
  if (e == newParent)
    return true;
  auto walk = newParent;
  for (;;) {
    if (walk == e)
      return true;
    auto const *hier = registry_.try_get<Hierarchy>(walk);
    if (!hier || !hier->parent)
      return false;
    walk = *hier->parent;
  }
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
  if (!registry_.valid(e))
    throw std::invalid_argument("EditorScene: invalid entity");
  registry_.get<Name>(e).value = std::move(name);
}

void EditorScene::setLocalTransform(entt::entity e, lr::core::Transform local) {
  requireGui();
  if (!registry_.valid(e))
    throw std::invalid_argument("EditorScene: invalid entity");
  registry_.get<LocalTransform>(e).value = local;
  markDirty(e);
}

void EditorScene::setParent(entt::entity e, std::optional<entt::entity> parent) {
  requireGui();
  if (!registry_.valid(e))
    throw std::invalid_argument("EditorScene: invalid entity");
  if (parent) {
    if (!registry_.valid(*parent))
      throw std::invalid_argument("EditorScene: invalid parent");
    if (wouldCreateCycle(e, *parent))
      throw std::invalid_argument("EditorScene: parent would create a hierarchy cycle");
  }
  registry_.get<Hierarchy>(e).parent = parent;
  markDirty(e);
}

void EditorScene::setMeshMaterial(entt::entity e, lr::core::MeshId mesh,
                                  lr::core::MaterialId material) {
  requireGui();
  if (!registry_.valid(e))
    throw std::invalid_argument("EditorScene: invalid entity");
  registry_.emplace_or_replace<MeshRef>(e, MeshRef{mesh});
  registry_.emplace_or_replace<MaterialRef>(e, MaterialRef{material});
  markDirty(e);
}

std::vector<entt::entity> EditorScene::collectSubtree(entt::entity root) const {
  std::vector<entt::entity> out;
  std::vector<entt::entity> stack{root};
  std::unordered_set<std::uint32_t> seen;
  while (!stack.empty()) {
    auto e = stack.back();
    stack.pop_back();
    auto key = static_cast<std::uint32_t>(entt::to_integral(e));
    if (!seen.insert(key).second)
      continue;
    out.push_back(e);
    for (auto [child, hier] : registry_.view<Hierarchy>().each()) {
      if (hier.parent && *hier.parent == e)
        stack.push_back(child);
    }
  }
  return out;
}

void EditorScene::destroy(entt::entity e) {
  requireGui();
  if (!registry_.valid(e))
    return;
  // CASCADE in registry. Queue every node's WorldLink: unsynced reparents mean a
  // child's World entity may not be a World-descendant of the destroyed root, so
  // destroying only the root would orphan them (probe6). Flush uses isAlive.
  auto subtree = collectSubtree(e);
  for (auto node : subtree) {
    if (auto const *link = registry_.try_get<WorldLink>(node)) {
      if (link->id.isValid())
        pendingWorldDestroys_.push_back(link->id);
    }
  }
  for (auto node : subtree) {
    if (registry_.valid(node))
      registry_.destroy(node);
  }
}

entt::entity EditorScene::adoptWorldEntity(lr::core::EntityId worldEntity, std::string name,
                                           lr::core::Transform local, lr::core::MeshId mesh,
                                           lr::core::MaterialId material,
                                           std::optional<entt::entity> parent) {
  requireGui();
  if (!worldEntity.isValid())
    throw std::invalid_argument("EditorScene: invalid world entity");
  if (parent && !registry_.valid(*parent))
    throw std::invalid_argument("EditorScene: invalid parent");
  auto e = registry_.create();
  registry_.emplace<Name>(e, Name{std::move(name)});
  registry_.emplace<LocalTransform>(e, LocalTransform{local});
  registry_.emplace<Hierarchy>(e, Hierarchy{parent});
  registry_.emplace<MeshRef>(e, MeshRef{mesh});
  registry_.emplace<MaterialRef>(e, MaterialRef{material});
  registry_.emplace<WorldLink>(e, WorldLink{worldEntity});
  registry_.emplace<Installed>(e);
  return e;
}

void EditorScene::clear(bool destroyWorldEntities) {
  requireGui();
  if (destroyWorldEntities) {
    for (auto [e, link] : registry_.view<WorldLink>().each()) {
      if (registry_.all_of<Installed>(e))
        continue; // installer journal owns these World entities
      if (link.id.isValid())
        pendingWorldDestroys_.push_back(link.id);
    }
  }
  registry_.clear();
}

void EditorScene::clearForModelSwitch() {
  // Installer will release Installed World entities; editor-created ones must go now.
  clear(true);
}

void EditorScene::forgetInstalledMirrors() {
  requireGui();
  std::vector<entt::entity> doomed;
  for (auto e : registry_.view<Installed>())
    doomed.push_back(e);
  for (auto e : doomed) {
    if (!registry_.valid(e))
      continue;
    for (auto child : registry_.view<Hierarchy>()) {
      auto &h = registry_.get<Hierarchy>(child);
      if (h.parent && *h.parent == e)
        h.parent = std::nullopt; // no Dirty — World still parents under installed
    }
    registry_.destroy(e);
  }
}

void EditorScene::restoreHierarchyFromWorld(lr::scene::World &world) {
  requireGui();
  auto worldKey = [](lr::core::EntityId id) -> std::uint64_t {
    return (static_cast<std::uint64_t>(id.generation) << 32) |
           static_cast<std::uint64_t>(id.index);
  };
  std::unordered_map<std::uint64_t, entt::entity> byWorld;
  for (auto [e, link] : registry_.view<WorldLink>().each()) {
    if (link.id.isValid())
      byWorld[worldKey(link.id)] = e;
  }
  for (auto [e, link] : registry_.view<WorldLink>().each()) {
    if (!link.id.isValid() || !world.isAlive(link.id))
      throw std::logic_error("EditorScene::restoreHierarchyFromWorld: WorldLink not alive");
    // Local transform from World (SoT); never Dirty.
    registry_.get<LocalTransform>(e).value = world.transformOf(link.id);
    auto wp = world.parentOf(link.id);
    if (!wp) {
      registry_.get<Hierarchy>(e).parent = std::nullopt;
      continue;
    }
    auto it = byWorld.find(worldKey(*wp));
    if (it == byWorld.end())
      throw std::logic_error(
          "EditorScene::restoreHierarchyFromWorld: World parent has no editor mirror");
    registry_.get<Hierarchy>(e).parent = it->second;
  }
}

void EditorScene::remirrorInstalledFromWorld(
    lr::scene::World &world, std::vector<lr::core::EntityId> const &worldEntities,
    std::vector<std::string> const &names, std::vector<lr::core::MeshId> const &meshes,
    std::vector<lr::core::MaterialId> const &materials) {
  requireGui();
  if (worldEntities.size() != names.size() || worldEntities.size() != meshes.size() ||
      worldEntities.size() != materials.size())
    throw std::logic_error("EditorScene::remirrorInstalledFromWorld: size mismatch");
  // Phase 1: create every Installed mirror with World local transform, parent null.
  for (std::size_t i = 0; i < worldEntities.size(); ++i) {
    if (!world.isAlive(worldEntities[i]))
      throw std::logic_error("EditorScene::remirrorInstalledFromWorld: entity not alive");
    auto local = world.transformOf(worldEntities[i]);
    adoptWorldEntity(worldEntities[i], names[i], local, meshes[i], materials[i], std::nullopt);
  }
  // Phase 2: restore ALL WorldLink parent/transform edges from World (orphans under
  // installed, installed under user, moved parts, sibling mirrors). No Dirty.
  restoreHierarchyFromWorld(world);
}

std::vector<entt::entity> EditorScene::topologicalDirty() const {
  // Kahn: parents before children among Dirty entities (and their Dirty ancestors).
  std::vector<entt::entity> dirty;
  for (auto e : registry_.view<Dirty>())
    dirty.push_back(e);
  if (dirty.empty())
    return dirty;

  std::unordered_set<std::uint32_t> dirtySet;
  for (auto e : dirty)
    dirtySet.insert(static_cast<std::uint32_t>(entt::to_integral(e)));

  std::unordered_map<std::uint32_t, int> indeg;
  std::unordered_map<std::uint32_t, std::vector<entt::entity>> children;
  for (auto e : dirty) {
    auto key = static_cast<std::uint32_t>(entt::to_integral(e));
    indeg.emplace(key, 0);
  }
  for (auto e : dirty) {
    auto const &hier = registry_.get<Hierarchy>(e);
    if (!hier.parent)
      continue;
    auto pk = static_cast<std::uint32_t>(entt::to_integral(*hier.parent));
    if (dirtySet.count(pk)) {
      children[pk].push_back(e);
      indeg[static_cast<std::uint32_t>(entt::to_integral(e))]++;
    }
  }
  std::queue<entt::entity> q;
  for (auto e : dirty) {
    auto key = static_cast<std::uint32_t>(entt::to_integral(e));
    if (indeg[key] == 0)
      q.push(e);
  }
  std::vector<entt::entity> order;
  while (!q.empty()) {
    auto e = q.front();
    q.pop();
    order.push_back(e);
    auto key = static_cast<std::uint32_t>(entt::to_integral(e));
    for (auto child : children[key]) {
      auto ck = static_cast<std::uint32_t>(entt::to_integral(child));
      if (--indeg[ck] == 0)
        q.push(child);
    }
  }
  if (order.size() != dirty.size())
    throw std::logic_error("EditorScene: dirty hierarchy cycle detected at sync");
  return order;
}

void EditorScene::syncToWorld(lr::scene::World &world) {
  requireGui();
  // N1: apply surviving entities' dirty reparents/transforms BEFORE flushing destroys.
  // Otherwise an unsynced reparent-away + destroy(oldParent) leaves a dead WorldLink
  // (World cascade-killed the child) or orphans (World parent was never updated).
  auto order = topologicalDirty();
  for (auto e : order) {
    if (!registry_.valid(e) || !registry_.all_of<Dirty>(e))
      continue;
    auto const &local = registry_.get<LocalTransform>(e).value;
    auto const parent = registry_.get<Hierarchy>(e).parent;
    if (parent && !registry_.all_of<WorldLink>(*parent))
      throw std::logic_error("EditorScene::syncToWorld: parent missing WorldLink");

    std::optional<lr::core::EntityId> worldParent;
    if (parent)
      worldParent = registry_.get<WorldLink>(*parent).id;

    if (auto *link = registry_.try_get<WorldLink>(e)) {
      if (!world.isAlive(link->id))
        throw std::logic_error("EditorScene::syncToWorld: WorldLink not alive");
      world.setParent(link->id, worldParent);
      world.setTransform(link->id, local);
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

  for (auto id : pendingWorldDestroys_) {
    if (world.isAlive(id))
      world.destroyEntity(id);
  }
  pendingWorldDestroys_.clear();
}

std::size_t EditorScene::entityCount() const noexcept {
  return static_cast<std::size_t>(registry_.view<Name>().size());
}

std::size_t EditorScene::dirtyCount() const noexcept {
  return registry_.view<Dirty>().size();
}

} // namespace demo::editor
