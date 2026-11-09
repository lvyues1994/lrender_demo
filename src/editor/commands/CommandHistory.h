#pragma once

#include "editor/Components.h"
#include "editor/EditorScene.h"

#include <lr/core/Id.h>
#include <lr/core/Math.h>
#include <lr/core/Scene.h>

#include <entt/entt.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace demo::editor {

// GUI-thread-exclusive undo stack (A1.2). Value-semantic before/after records;
// apply via EditorScene setters (Dirty → syncToWorld). Not QUndoStack.
//
// Scope: Transform (local TRS), MeshRef/MaterialRef, Hierarchy parent, and
// MaterialDesc override (E4). Session camera/pipeline/model stay latest-wins.

struct TransformCommand {
  entt::entity entity{entt::null};
  lr::core::Transform before{};
  lr::core::Transform after{};
};

struct MaterialCommand {
  entt::entity entity{entt::null};
  // nullopt = entity had no MeshRef/MaterialRef (no World renderable).
  std::optional<std::pair<lr::core::MeshId, lr::core::MaterialId>> before{};
  lr::core::MeshId meshAfter{};
  lr::core::MaterialId materialAfter{};
  // I-A: setMeshMaterial clears MaterialOverride; undo/redo must restore it.
  std::optional<lr::core::MaterialDesc> overrideBefore{};
  std::optional<lr::core::MaterialDesc> overrideAfter{};
};

struct HierarchyCommand {
  entt::entity entity{entt::null};
  std::optional<entt::entity> before{};
  std::optional<entt::entity> after{};
};

struct MaterialDescCommand {
  entt::entity entity{entt::null};
  lr::core::MaterialId material{};
  // nullopt = no MaterialOverride component before/after.
  std::optional<lr::core::MaterialDesc> before{};
  std::optional<lr::core::MaterialDesc> after{};
};

using EditorCommand =
    std::variant<TransformCommand, MaterialCommand, HierarchyCommand, MaterialDescCommand>;

class CommandHistory {
public:
  std::uint64_t beginMerge();
  void endMerge(std::uint64_t epoch = 0);
  bool isMerging() const noexcept { return mergeDepth_ > 0; }
  std::uint64_t mergeEpoch() const noexcept { return mergeEpoch_; }

  void recordTransform(entt::entity entity, lr::core::Transform before,
                       lr::core::Transform after);
  void recordMaterial(entt::entity entity,
                      std::optional<std::pair<lr::core::MeshId, lr::core::MaterialId>> before,
                      lr::core::MeshId meshAfter, lr::core::MaterialId materialAfter,
                      std::optional<lr::core::MaterialDesc> overrideBefore = std::nullopt,
                      std::optional<lr::core::MaterialDesc> overrideAfter = std::nullopt);
  void recordHierarchy(entt::entity entity, std::optional<entt::entity> before,
                       std::optional<entt::entity> after);
  void recordMaterialDesc(entt::entity entity, lr::core::MaterialId material,
                          std::optional<lr::core::MaterialDesc> before,
                          std::optional<lr::core::MaterialDesc> after);

  void executeTransform(EditorScene &scene, entt::entity entity, lr::core::Transform after);
  void executeMaterial(EditorScene &scene, entt::entity entity, lr::core::MeshId mesh,
                       lr::core::MaterialId material);
  void executeHierarchy(EditorScene &scene, entt::entity entity,
                        std::optional<entt::entity> parent);
  void executeMaterialDesc(EditorScene &scene, entt::entity entity,
                           lr::core::MaterialDesc desc);

  bool canUndo() const noexcept { return !undo_.empty(); }
  bool canRedo() const noexcept { return !redo_.empty(); }
  std::size_t undoDepth() const noexcept { return undo_.size(); }
  std::size_t redoDepth() const noexcept { return redo_.size(); }

  void undo(EditorScene &scene);
  void redo(EditorScene &scene);
  void clear() noexcept;
  void forgetEntity(entt::entity e);

private:
  std::vector<EditorCommand> undo_;
  std::vector<EditorCommand> redo_;
  int mergeDepth_ = 0;
  std::optional<std::size_t> mergeAnchor_{};
  std::uint64_t mergeEpoch_ = 0;

  void pushNew(EditorCommand cmd);
  void endMergeFully() noexcept;
  static void applyBefore(EditorScene &scene, EditorCommand const &cmd);
  static void applyAfter(EditorScene &scene, EditorCommand const &cmd);
};

class MergeGuard {
public:
  explicit MergeGuard(CommandHistory &history)
      : history_(&history), epoch_(history.beginMerge()) {}
  ~MergeGuard() {
    if (history_)
      history_->endMerge(epoch_);
  }
  MergeGuard(MergeGuard const &) = delete;
  MergeGuard &operator=(MergeGuard const &) = delete;
  MergeGuard(MergeGuard &&other) noexcept
      : history_(other.history_), epoch_(other.epoch_) {
    other.history_ = nullptr;
  }
  MergeGuard &operator=(MergeGuard &&other) noexcept {
    if (this != &other) {
      if (history_)
        history_->endMerge(epoch_);
      history_ = other.history_;
      epoch_ = other.epoch_;
      other.history_ = nullptr;
    }
    return *this;
  }

private:
  CommandHistory *history_ = nullptr;
  std::uint64_t epoch_ = 0;
};

} // namespace demo::editor
