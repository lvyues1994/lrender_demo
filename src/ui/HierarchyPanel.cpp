#include "ui/HierarchyPanel.h"

#include <QDropEvent>
#include <QHeaderView>
#include <QMessageBox>
#include <QTreeWidgetItem>
#include <stdexcept>
#include <unordered_map>

namespace demo {
namespace {
EditorEntityId idFromItem(QTreeWidgetItem *item) {
  if (!item)
    return {};
  return EditorEntityId{static_cast<std::uint32_t>(item->data(0, Qt::UserRole).toUInt())};
}
} // namespace

HierarchyPanel::HierarchyPanel(HierarchyCommands commands, QWidget *parent)
    : QTreeWidget(parent), commands_(std::move(commands)) {
  setHeaderLabel(QStringLiteral("Outliner"));
  setSelectionMode(QAbstractItemView::SingleSelection);
  setDragDropMode(QAbstractItemView::InternalMove);
  setDefaultDropAction(Qt::MoveAction);
  header()->setStretchLastSection(true);
  connect(this, &QTreeWidget::itemSelectionChanged, this, [this] { onSelectionChanged(); });
}

void HierarchyPanel::showSnapshot(SessionSnapshotPtr snap,
                                  editor::SelectionState const &selection) {
  if (!snap)
    return;
  rebuilding_ = true;
  clear();
  std::unordered_map<std::uint32_t, QTreeWidgetItem *> items;
  for (auto const &e : snap->entities) {
    auto *item = new QTreeWidgetItem(QStringList{QString::fromStdString(e.name)});
    item->setData(0, Qt::UserRole, QVariant::fromValue(e.id.value));
    item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled |
                   Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    items[e.id.value] = item;
  }
  for (auto const &e : snap->entities) {
    auto *item = items[e.id.value];
    if (e.parent && items.count(e.parent->value))
      items[e.parent->value]->addChild(item);
    else
      addTopLevelItem(item);
  }
  expandAll();
  if (auto primary = selection.primary()) {
    if (auto it = items.find(primary->value); it != items.end()) {
      setCurrentItem(it->second);
      it->second->setSelected(true);
    }
  }
  rebuilding_ = false;
}

void HierarchyPanel::onSelectionChanged() {
  if (rebuilding_ || !commands_.select)
    return;
  auto *item = currentItem();
  if (!item) {
    commands_.select({});
    return;
  }
  commands_.select(idFromItem(item));
}

void HierarchyPanel::dropEvent(QDropEvent *event) {
  auto *dragged = currentItem();
  auto childId = idFromItem(dragged);
  // Let Qt compute drop target, then read parent after move.
  QTreeWidget::dropEvent(event);
  if (!commands_.reparent || !childId.isValid())
    return;
  auto *item = currentItem();
  if (!item)
    return;
  std::optional<EditorEntityId> parent;
  if (auto *p = item->parent())
    parent = idFromItem(p);
  try {
    commands_.reparent(childId, parent);
  } catch (std::invalid_argument const &ex) {
    if (commands_.tip)
      commands_.tip(QString::fromUtf8(ex.what()));
    // Snapshot refresh from session will restore legal tree.
  }
}

} // namespace demo
