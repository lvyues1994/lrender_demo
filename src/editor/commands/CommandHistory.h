#pragma once

#include "editor/Components.h"
#include "editor/EditorScene.h"

#include <lr/core/Id.h>
#include <lr/core/Math.h>

#include <entt/entt.hpp>
#include <cstddef>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace demo::editor {

// GUI-thread-exclusive undo stack (A1.2). Value-semantic before/after records;
// apply via EditorScene setters (Dirty → syncToWorld). Not QUndoStack: domain
// must be testable without Widgets, merge/coalesce is custom, and stale entity
// ids must skip without Qt text/index coupling. MainWindow binds QAction shortcuts
// to session undo/redo which forward here.
//
// Scope: Transform (local TRS) + MeshRef/MaterialRef only. Undo restores the
// entity's local transform (not world-space). Session camera/pipeline/model and
// ambient/roughness/normal stay latest-wins. Editable MaterialDesc parameter
// undo lands with E4 inspector — E3 only records MeshId+MaterialId reference
// swaps (including "no renderable" ↔ "has renderable").

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
};

using EditorCommand = std::variant<TransformCommand, MaterialCommand>;

class CommandHistory {
public:
  // Open a merge window (E5 gizmo drag / material slider). Only commands pushed
  // after the outermost beginMerge coalesce with the previous in-window entry
  // (same kind+entity); commands already on the stack are never swallowed (I2).
  // Nested beginMerge is refcounted. undo/redo/clear end any open merge (M2).
  void beginMerge();
  void endMerge();
  bool isMerging() const noexcept { return mergeDepth_ > 0; }

  void recordTransform(entt::entity entity, lr::core::Transform before,
                       lr::core::Transform after);
  void recordMaterial(entt::entity entity,
                      std::optional<std::pair<lr::core::MeshId, lr::core::MaterialId>> before,
                      lr::core::MeshId meshAfter, lr::core::MaterialId materialAfter);

  void executeTransform(EditorScene &scene, entt::entity entity, lr::core::Transform after);
  void executeMaterial(EditorScene &scene, entt::entity entity, lr::core::MeshId mesh,
                       lr::core::MaterialId material);

  bool canUndo() const noexcept { return !undo_.empty(); }
  bool canRedo() const noexcept { return !redo_.empty(); }
  std::size_t undoDepth() const noexcept { return undo_.size(); }
  std::size_t redoDepth() const noexcept { return redo_.size(); }

  // Restore before (undo) / after (redo). Missing entity → skip apply, still pop.
  // Ends any open merge window.
  void undo(EditorScene &scene);
  void redo(EditorScene &scene);

  void clear() noexcept;

  // M3: drop stack entries that reference e (call before/after registry destroy).
  void forgetEntity(entt::entity e);

private:
  std::vector<EditorCommand> undo_;
  std::vector<EditorCommand> redo_;
  int mergeDepth_ = 0;
  // undo_.size() at outermost beginMerge; coalesce only while size > anchor.
  std::optional<std::size_t> mergeAnchor_{};

  void pushNew(EditorCommand cmd);
  void endMergeFully() noexcept;
  static void applyBefore(EditorScene &scene, EditorCommand const &cmd);
  static void applyAfter(EditorScene &scene, EditorCommand const &cmd);
};

// M2: RAII beginMerge/endMerge for E5 gizmo. Moves; undo/redo on the history
// still ends merge independently (guard's endMerge is then a no-op depth clamp).
class MergeGuard {
public:
  explicit MergeGuard(CommandHistory &history) : history_(&history) {
    history_->beginMerge();
  }
  ~MergeGuard() {
    if (history_)
      history_->endMerge();
  }
  MergeGuard(MergeGuard const &) = delete;
  MergeGuard &operator=(MergeGuard const &) = delete;
  MergeGuard(MergeGuard &&other) noexcept : history_(other.history_) {
    other.history_ = nullptr;
  }
  MergeGuard &operator=(MergeGuard &&other) noexcept {
    if (this != &other) {
      if (history_)
        history_->endMerge();
      history_ = other.history_;
      other.history_ = nullptr;
    }
    return *this;
  }

private:
  CommandHistory *history_ = nullptr;
};

} // namespace demo::editor
