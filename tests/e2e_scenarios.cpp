#include "e2e_scenarios.h"
#include <iostream>
namespace demo {
bool E2eScenario::tick(RenderSession &s, MainWindow &w) {
  auto p = s.snapshot();
  // Control stress must keep advancing during hide/zero/recovery: requiring a
  // frame while paused would make the scenario itself prevent restoration.
  if (stage != 1 && (p->life != LifeState::Running || !p->installedModel))
    return false;
  if (p->performance.inFlight > 2)
    throw std::runtime_error("unbounded frame queue");
  static char const *ids[] = {"african_head", "diablo", "boggie", "floor"};
  if (stage == 0) {
    if (name == "cycle") {
      if (modelIndex && p->installedModel->value != ids[modelIndex - 1])
        return false;
      if (modelIndex && observedInstall < modelIndex) {
        observedInstall = modelIndex;
        baseline = p->performance.completed;
        s.setCamera(p->camera); // explicitly request a receipt even in idle
                                // dirty-only visibility
        return false;
      }
      if (modelIndex && p->performance.completed <= baseline)
        return false;
      if (modelIndex)
        std::cout << "e2e installed+rendered=" << ids[modelIndex - 1]
                  << " completed=" << p->performance.completed << std::endl;
      if (modelIndex < 4) {
        s.selectModel({ids[modelIndex++]});
        return false;
      }
    } else if (name == "fallback") {
      if (p->path != PresentationPath::Readback || p->warnings.empty())
        throw std::runtime_error("expected observable readback fallback");
      std::cout << "e2e fallback reason=" << p->warnings.back() << std::endl;
    }
    stage = 1;
    return false;
  }
  if (stage == 1) {
    if (changes < 100 && (name == "cycle" || name == "resize")) {
      if (name == "cycle") {
        s.selectModel({ids[changes % 4]});
        auto pipeline = PipelineKind::Visibility;
        if (changes % 3 == 1)
          pipeline = PipelineKind::Pathtrace;
        else if (changes % 3 == 2)
          pipeline = PipelineKind::PathtraceInteractive;
        s.setRenderOptions({pipeline, 1, false});
      } else {
        int x = changes % 4 ? 64 + int(changes % 13) : 0,
            y = x ? 48 + int(changes % 7) : 0;
        w.viewport().setFixedSize(x, y);
        w.viewport().setVisible(changes % 3 != 0);
        auto d = w.viewport().devicePixelRatioF();
        s.setVisible(changes % 3 != 0);
        s.setViewport({{x, y}, {qRound(x * d), qRound(y * d)}, d});
      }
      ++changes;
      return false; // one command pair per real GUI script turn, never100 in
                    // one stack
    }
    std::cout << "e2e control scenario=" << name
              << " GUI-turn changes=" << changes
              << " (coalescing allowed, not100 completed installations)"
              << std::endl;
    w.viewport().setVisible(true);
    w.viewport().setFixedSize(160, 120);
    auto d = w.viewport().devicePixelRatioF();
    s.setVisible(true);
    s.setViewport({{160, 120}, {qRound(160 * d), qRound(120 * d)}, d});
    s.setRenderOptions({PipelineKind::Pathtrace, 1, false});
    baseline = p->performance.completed;
    stage = 2;
    return false;
  }
  if (p->performance.completed < baseline + frames)
    return false;
  if (p->assetLoadsInFlight || p->assetInstalling) return false;
  if (name == "cycle" && p->installedModel->value != "floor")
    return false;
  std::cout << "e2e scenario=" << name
            << " receipts=" << p->performance.completed - baseline
            << " sharedShown=" << p->performance.sharedShown
            << " inflight=" << p->performance.inFlight << std::endl;
  passed = true;
  return true;
}
} // namespace demo
