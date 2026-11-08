#pragma once
#include "render/RenderSession.h"
#include "ui/MainWindow.h"
namespace demo {
struct E2eScenario {
  std::string name;
  bool passed = false;
  std::uint64_t frames = 300, baseline = 0;
  unsigned stage = 0, modelIndex = 0, observedInstall = 0, changes = 0;
  bool tick(RenderSession &, MainWindow &);
};
} // namespace demo
