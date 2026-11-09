# lrender Qt Vulkan demo

Qt5.15 Widgets viewer using real lrender/lexec/lqtexec. Vulkan is default;
CPU is used only with explicit `--backend cpu`. No general OBJ/MTL editor/importer.

## Offline build / prerequisites

Use the existing local source tree with all14 locked checkouts below. CMake never
fetches/clones missing sources: it fails with an explicit lock/path error. Qt5.15
Core/Gui/Widgets development files, C++17 exceptions, CMake>=3.25, Ninja, Clang
for the tc transpiler, Vulkan loader/headers/driver and XCB headers/library are
system prerequisites; no install command is run automatically. Tested Qt5.15.13,
GCC13.3 and NVIDIA RTX5070Ti on Linux XCB. No Qt private ABI or Qt-owned VkSurface.

```sh
cd /absolute/path/to/lrender_demo
cmake -S . -B build/plan-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/plan-debug --target lrender_demo demo_tests demo_link_smoke demo_qt_vulkan_probe --parallel 2
cmake -P cmake/VerifyDependencies.cmake
QT_QPA_PLATFORM=offscreen build/plan-debug/demo_tests --no-skip=true
QT_QPA_PLATFORM=offscreen ctest --test-dir build/plan-debug -L unit --output-on-failure
```

lr::render WHOLE_ARCHIVE, one shared lexec target, C++17 exceptions, Vulkan and tc
transpiler are enabled; network/tests/examples/benchmarks of dependencies and
OpenGL are disabled. A successful build is not proof of shared presentation.

## Run / resource root / moved executable

`--asset-root` points to TinyRenderer data containing LICENSE.txt and obj/, NOT
the parent assets directory or catalog.json. Default is the absolute data root
recorded at build time (convenience only). Use explicit root for moved binaries;
no cwd-relative fallback, downloading, re-cloning, or model-name guessing occurs.
All catalog paths remain fixed, relative and canonical-root-contained; traversal,
absolute paths and symlink escapes are rejected. Root structure is validated at
startup; each selected model is parsed/validated when requested. Trusted copies
are accepted, not cryptographically authenticated as the original source tree.

```sh
APP=/absolute/path/to/lrender_demo/build/plan-debug/lrender_demo
DATA=/absolute/path/to/lrender_demo/assets/sources/tinyrenderer
cd /tmp
"$APP" --backend vulkan --presentation auto --model african_head --asset-root "$DATA"
"$APP" --backend vulkan --presentation shared --asset-root "$DATA"
"$APP" --backend vulkan --presentation readback --asset-root "$DATA"
"$APP" --backend cpu --presentation readback --model floor --asset-root "$DATA"
# Invalid root: explicit error and exit1, never fallback to compiled resources.
QT_QPA_PLATFORM=offscreen "$APP" --backend cpu --asset-root /does/not/exist
# Same-machine relocation (installed runtime dependencies/RPATH still required):
mkdir -p /tmp/lrender-demo-bin
cp "$APP" /tmp/lrender-demo-bin/lrender_demo
/tmp/lrender-demo-bin/lrender_demo --backend vulkan --asset-root "$DATA"
```

Move/copy of the executable plus explicit resource root is supported on the tested
host; this is NOT a self-contained SDK/driver bundle or cross-machine packaging.
Drag to orbit, wheel to zoom, Reset view frames the installed bounds. Model,
pipeline (Visibility / Pathtrace / PathtraceInteractive), exposure, TAA, ambient, roughness and normal map controls emit values;
appearance commands synchronously acknowledge the actual GUI-applied value, even
when snapshot publication is delayed. Ambient/roughness typing commits on Return
or focus-out (arrows/wheel commit steps); periodic statistics refreshes preserve
uncommitted text and do not roll back acknowledged values. Snapshots synchronize
without signal feedback. Requested and installed
models differ while loading/after failure; old installed World remains on load
failure. Blocked appearance changes do not remain displayed as accepted values.

## Real verification / captures

