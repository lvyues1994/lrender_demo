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
        } else {
          d.meshAfter = s.meshAfter;
          d.materialAfter = s.materialAfter;
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
  return std::visit([&](auto const &c) { return c.entity == e; }, cmd);
}

} // namespace

void CommandHistory::endMergeFully() noexcept {
  mergeDepth_ = 0;
  mergeAnchor_.reset();
  ++mergeEpoch_; // invalidate open MergeGuards (E5: undo during drag ends merge)
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
    return; // stale guard after undo/redo/clear
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
  // I2: only coalesce with entries pushed inside this merge window.
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
    lr::core::MeshId meshAfter, lr::core::MaterialId materialAfter) {
  pushNew(MaterialCommand{entity, std::move(before), meshAfter, materialAfter});
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
  scene.setMeshMaterial(entity, mesh, material);
  recordMaterial(entity, before, mesh, material);
}

void CommandHistory::applyBefore(EditorScene &scene, EditorCommand const &cmd) {
  std::visit(
      [&](auto const &c) {
        if (!scene.registry().valid(c.entity))
          return;
        if constexpr (std::is_same_v<std::decay_t<decltype(c)>, TransformCommand>) {
          // Restores local TRS (editor LocalTransform), not world-space.
          scene.setLocalTransform(c.entity, c.before);
        } else {
          if (c.before)
            scene.setMeshMaterial(c.entity, c.before->first, c.before->second);
          else
            scene.clearMeshMaterial(c.entity);
        }
      },
      cmd);
}

void CommandHistory::applyAfter(EditorScene &scene, EditorCommand const &cmd) {
  std::visit(
      [&](auto const &c) {
        if (!scene.registry().valid(c.entity))
          return;
        if constexpr (std::is_same_v<std::decay_t<decltype(c)>, TransformCommand>) {
          scene.setLocalTransform(c.entity, c.after);
        } else {
          scene.setMeshMaterial(c.entity, c.meshAfter, c.materialAfter);
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
  // Anchor may now point past size; clamp.
  if (mergeAnchor_ && *mergeAnchor_ > undo_.size())
    mergeAnchor_ = undo_.size();
}

} // namespace demo::editor
