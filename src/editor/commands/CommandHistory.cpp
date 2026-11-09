#include "editor/commands/CommandHistory.h"
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

lr::core::MeshId meshOf(EditorScene const &scene, entt::entity e) {
  auto const *m = scene.registry().try_get<MeshRef>(e);
  return m ? m->id : lr::core::MeshId{};
}

lr::core::MaterialId materialOf(EditorScene const &scene, entt::entity e) {
  auto const *m = scene.registry().try_get<MaterialRef>(e);
  return m ? m->id : lr::core::MaterialId{};
}

} // namespace

void CommandHistory::beginMerge() { ++mergeDepth_; }

void CommandHistory::endMerge() {
  if (mergeDepth_ > 0)
    --mergeDepth_;
}

void CommandHistory::clear() noexcept {
  undo_.clear();
  redo_.clear();
  // Leave mergeDepth alone? Clear during model switch should end merge.
  mergeDepth_ = 0;
}

void CommandHistory::pushNew(EditorCommand cmd) {
  if (mergeDepth_ > 0 && !undo_.empty() && sameEntityKind(undo_.back(), cmd)) {
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

void CommandHistory::recordMaterial(entt::entity entity, lr::core::MeshId meshBefore,
                                   lr::core::MaterialId materialBefore,
                                   lr::core::MeshId meshAfter,
                                   lr::core::MaterialId materialAfter) {
  pushNew(MaterialCommand{entity, meshBefore, materialBefore, meshAfter, materialAfter});
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
  auto mb = meshOf(scene, entity);
  auto matb = materialOf(scene, entity);
  scene.setMeshMaterial(entity, mesh, material);
  recordMaterial(entity, mb, matb, mesh, material);
}

void CommandHistory::applyBefore(EditorScene &scene, EditorCommand const &cmd) {
  std::visit(
      [&](auto const &c) {
        if (!scene.registry().valid(c.entity))
          return;
        if constexpr (std::is_same_v<std::decay_t<decltype(c)>, TransformCommand>) {
          scene.setLocalTransform(c.entity, c.before);
        } else {
          scene.setMeshMaterial(c.entity, c.meshBefore, c.materialBefore);
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
  if (undo_.empty())
    return;
  auto cmd = std::move(undo_.back());
  undo_.pop_back();
  applyBefore(scene, cmd);
  redo_.push_back(std::move(cmd));
}

void CommandHistory::redo(EditorScene &scene) {
  if (redo_.empty())
    return;
  auto cmd = std::move(redo_.back());
  redo_.pop_back();
  applyAfter(scene, cmd);
  undo_.push_back(std::move(cmd));
}

} // namespace demo::editor
