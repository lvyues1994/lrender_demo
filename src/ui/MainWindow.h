#pragma once
#include "HierarchyPanel.h"
#include "PropertyPanel.h"
#include "ViewportHost.h"
#include "editor/SelectionState.h"
#include <QMainWindow>
class QAction;
class QComboBox;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;
class QDockWidget;
namespace demo {
class MainWindow : public QMainWindow {
public:
  explicit MainWindow(UiCommands);
  ViewportHost &viewport() { return *viewport_; }
  editor::SelectionState &selection() noexcept { return selection_; }
  void showSnapshot(SessionSnapshotPtr);

protected:
  void closeEvent(QCloseEvent *) override;

private:
  UiCommands commands_;
  ViewportHost *viewport_;
  HierarchyPanel *hierarchy_ = nullptr;
  PropertyPanel *properties_ = nullptr;
  editor::SelectionState selection_;
  QComboBox *model_ = nullptr, *pipeline_ = nullptr;
  QDoubleSpinBox *exposure_ = nullptr;
  QCheckBox *taa_ = nullptr;
  QLabel *status_ = nullptr, *performance_ = nullptr;
  bool closing_ = false, stopped_ = false;
  lr::core::Aabb bounds_{{-1, -1, -1}, {1, 1, 1}};
  void optionsChanged();
  SessionSnapshotPtr lastSnapshot_;
  std::optional<bool> normalAcknowledgement_;
  void restoreAppearance();
  void refreshEditActions();
  QAction *undoAction_ = nullptr;
  QAction *redoAction_ = nullptr;
};
} // namespace demo
