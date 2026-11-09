# lrender sync + PathtraceInteractive pipeline report

Time: 2026-11-09T15:00:17+0800 (Asia/Shanghai)
Branch: feat/qt-vulkan-demo
BASE: 589737cea8fe94c1086a681a062cd58f85d49119
HEAD: 1a4df86b53d4133314620065299c76e39da5d37b

## 1) Upstream sync

| Checkout | Old SHA | New SHA | Method |
|---|---|---|---|
| third_party/lrender | dcf36670724c65b016db9234309ad49192dda6c8 | d00dcf52c0cac4242464ad0ecd646466aa38ef97 | fetch + `merge --ff-only origin/main` |

- Working tree of lrender was clean and not diverged before FF.
- Transitive deps checked (compute_shaders / co2 / stackfull / lexec): already at latest `origin/main` and already satisfy lrender pins; **no FF needed**.
- Lock file `cmake/dependencies.lock.json` updated for lrender only; VerifyDependencies: 14/14.

### Upstream changelog summary (d00dcf5)

Commit subject: *The interactive path tracing pipeline denoises the path tracer while the view moves, and shows the reference's mean once it holds 64 samples.*

Public API:
- `PipelineRegistry::pathTraceInteractive()` added (alongside `visibility()` / `pathTraceReference()`).
- `makePathTraceDenoiseFeature()`: PostProcess SVGF-style denoise of PT mean using visibility guides; &lt;16 samples show denoised colour; 16–64 fade into the reference mean.
- New kernel `render/kernels/Denoise.h`, MotionPass helper, DenoiseTest (CPU + GPU backends).
- Examples/CLI accept `--pipeline pathtrace-interactive` (keys 1/2/3 / Tab).

Breaking for implementers of `PipelineRegistry`: new pure virtual `pathTraceInteractive()` (demo spies forward to inner registry; no local registry impl).

No other demo-facing API break observed; demo compiled against new lr_render after lock update.

## 2) New pipeline in demo

Added `PipelineKind::PathtraceInteractive` + helper `isPathTracePipeline()`.

Wiring:
- `RenderSession` selects `pipelines().pathTraceInteractive()`; continuous pump + history/soft-rebuild rules match Pathtrace (soft keep presenter/host; hard on native loss / fallback / stop).
- UI combo: Visibility / Pathtrace / PathtraceInteractive; snapshot restore index 0/1/2.
- Black-flash self-test stage `pipeline-interactive` between Pathtrace and resize.
- e2e cycle rotates three pipelines (`changes % 3`).
- Readback black-flash sampling uses owned widget image (missing native host is only a shared black-hole).

Architecture unchanged: value commands, immutable snapshots, RenderSession exclusivity, soft/hard rebuild, black-flash fix not regressed.

## 3) TDD evidence

RED then GREEN:
- `session/pathtrace-interactive-selects-interactive-pipeline-and-stays-continuous`
- `session/cpu_smoke-pathtrace-interactive-accumulates-nonblack`
- `session/soft-rebuild-pathtrace-interactive-keeps-path-without-release`
- `ui/options-and-model-commands-snapshot-does-not-loop` (count==3, index 2)

Final unit: **165/165** cases, 6546 assertions (`/tmp/pti-unit-all.log` / CTest demo_tests).
Final CTest: **27/27**, 0 skip (`/tmp/pti-final-ctest.log`).

Black-flash (pipeline-interactive blackSamples=0 hostChanges=0):
- Vulkan shared RTX 5070 Ti: exit 0 (`/tmp/pti-shared-bf2.log`; CTest e2e_shared_black_flash ~1.1s)
- Vulkan readback: exit 0 (`/tmp/pti-readback-bf2.log`)
- CPU readback offscreen: exit 0 (`/tmp/pti-cpu-bf2.log`) — interactive denoise supported on CPU upstream.

Also: e2e_shared_dpr125 Passed; e2e_gpu_readback Passed; e2e_shared_cycle includes interactive rotation.

## 4) Binary mtimes (Asia/Shanghai)

Newest owned source edit: 2026-11-09 14:58:47

| Binary | mtime |
|---|---|
| lrender_demo | 2026-11-09 14:58:52 |
| demo_e2e | 2026-11-09 14:58:52 |
| demo_tests | 2026-11-09 15:00:17 |
| demo_qt_vulkan_probe | 2026-11-09 15:00:17 |
| demo_link_smoke | 2026-11-09 15:00:17 |

All newer than owned sources. Build dir: `build/plan-debug`.

## 5) Commit scope

Owned files only (no third_party / assets / .vscode; no push/amend):
- cmake/dependencies.lock.json
- src/render/SessionTypes.h, RenderSession.cpp
- src/ui/MainWindow.cpp
- src/app/main.cpp
- tests/session_test.cpp, ui_test.cpp, e2e_scenarios.cpp
- README.md
- .superpowers/sdd/lrender-sync-pipeline-report.md, review.diff

## 6) Known limits

- Interactive PT still accumulates like reference PT; denoise is while samples &lt;64 / view moving (upstream semantics).
- Black-flash on readback does not assert hostChanges (no native host); shared path remains the host-stability authority.
- third_party checkouts stay gitignored; lock SHA is the reproducible pin.
- No push in this task.
