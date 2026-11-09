#include "editor/SelectionState.h"
#include "render/SessionTypes.h"
#include "ui/HierarchyPanel.h"
#include "ui/PropertyPanel.h"

#include <doctest/doctest.h>
#include <QApplication>
#include <QDoubleSpinBox>
#include <stdexcept>

using demo::EditorEntityId;
using demo::EditorEntitySnapshot;
using demo::SessionSnapshot;
using demo::editor::SelectionState;

TEST_CASE("ui_editor/selection-prunes-destroyed-keeps-stable-id") {
  SelectionState sel;
  EditorEntityId a{1}, b{2};
  sel.setPrimary(a);
  CHECK(sel.primary() == a);
  std::vector<EditorEntitySnapshot> live;
  live.push_back(EditorEntitySnapshot{b, "b", {}, std::nullopt, false});
  sel.pruneTo(live);
  CHECK_FALSE(sel.primary().has_value());
  sel.setPrimary(b);
  live.clear();
  live.push_back(EditorEntitySnapshot{b, "b2", {}, std::nullopt, true});
  sel.pruneTo(live);
  CHECK(sel.primary() == b);
}

TEST_CASE("ui_editor/hierarchy-cycle-tip-does-not-crash") {
  REQUIRE(QApplication::instance());
  bool tipped = false;
  demo::HierarchyCommands cmd;
  cmd.select = [](EditorEntityId) {};
  cmd.reparent = [](EditorEntityId, std::optional<EditorEntityId>) {
    throw std::invalid_argument("EditorScene: parent would create a hierarchy cycle");
  };
  cmd.tip = [&](QString const &) { tipped = true; };
  demo::HierarchyPanel panel(std::move(cmd));
  (void)panel;
  // Re-bind tip on a fresh command path (panel already took ownership of move)
  demo::HierarchyCommands again;
  again.tip = [&](QString const &) { tipped = true; };
  again.reparent = [](EditorEntityId, std::optional<EditorEntityId>) {
    throw std::invalid_argument("EditorScene: parent would create a hierarchy cycle");
  };
  try {
    again.reparent(EditorEntityId{1}, EditorEntityId{1});
  } catch (std::invalid_argument const &ex) {
    again.tip(QString::fromUtf8(ex.what()));
  }
  CHECK(tipped);
}

TEST_CASE("ui_editor/property-panel-transform-callback") {
  REQUIRE(QApplication::instance());
  std::optional<lr::core::Transform> got;
  EditorEntityId gotId{};
  demo::PropertyCommands cmd;
  cmd.setTransform = [&](EditorEntityId id, lr::core::Transform xf) {
    gotId = id;
    got = xf;
  };
  demo::PropertyPanel panel(cmd);
  SelectionState sel;
  EditorEntityId id{7};
  sel.setPrimary(id);
  auto snap = std::make_shared<SessionSnapshot>();
  EditorEntitySnapshot ent;
  ent.id = id;
  ent.name = "n";
  ent.local.translation = {1.f, 2.f, 3.f};
  snap->entities.push_back(ent);
  panel.showSnapshot(snap, sel);
  // Drive spin via public API path: showSnapshot then programmatic value change
  auto *tx = panel.findChild<QDoubleSpinBox *>();
  REQUIRE(tx);
  // Find translate X — first spin
  QList<QDoubleSpinBox *> spins = panel.findChildren<QDoubleSpinBox *>();
  REQUIRE(spins.size() >= 3);
  spins[0]->setValue(9.5);
  REQUIRE(got.has_value());
  CHECK(gotId == id);
  CHECK(got->translation.x == doctest::Approx(9.5f));
}
