#pragma once
#include "ViewportHost.h"
#include <QMainWindow>
class QComboBox;
class QDoubleSpinBox;
class QCheckBox;
class QLabel;
namespace demo {
class MainWindow : public QMainWindow {
public:
  explicit MainWindow(UiCommands);
  ViewportHost &viewport() { return *viewport_; }
  void showSnapshot(SessionSnapshotPtr);

protected:
  void closeEvent(QCloseEvent *) override;

private:
  UiCommands commands_;
  ViewportHost *viewport_;
  QComboBox *model_ = nullptr, *pipeline_ = nullptr;
  QDoubleSpinBox *exposure_ = nullptr;
  QCheckBox *taa_ = nullptr;
  QLabel *status_ = nullptr;
  bool closing_ = false, stopped_ = false;
  lr::core::Aabb bounds_{{-1, -1, -1}, {1, 1, 1}};
  void optionsChanged();
};
} // namespace demo
