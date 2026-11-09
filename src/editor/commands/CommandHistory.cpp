#include "editor/commands/CommandHistory.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace demo::editor {
namespace {

bool sameEntityKind(EditorCommand const &a, EditorCommand const &b) {
  if (a.index() != b.index())
    return false;
  return std::visit(
      [](auto const &x, auto const &y) {
        using X = std::decay_t<decltype(x)>;
        using Y = std::decay_t<decltype(y)>;
        if constexpr (std::is_same_v<X, Y>)
          return x.entity == y.entity;
        else
          return false;
      },
      a, b);
}

void coalesceInto(EditorCommand &dst, EditorCommand const &src) {
  std::visit(
      [&](auto &d) {
        using T = std::decay_t<decltype(d)>;
        auto const &s = std::get<T>(src);
        if constexpr (std::is_same_v<T, TransformCommand>) {
          d.after = s.after;
        } else if constexpr (std::is_same_v<T, MaterialCommand>) {
          d.meshAfter = s.meshAfter;
          d.materialAfter = s.materialAfter;
          d.overrideAfter = s.overrideAfter;
        } else if constexpr (std::is_same_v<T, HierarchyCommand>) {
          d.after = s.after;
        } else if constexpr (std::is_same_v<T, MaterialDescCommand>) {
          d.after = s.after;
          d.material = s.material;
        }
      },
      dst);
}

std::optional<std::pair<lr::core::MeshId, lr::core::MaterialId>>
materialBeforeOf(EditorScene const &scene, entt::entity e) {
  auto const *mesh = scene.registry().try_get<MeshRef>(e);
  auto const *mat = scene.registry().try_get<MaterialRef>(e);
  if (!mesh || !mat)
    return std::nullopt;
  return std::make_pair(mesh->id, mat->id);
}

bool referencesEntity(EditorCommand const &cmd, entt::entity e) {
  return std::visit(
      [&](auto const &c) {
        using T = std::decay_t<decltype(c)>;
        if (c.entity == e)
          return true;
        if constexpr (std::is_same_v<T, HierarchyCommand>) {
          if (c.before && *c.before == e)
            return true;
          if (c.after && *c.after == e)
            return true;
        }
        return false;
      },
      cmd);
}

void applyMaterialDesc(EditorScene &scene, entt::entity e,
                       std::optional<lr::core::MaterialDesc> const &desc) {
  if (desc)
    scene.setMaterialOverride(e, *desc);
  else
    scene.clearMaterialOverride(e);
}

} // namespace

void CommandHistory::endMergeFully() noexcept {
  mergeDepth_ = 0;
  mergeAnchor_.reset();
  ++mergeEpoch_;
}

std::uint64_t CommandHistory::beginMerge() {
  if (mergeDepth_ == 0) {
    mergeAnchor_ = undo_.size();
    ++mergeEpoch_;
  }
  ++mergeDepth_;
  return mergeEpoch_;
}

void CommandHistory::endMerge(std::uint64_t epoch) {
  if (epoch != 0 && epoch != mergeEpoch_)
    return;
  if (mergeDepth_ > 0)
    --mergeDepth_;
  if (mergeDepth_ == 0)
    mergeAnchor_.reset();
}

void CommandHistory::clear() noexcept {
  undo_.clear();
  redo_.clear();
  endMergeFully();
}

void CommandHistory::pushNew(EditorCommand cmd) {
  if (mergeDepth_ > 0 && mergeAnchor_ && undo_.size() > *mergeAnchor_ &&
      sameEntityKind(undo_.back(), cmd)) {
    coalesceInto(undo_.back(), cmd);
  } else {
    undo_.push_back(std::move(cmd));
  }
  redo_.clear();
}

void CommandHistory::recordTransform(entt::entity entity, lr::core::Transform before,
                                    lr::core::Transform after) {
  pushNew(TransformCommand{entity, before, after});
}

void CommandHistory::recordMaterial(
    entt::entity entity,
    std::optional<std::pair<lr::core::MeshId, lr::core::MaterialId>> before,
    lr::core::MeshId meshAfter, lr::core::MaterialId materialAfter,
    std::optional<lr::core::MaterialDesc> overrideBefore,
    std::optional<lr::core::MaterialDesc> overrideAfter) {
  pushNew(MaterialCommand{entity, std::move(before), meshAfter, materialAfter,
                          std::move(overrideBefore), std::move(overrideAfter)});
}

void CommandHistory::recordHierarchy(entt::entity entity, std::optional<entt::entity> before,
                                    std::optional<entt::entity> after) {
  pushNew(HierarchyCommand{entity, before, after});
}

void CommandHistory::recordMaterialDesc(entt::entity entity, lr::core::MaterialId material,
                                       std::optional<lr::core::MaterialDesc> before,
                                       std::optional<lr::core::MaterialDesc> after) {
  pushNew(MaterialDescCommand{entity, material, std::move(before), std::move(after)});
}