```sh
timeout 60 "$APP" --backend vulkan --presentation shared --asset-root "$DATA" --self-test full --screenshot /tmp/lrender-shared
QT_SCALE_FACTOR=1.25 timeout 60 "$APP" --backend vulkan --presentation shared --asset-root "$DATA" --self-test full --screenshot /tmp/lrender-dpr125
QT_QPA_PLATFORM=offscreen timeout 60 "$APP" --backend vulkan --presentation auto --asset-root "$DATA" --self-test full
# Required sharing on offscreen must FAIL(exit1), never77 skip:
QT_QPA_PLATFORM=offscreen timeout 60 "$APP" --backend vulkan --presentation shared --asset-root "$DATA" --self-test full
# Real platform probe and optional strict CTest:
/absolute/path/to/lrender_demo/build/plan-debug/demo_qt_vulkan_probe --require-shared --frames 24
# Configure with -DDEMO_ENABLE_STRICT_SHARED_TEST=ON then ctest -L strict_shared.
```

Self-test timers/deadline and Qt injected interactions apply only to --self-test;
ordinary viewer has no45s automatic close. QWidget::grab excludes the native
child: *-widgets.png proves chrome/status, *-native-window.png captures the
application's own native XID, *-presented.png / *-latest-diablo.png are actual GPU
presenter captures. Do not claim a black native child in QWidget grab is a failed
swapchain, or use a chrome-only image as viewport evidence.

## What numbers mean

Completion FPS is renderer receipt count in the last<=1s window, not monitor
refresh/scanout. CPU GUI prepare is measured command/frame assembly wall time;
submit-to-renderer finish wall is upstream FrameResult.wallTime (includes host,
queues/device/finish), NOT GPU execution time. Display events are presenter shown
on Shared or owned-image handoffs on Readback, not photons/widget paints. Dropped,
inflight and accumulated samples are distinct. Presenter handover latency is not
input-to-photon latency. GPU pass sum is only actual complete finite passTimings
from a GPU-identified device; absent/partial values show unavailable, never CPU
substitution. GPU timers are default disabled; upstream profiling serializes
dispatches and changes parallelism. Host/CPU timings cannot enable GPU metrics.

## Assets / permissions / limitations

See [catalog](assets/catalog.json) and [original notices](licenses/ASSET_NOTICES.txt).
The JSON documents the fixed C++ catalog, parts/roles/approximation and exact source
SHA; it is not a dynamic arbitrary-path import language. Diffuse/emissive are sRGB,
normal/spec linear; *_nm_tangent is tangent normal, spec is unbound/not uploaded,
never metallic. Fixed metallic0/roughness0.5 is explicitly approximate (UI may
change roughness). RGBA is top-row-first; OBJ UV flips once. Model source readmes
are preserved; TinyRenderer root software LICENSE is NOT model commercial
redistribution permission. African head example permission, diablo email fragment,
boggie educational permission and floor lack of separate model grant are distinct.
No blanket legal/commercial-use conclusion; do not redistribute without permission.

Known limits: deterministic128MiB bounded catalog texture streaming uses a finite
empty-delta refresh, but the initial factor-only frame may briefly miss textures.
Normal on/off influence and upright UV imagery are observed, not an author
handedness/lighting golden. P9 authoritative runtime log is task-9-shared.log;
task-9-final-shared.log is an older diagnostic, not final acceptance. Historical
2.14s cleanup root cause remains unproven. P10 now observes comparable1–2s
waits specifically in presenter.flush, with GUI responsive. A controlled P9-style
conservative-camera run also exhibits these waits; they are not unique to camera
reuse. Public flush waits Ready/Taken retirement and copied fences; exact underlying
WSI/driver cause and whether it matches historical P4 are NOT established.
P9 PT/TAA camera changes conservatively rebuilt target/presenter/host in198–293ms;
plain Visibility did not. P10 camera-only changes reuse target/presenter/host:
actual upstream PT compares CameraDesc for accumulation, and TAA consumes
cameraCut on the next accepted submit. Real CPU sample1 plus actual GPU/shared/DPR
regression are required evidence, not an assumption cameraCut resets PT. Pipeline,
TAA-enable/disable, scene, extent and native-loss boundaries still drain/rebuild.

