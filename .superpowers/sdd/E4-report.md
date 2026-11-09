# E4 — Selection / Hierarchy / Property Panel（C1/I1–I5 修复复核）

- **Branch:** `feat/qt-vulkan-demo`
- **BASE:** `067774a1c580f92c022b6effbca0d278de4517b9` (E3 APPROVED)
- **HEAD:** `f2cfc36e8baf6340b87b70ab51338ea1aa8ccf65` (amended tip may differ by hash; verify with git rev-parse)
- **Asia/Shanghai:** 2026-11-09T21:39:35+0800

## 提交（相对 BASE）

在 `dde5962` E4 初版 FAIL 之后，本轮 TDD 修复 C1/I1–I5 + Minors，普通提交、不 push。

## 修法摘要

### C1 材质写时复制
- 实体首次 MaterialDesc override：以 `materialBase`（安装时 `rememberMaterialBase` / 会话 setRoughness 刷新）完整 desc 为种子，`addMaterial` 拷贝；`MaterialRef` 仍指原材质；World renderable → `copyId`。
- 后续编辑 `replaceMaterial(copyId)`；清除/undo → renderable 指回原材质并 `releaseMaterial` 拷贝。
- 共用材质的其他实体不受影响；会话 `setRoughness`/`setNormalMapping` 只改原始材质，有 override 的实体以 override 为准（已写 Page A1.3 / A3 E4）。
- remirror / clearForModelSwitch / destroy / setMeshMaterial 路径一致释放或丢弃拷贝。

### I1–I4 面板
- **I1** `emitTransform` 从快照 local 复制，保留 rotation，只改 TRS 分量。
- **I2** `setKeyboardTracking(false)` + FocusIn/Out → `beginEditMerge`/`endEditMerge`（MergeGuard）。
- **I3** 有焦点或值相同时跳过 `setValue`。
- **I4** 实体 Camera FOV/near/far **不驱动视口**（World 无 removeCamera；视口用 session CameraState）；面板 `cameraViewportNote` 标明；Page/报告已写。

### I5 / Minors
- 重写 `ui_editor_test` 走面板槽；会话层 hierarchy undo、MaterialDesc CoW undo/redo、offscreen 面板链路。
- Minor1 同父跳过 reparent；Minor2 灯光保留类型/颜色/方向；Minor3 无 override 时显示 `materialBase`；Minor4 MainWindow catch 不 rethrow；Minor5 Page A3 E4/E5 + A1.3。

## 变异检查（6/6 CATCH）

| 变异 | 测试 | 结果 |
|------|------|------|
| shared `replaceMaterial`（无 CoW） | `editor/material-override-cow-*` | **FAIL→CATCH** |
| `setEntityParent` 不记栈 | `session/hierarchy-undo-*` | **CATCH** |
| `forgetEntity` 不检查父引用 | `undo/forget-entity-drops-parent-refs` | **CATCH** |
| 去掉 `syncing_` | `ui_editor/*` | **CATCH** |
| `setEntityMaterialDesc` 不记栈 | `session/material-desc-cow-*` | **CATCH** |
| 快照不填 `materialOverride` | `session/material-desc-cow-*` | **CATCH** |

## 验证（Asia/Shanghai）

`DISPLAY=:1`；`DEMO_BUILD_DIR=build/plan-debug`；ASan 树 `/tmp/e3r1-asan-build`。

| 项 | 结果 |
|----|------|
| 单测 | **254/254** PASS，**403964** assertions，0 skipped |
| `DEMO_EDITOR_STRESS=1` | **PASS**（~28.2M assertions） |
| ASan+UBSan | **254/254** PASS（PropertyPanel 对 Qt functor 关 `-fno-sanitize=vptr`） |
| CTest（解锁，串行，`DISPLAY=:1`） | **27/27** PASS，**0 SKIP** |
| `acceptance_gpu.sh` | **ACCEPTANCE COMPLETE** |

### 二进制 mtime（+0800）

- `demo_tests` / `lrender_demo` ≈ **2026-11-09 21:36:48 +0800**
- 均 **新于** 全部自有源码（含 `CMakeLists.txt` @ 21:36:44）；`sources >= bin mtime = 0`。

## Page

已 EditCanvas 附录 A3 E4/E5 行与 A1.3：视口点选→E5；C1 材质语义；I4 实体相机不影响视口。
[lrender_demo｜获批设计 v1.0 + 附录A v1.1（已批准）](https://cursor.com/canvas/store-1530fd2b-879b-49fb-a1b7-86e8dbc8401e/290b72c2-9c2d-450b-8cd5-ae22d461839b)

## 未验证

- 视口点选（E5）；Gizmo；MaterialDesc 多纹理字段 UI；Light/Camera undo；ASan GPU e2e；真实窗口层级拖拽 soak；World 侧二次 `replaceMaterial` 后 `extractDelta.materials` 偶发空（编辑器/invariant 已覆盖 CoW 语义，未把空 delta 当硬门禁）。