void CommandHistory::executeTransform(EditorScene &scene, entt::entity entity,
                                     lr::core::Transform after) {
  if (!scene.registry().valid(entity))
    return;
  auto before = scene.registry().get<LocalTransform>(entity).value;
  scene.setLocalTransform(entity, after);
  recordTransform(entity, before, after);
}

void CommandHistory::executeMaterial(EditorScene &scene, entt::entity entity,
                                    lr::core::MeshId mesh, lr::core::MaterialId material) {
  if (!scene.registry().valid(entity))
    return;
  auto before = materialBeforeOf(scene, entity);
  std::optional<lr::core::MaterialDesc> ovrBefore;
  if (auto const *ovr = scene.registry().try_get<MaterialOverride>(entity))
    ovrBefore = ovr->value;
  scene.setMeshMaterial(entity, mesh, material); // clears override
  recordMaterial(entity, before, mesh, material, ovrBefore, std::nullopt);
}

void CommandHistory::executeHierarchy(EditorScene &scene, entt::entity entity,
                                     std::optional<entt::entity> parent) {
  if (!scene.registry().valid(entity))
    return;
  auto before = scene.registry().get<Hierarchy>(entity).parent;
  scene.setParent(entity, parent);
  recordHierarchy(entity, before, parent);
}

void CommandHistory::executeMaterialDesc(EditorScene &scene, entt::entity entity,
                                        lr::core::MaterialDesc desc) {
  if (!scene.registry().valid(entity))
    return;
  auto const *mat = scene.registry().try_get<MaterialRef>(entity);
  if (!mat)
    return;
  std::optional<lr::core::MaterialDesc> before;
  if (auto const *ovr = scene.registry().try_get<MaterialOverride>(entity))
    before = ovr->value;
  scene.setMaterialOverride(entity, desc);
  recordMaterialDesc(entity, mat->id, before, desc);
}

void CommandHistory::applyBefore(EditorScene &scene, EditorCommand const &cmd) {
  std::visit(
      [&](auto const &c) {
        if (!scene.registry().valid(c.entity))
          return;
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, TransformCommand>) {
          scene.setLocalTransform(c.entity, c.before);
        } else if constexpr (std::is_same_v<T, MaterialCommand>) {
          if (c.before)
            scene.setMeshMaterial(c.entity, c.before->first, c.before->second);
          else
            scene.clearMeshMaterial(c.entity);
          // Restore override after ref swap (setMeshMaterial clears it).
          if (c.overrideBefore)
            scene.setMaterialOverride(c.entity, *c.overrideBefore);
        } else if constexpr (std::is_same_v<T, HierarchyCommand>) {
          scene.setParent(c.entity, c.before);
        } else if constexpr (std::is_same_v<T, MaterialDescCommand>) {
          applyMaterialDesc(scene, c.entity, c.before);
        }
      },
      cmd);
}

void CommandHistory::applyAfter(EditorScene &scene, EditorCommand const &cmd) {
  std::visit(
      [&](auto const &c) {
        if (!scene.registry().valid(c.entity))
          return;
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, TransformCommand>) {
          scene.setLocalTransform(c.entity, c.after);
        } else if constexpr (std::is_same_v<T, MaterialCommand>) {
          scene.setMeshMaterial(c.entity, c.meshAfter, c.materialAfter);
          if (c.overrideAfter)
            scene.setMaterialOverride(c.entity, *c.overrideAfter);
        } else if constexpr (std::is_same_v<T, HierarchyCommand>) {
          scene.setParent(c.entity, c.after);
        } else if constexpr (std::is_same_v<T, MaterialDescCommand>) {
          applyMaterialDesc(scene, c.entity, c.after);
        }
      },
      cmd);
}

void CommandHistory::undo(EditorScene &scene) {
  endMergeFully();
  if (undo_.empty())
    return;
  auto cmd = std::move(undo_.back());
  undo_.pop_back();
  applyBefore(scene, cmd);
  redo_.push_back(std::move(cmd));
}

void CommandHistory::redo(EditorScene &scene) {
  endMergeFully();
  if (redo_.empty())
    return;
  auto cmd = std::move(redo_.back());
  redo_.pop_back();
  applyAfter(scene, cmd);
  undo_.push_back(std::move(cmd));
}

void CommandHistory::forgetEntity(entt::entity e) {
  auto drop = [&](std::vector<EditorCommand> &stack) {
    stack.erase(std::remove_if(stack.begin(), stack.end(),
                               [&](EditorCommand const &c) { return referencesEntity(c, e); }),
                stack.end());
  };
  drop(undo_);
  drop(redo_);
  if (mergeAnchor_ && *mergeAnchor_ > undo_.size())
    mergeAnchor_ = undo_.size();
}

} // namespace demo::editor