P11 implementation at `443acbbeeec174a0c6c966ed9068c7025268777d` passed independent
review (PASS/APPROVED, no Critical/Important findings). Final fresh full CTest
passed 26/26 with zero skips; evidence is
.superpowers/sdd/task-11-final-verification.log. This covers loader/pool/root/
onStopped lifetime assertions, the active-loader shutdown hang, nonblocking wait
diagnostics, zero-shared rejection and explicitly simulated failure boundaries.
True driver hang/physical device loss/device reset/non-XCB sharing remain
unverified; a running native GPU call cannot be forcibly cancelled. Approval is
for the tested Linux/xcb scope, not a cross-platform or hardware-fault guarantee.

Remaining nonblocking review notes: CPU auto fallback diagnostic/error labeling
is ambiguous; strict negative expected text is broad, but exact exit1 plus graceful
root-joined gating protects against arbitrary crashes/timeouts; repeated
close-loading test naming is cosmetic; one manual DPR run observed 101ms maximum
GUI gap; root-joined pool-epilogue waiting is deliberately unbounded/fail-closed,
never an excuse to destroy active resources. These are recorded limitations, not
claims of further functionality. Independent approval does not itself merge,
push or hand over the branch.

## Exact local source lock

All SHAs cross-checked against approved design section7. System Qt/Vulkan/Clang are
not cloned. Source changes or missing checkouts fail configuration/lock validation;
[lock JSON](cmake/dependencies.lock.json) preserves URL/branch/upstream declarations.
The upstream pin differs for cgltf/stb/doctest/astcenc and lqtexec's lexec fallback;
local WHOLE_ARCHIVE/shared-target integration was tested, not silently downgraded.
Doctest reports2.5.0 in its local header despite upstream declaration v2.5.3; use the
full SHA, not the printed version, as the reproducibility identity. Regenerating
locks is a deliberate future review action, not an automatic configure step.

| Local checkout | Full SHA | Unmodified upstream declaration / difference |
|---|---|---|
| third_party/cgltf | `85cd62382dfea638278962690cf515023f33ed00` | 360db1a95480fe102ae9c69b27c5d101167ff5ba |
| third_party/co2 | `a265e5771b0e3ca68d6d473b9bb410fadddf4c23` | a265e5771b0e3ca68d6d473b9bb410fadddf4c23 |
| third_party/compute_shaders | `03db9f228f07269a4ba721b5145461161d31cbd0` | 03db9f228f07269a4ba721b5145461161d31cbd0 |
| third_party/doctest | `d44d4f6e66232d716af82f00a063759e9d0e50d6` | v2.5.3 |
| third_party/lexec | `a2568cc5bf14f69addc7d0e04b1e5ed5c0b2173b` | lrender: a2568cc5bf14f69addc7d0e04b1e5ed5c0b2173b; lqtexec fallback: e032d551d53d87209b83d07fd172890461db643b (not used; shared existing lexec::lexec target) |
| third_party/lqtexec | `f9c8d69d180f5c53bada3c3f408b9c356879a2e5` | direct demo dependency; latest default-branch HEAD at download, frozen by sha |
| third_party/lrender | `d00dcf52c0cac4242464ad0ecd646466aa38ef97` | direct demo dependency; latest default-branch HEAD at download, frozen by sha |
| third_party/stackfull | `acb88611e1de8fbe7ed6e195ba76ad9cbb04a9dc` | acb88611e1de8fbe7ed6e195ba76ad9cbb04a9dc |
| third_party/stb | `4e9b368bfc071e97206ea5c6adba68dd0cbf8897` | 003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5 |
| third_party/tc_astcenc | `2bed807e63d5bd756d26cd00604e5ae3b86e3348` | 5.7.0 |
| third_party/tc_bcdec | `80859ed3b7afb1c527a2a99d70c61457bea72d0c` | 80859ed3b7afb1c527a2a99d70c61457bea72d0c |
| third_party/tc_etcdec | `972875d403ed8ac27e0f35c2f29d819e710a688a` | 972875d403ed8ac27e0f35c2f29d819e710a688a |
| third_party/tl_expected | `1770e3559f2f6ea4a5fb4f577ad22aeb30fbd8e4` | 1770e3559f2f6ea4a5fb4f577ad22aeb30fbd8e4 |
| assets/sources/tinyrenderer | `97eb7a480e762285899a93c5d21e70a79e60782e` | resource source, not a build dependency |


