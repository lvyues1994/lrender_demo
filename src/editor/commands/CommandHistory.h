#pragma once

#include "editor/Components.h"
#include "editor/EditorScene.h"

#include <lr/core/Id.h>
#include <lr/core/Math.h>

#include <entt/entt.hpp>
#include <optional>
#include <variant>
#include <vector>

namespace demo::editor {

// GUI-thread-exclusive undo stack (A1.2). Value-semantic before/after records;
// apply via EditorScene setters (Dirty → syncToWorld). Not QUndoStack: domain
// must be testable without Widgets, merge/coalesce is custom, and stale entity
// ids must skip without Qt text/index coupling. MainWindow binds QAction shortcuts
// to session undo/redo which forward here.
//
// Scope: Transform (TRS) + MaterialRef/MeshRef only. Session camera/pipeline/model
// and ambient/roughness/normal stay latest-wins. Editable MaterialDesc overrides
// deferred to E4 — E3 only records MeshId+MaterialId reference swaps.

struct TransformCommand {
  entt::entity entity{entt::null};
  lr::core::Transform before{};
  lr::core::Transform after{};
};

struct MaterialCommand {
  entt::entity entity{entt::null};
  lr::core::MeshId meshBefore{};
  lr::core::MaterialId materialBefore{};
  lr::core::MeshId meshAfter{};
  lr::core::MaterialId materialAfter{};
};

using EditorCommand = std::variant<TransformCommand, MaterialCommand>;

class CommandHistory {
public:
  // Open a merge window (E5 gizmo drag / material slider). Consecutive commands of
  // the same kind on the same entity coalesce into one stack entry (keep first
  // before, update after). Nested beginMerge is refcounted.
  void beginMerge();
  void endMerge();
  bool isMerging() const noexcept { return mergeDepth_ > 0; }

  // Record after an external apply already happened, or record+apply helpers below.
  void recordTransform(entt::entity entity, lr::core::Transform before,
                       lr::core::Transform after);
  void recordMaterial(entt::entity entity, lr::core::MeshId meshBefore,
                      lr::core::MaterialId materialBefore, lr::core::MeshId meshAfter,
                      lr::core::MaterialId materialAfter);

  // Apply after-state through EditorScene (marks Dirty) then record.
  void executeTransform(EditorScene &scene, entt::entity entity, lr::core::Transform after);
  void executeMaterial(EditorScene &scene, entt::entity entity, lr::core::MeshId mesh,
                       lr::core::MaterialId material);

  bool canUndo() const noexcept { return !undo_.empty(); }
  bool canRedo() const noexcept { return !redo_.empty(); }
  std::size_t undoDepth() const noexcept { return undo_.size(); }
  std::size_t redoDepth() const noexcept { return redo_.size(); }

  // Restore before (undo) / after (redo). If entity is destroyed or stale, skip
  // the apply but still consume the entry so the stack cannot stick.
  void undo(EditorScene &scene);
  void redo(EditorScene &scene);

  void clear() noexcept;

private:
  std::vector<EditorCommand> undo_;
  std::vector<EditorCommand> redo_;
  int mergeDepth_ = 0;

  void pushNew(EditorCommand cmd);
  static void applyBefore(EditorScene &scene, EditorCommand const &cmd);
  static void applyAfter(EditorScene &scene, EditorCommand const &cmd);
};

} // namespace demo::editor
