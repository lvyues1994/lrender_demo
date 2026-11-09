#pragma once

#include "editor/SelectionState.h"
#include "render/SessionTypes.h"

#include <QTreeWidget>
class QDropEvent;
#include <functional>
#include <optional>

namespace demo {

struct HierarchyCommands {
  std::function<void(EditorEntityId)> select;
  // May throw std::invalid_argument on cycle — panel catches and reports.
  std::function<void(EditorEntityId, std::optional<EditorEntityId>)> reparent;
  std::function<void(QString const &)> tip; // status / cycle tip
};

class HierarchyPanel : public QTreeWidget {
  Q_OBJECT
public:
  explicit HierarchyPanel(HierarchyCommands commands, QWidget *parent = nullptr);
  void showSnapshot(SessionSnapshotPtr snap, editor::SelectionState const &selection);

private:
  HierarchyCommands commands_;
  bool rebuilding_ = false;
  void onSelectionChanged();
  void onItemDropped();
protected:
  void dropEvent(QDropEvent *event) override;
};

} // namespace demo
