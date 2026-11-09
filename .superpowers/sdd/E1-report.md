# E1 — lexec sender 工作流重写

- **Branch:** `feat/qt-vulkan-demo`
- **BASE:** `80528df881d0c1f5c22d8c26f7e11221d986bbaa`
- **HEAD:** feat/qt-vulkan-demo tip after E1 (run: git rev-parse HEAD)
- **Asia/Shanghai:** 2026-11-09T16:25:00+08:00
- **Page:** 附录 A 已更新为「v1.1 已批准」（entt + 同窗双视口；不覆盖 v1.0）

## 改动摘要

1. **`src/render/workflows/AdapterSenders.h`**（新）
   - `DoneOpSender`：把 `PresentationAdapter::Done` 包装为仅 `set_value(Result)` / `set_stopped` 的 sender（无 `set_error`，便于 `spawn`）。
   - `beginDrainSender` / `releasePathSender`。

2. **`src/render/workflows/SessionWorkflows.h`**（新）
   - `softDrainOnly` → `beginDrainSender`（soft 保 presenter/host）。

3. **`src/render/RenderSession.cpp`**
   - **Soft（VulkanShared）：** `softDrainOnly | then(control-hop) | upon_error` + `spawn`。GUI 投递仍走既有 `control()`（QTimer post），避免与 `adapter.work()` 的 `then_on` 嵌套重入；语义对齐 pre-E1 soft/hard 规则。
   - **Hard `beginDrain`：** `beginDrainSender | then(control→destroyTarget→releasePathSender|then(control))` + `upon_error` + `spawn`。`control()` 在 `~RenderSession` 重置 `uiScope` 之后仍可用（asset_async 析构路径依赖此点）。

4. **`tests/workflow_test.cpp`** + **CMakeLists.txt**
   - 3 个用例：`drain-then-release-order-on-gui`、`drain-error-skips-release`、`soft-drain-only-never-releases`。
   - 组合真实 `lexec`/`lqtexec` 算法：`spawn`、`then`、`then_on`、`upon_error`、`counting_scope`。

## 保持的保证

| 保证 | 做法 |
|------|------|
| 不阻塞 GUI | drain/release 经 adapter worker；完成用 `control()`/`then_on` 回 GUI；无 `sync_wait` |
| 不闪黑 | soft 只 drain，不 `releasePath`；hard 才 destroy→release |
| 按序 drain | hard：drain → destroy target → release → Idle/destroyResources |
| fail-closed | `upon_error` / `RenderError` → `DrainFailed` + `report` |
| RenderSession 独占 | 未向 UI 暴露 World/target；值命令/快照不变 |
| 真实算法 | 仅用头文件中的 `lexec::then/upon_error/spawn`、`lqtexec::then_on`、`counting_scope` |

## 证据

### 二进制 mtime（Asia/Shanghai，箱内本地）

```
build/plan-debug/lrender_demo  2026-11-09 16:21:19.059415767 +0800
build/plan-debug/demo_e2e      2026-11-09 16:21:19.053673715 +0800
build/plan-debug/demo_tests    2026-11-09 16:21:19.182308953 +0800
```

### 单测

- `QT_QPA_PLATFORM=offscreen demo_tests --no-skip=true`
- **168/168** passed，6572 assertions（含新增 3× `workflow/*`）
- 日志：`/tmp/e1-unit-final.log`

### CTest（除黑帧）

- `ctest -E e2e_shared_black_flash` → **26/26** passed
- 含 DPR：`e2e_shared_dpr125` Passed 4.79s
- 含负例标签：`e2e_zero_shared`、`e2e_strict_offscreen`、`e2e_fault_*` 等
- 日志：`/tmp/e1-ctest-nobf.log`

### 显式负例（期望非零）

| 用例 | exit |
|------|------|
| `--inject-zero-shared` | 1 |
| `QT_QPA_PLATFORM=offscreen` + shared resize | 1 |
| `--asset-root` 缺失目录 | 1 |

### 黑帧 e2e ×3

| run | exit | soft extent | final |
|-----|------|-------------|-------|
| 1 | 1 | 160x120 | completed=2 sharedShown=2 |
| 2 | 1 | 160x120 | completed=2 sharedShown=2 |
| 3 | 1 | 160x120 | completed=2 sharedShown=2 |

日志：`/tmp/e1-black-flash-x3.log`

## 已知局限

1. **`e2e_shared_black_flash` 当前环境失败（非 E1 逻辑回归）**
   - Soft 完成正常（`soft target extent=160x120`）。
   - Visibility 非 continuous：soft 后脏帧耗尽即 idle（`completed/sharedShown=2` 符合）。
   - Stage0 卡在 `nearBlack(grabWindow)`：Vulkan shared 窗口抓屏持续近黑，45s 内部 deadline 退出。
   - **对照：** 将 `RenderSession.cpp` 整文件回退到 `80528df` 后同命令同样失败；` --self-test full` 在同环境通过（exit 0）。
   - 故判为 **grabWindow/近黑门禁的环境问题**，不是 sender 改写引入的 soft/hard 破坏。晨间 `fix-black-flash-timeout` 绿跑记录仍有效作历史基线。

2. **E1 未改加载/管线切换/退出的其它回调路径**（仍为既有 `control`/loader sender）；后续 E 任务再扩。

3. **未开始 E2**（entt）。

## 未做

- 未 push / merge / amend / `--no-verify` / worktree
- 未改 `.vscode` / 第三方源码