## Shutdown / P11 acceptance
ApplicationLifetime::requestExit is idempotent. GUI keeps processing while Session
settles accepted submissions, drains, destroys target, worker flushes/releases
presenter, GUI retires native host, worker destroys renderer/World/runtime, and
onStopped arrives. Application additionally waits for *all* actual AssetLoader
receipts, including canceled-but-running producers, before releasing loader,
closing its ObjectScope/root and joining root asynchronously. QApplication and
GUI/pool remain alive through root join. No GUI sync_wait/flush/thread join.
Wrong-owner/active-loader/early-eventloop destruction diagnoses a contract
violation rather than silently destroying live resources. Every second during
shutdown a waiting status is logged; it NEVER forcibly frees driver resources.
A hard driver hang cannot be safely recovered by killing a worker. CTest TIMEOUT
is an external test-failure bound, not production shutdown or a recovery claim.

```
cmake -S . -B build/plan-debug -DDEMO_ENABLE_STRICT_SHARED_TEST=ON -DDEMO_ENABLE_GPU_E2E=ON
cmake --build build/plan-debug --target demo_tests demo_e2e demo_qt_vulkan_probe lrender_demo --parallel 2
ctest --test-dir build/plan-debug -L unit --output-on-failure
ctest --test-dir build/plan-debug -L platform --output-on-failure
ctest --test-dir build/plan-debug -L e2e --output-on-failure
build/plan-debug/demo_e2e --backend vulkan --presentation shared --model floor --frames 300 --scenario cycle
QT_SCALE_FACTOR=1.25 build/plan-debug/demo_e2e --backend vulkan --presentation shared --frames 300 --scenario resize
build/plan-debug/demo_e2e --backend cpu --presentation readback --scenario close-loading --close-stage late-loader
```
`demo_e2e` is the same real Widgets/Session application with bounded test-only
scenario actions, not a fake rendering implementation. Cycle actually installs
and waits for a subsequent renderer receipt for each of all four catalog models,
then issues100 coalescible model and100 pipeline commands (NOT100 completed model
installations) and waits for requested real receipts. Resize issues100 coalescible
commands with hide/zero/show, then waits for restored actual rendering. E2E
`--frames`1..10000 bounds receipts, not GPU presentation count. Required sharing
always additionally requires sharedShown>0. GPU tests are opt-in and FAIL rather
than77skip; only ordinary native probe can report77. Negative tests use an exact
exit1+root-joined+diagnostic wrapper, never treat arbitrary crashes/timeouts as
success. `--scenario faults --fault-stage load|submit|present` and
`--inject-zero-shared` are explicit simulated faults, not physical device loss.
For read/decode/install/prepare/submit/drain/native/early-bootstrap closure use
`--scenario close-loading --close-stage load|decode|install|prepare|submit|drain|native|bootstrap|late-loader`.
Late-loader holds an actual bounded producer until GUI heartbeat progresses
AFTER Session onStopped, exercising the prior finish-return hang. On CPU,
native-loss is explicitly simulated; actual Linux/xcb native invalidation is
covered by real-window full self-test. No non-xcb platform support is claimed.
Widget screenshots exclude the native child; `*-widgets.png` is chrome only for
Shared, `*-native-window.png` is the actual own XID capture. No combined image is
represented as an original screenshot or author-approved normal-map golden.

Dependency guard validates14 clean pinned checkouts on every build as well as
configure/CTest. Disposable fixtures test wrong SHA/missing/dirty/traversal/
escaping symlink and cached-build dirtiness; never corrupt actual dependencies.
To reconstruct a lock after an explicitly approved dependency update, inspect
all14 local `git rev-parse HEAD`, source URL/branch/upstream pin declarations,
compare the approved full-SHA list, edit/review records deliberately, then run
VerifyDependencies and clean configure/build/unit. Never regenerate by floating
fetch or blindly replacing locked SHA with current HEAD. Doctest's self-reported
header version remains source metadata distinct from its pinned commit; no
unsupported version or upstream compatibility assertion is inferred.
