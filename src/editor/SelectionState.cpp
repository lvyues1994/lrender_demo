#include "editor/SelectionState.h"

#include <algorithm>

namespace demo::editor {

bool SelectionState::isSelected(EditorEntityId id) const noexcept {
  return std::find(selected_.begin(), selected_.end(), id) != selected_.end();
}

void SelectionState::setPrimary(EditorEntityId id) {
  if (!id.isValid()) {
    clear();
    return;
  }
  primary_ = id;
  selected_ = {id};
}

void SelectionState::clear() noexcept {
  primary_.reset();
  selected_.clear();
}

void SelectionState::pruneTo(std::vector<EditorEntitySnapshot> const &live) {
  std::unordered_set<std::uint32_t> ok;
  ok.reserve(live.size());
  for (auto const &e : live)
    if (e.id.isValid())
      ok.insert(e.id.value);
  selected_.erase(std::remove_if(selected_.begin(), selected_.end(),
                                 [&](EditorEntityId id) { return !ok.count(id.value); }),
                  selected_.end());
  if (primary_ && !ok.count(primary_->value))
    primary_.reset();
  if (!primary_ && !selected_.empty())
    primary_ = selected_.front();
  if (selected_.empty())
    primary_.reset();
}

} // namespace demo::editor
