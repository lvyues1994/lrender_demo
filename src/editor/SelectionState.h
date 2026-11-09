#pragma once

#include "render/SessionTypes.h"

#include <optional>
#include <unordered_set>
#include <vector>

namespace demo::editor {

// GUI-owned selection (A1.3). Not in EditorScene/World — survives remirror because
// EditorEntityId is stable across cancel/fail; cleared when selected ids vanish
// (destroy / model switch clear).
class SelectionState {
public:
  std::optional<EditorEntityId> primary() const noexcept { return primary_; }
  std::vector<EditorEntityId> const &selected() const noexcept { return selected_; }
  bool isSelected(EditorEntityId id) const noexcept;

  void setPrimary(EditorEntityId id);
  void clear() noexcept;
  // Drop any selected id not present in live; clear primary if gone.
  void pruneTo(std::vector<EditorEntitySnapshot> const &live);

private:
  std::optional<EditorEntityId> primary_{};
  std::vector<EditorEntityId> selected_{};
};

} // namespace demo::editor
