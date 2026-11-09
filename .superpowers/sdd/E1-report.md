# E1 — lexec sender 工作流（复核补齐）

- **Branch:** `feat/qt-vulkan-demo`
- **BASE:** `80528df881d0c1f5c22d8c26f7e11221d986bbaa`
- **HEAD:** feat/qt-vulkan-demo tip — run `git rev-parse HEAD`
- **Asia/Shanghai:** 2026-11-09T16:45:00+08:00

## 范围完成情况

| 项 | 状态 |
|----|------|
| Soft：`drain \| then_on(GUI, destroy+bind)`，不 release | 完成 |
| Hard：`drain \| then_on(destroy) \| release \| then_on(create/finish-stop)` | 完成 |
| upon_error → GUI hop（M1） | 完成 `guiDrainFault` |
| DoneOpSender stop 时仍 set_value(cancelled)（M2） | 完成 |
| workflow 覆盖 hard/soft/错误/取消/early-return（M3） | 完成 6 用例 |
| 黑帧锁屏/息屏 → exit 77 + CTest SKIP（M4） | 完成 |
| AssetLoader：`associate` + 既有 `write_env` 取消 | 完成 |
| SessionFrameSubmit：`associate` | 完成 |
| 加载/管线切换值命令 | 保持 latest-wins（A1.6）；取消走 LoadTicket/`write_env` stop |
| `lexec::stop_when` 公共 API | **上游未导出**（见局限） |

## 屏幕状态（只读，验收前）

```
loginctl session 2: LockedHint=yes IdleHint=yes Active=yes
xset: DPMS Enabled, Monitor is Off
```

黑帧自测：`exit 77`，明确 SKIP 文案。
**等待解锁后重跑验收：黑帧 ×3、DPR1.25、负例。**（本轮未尝试解锁）

锁屏下仍作为功能证据跑通：`e2e_shared_cycle`、`e2e_shared_dpr125`、unit、负例等。

## 证据

### 二进制 mtime（Asia/Shanghai）

```
lrender_demo  2026-11-09 16:44:04 +0800
demo_e2e      2026-11-09 16:44:04 +0800
demo_tests    2026-11-09 16:44:01 +0800
```

### 单测

- `QT_QPA_PLATFORM=offscreen demo_tests --no-skip=true`
- **171/171** passed，**6591** assertions（含 workflow/* ×6）
- 日志：`/tmp/e1i1-unit2.log`

### CTest

- **27 tests：100% passed，1 skipped**（`e2e_shared_black_flash` SKIP 77）
- 含 DPR `e2e_shared_dpr125` Passed；负例标签通过
- 日志：`/tmp/e1i1-ctest.log`

## 上游 API 证据：`stop_when`

- 文件：`third_party/lexec/include/lexec/algorithms/stop_when.hpp`
- `stop_when` 定义在 **`namespace lexec::detail`**（约 L17–L146），**未** `using`/`inline constexpr` 到 `lexec::`
- 公共取消路径：`write_env` + `prop{get_stop_token, …}`（AssetLoader 已用）；`spawn` 内部使用 `detail::stop_when`
- **未**调用 `lexec::detail::stop_when`（避免依赖 detail）

## 保持的保证

不阻塞 GUI；soft 不 release（不闪黑）；hard 序 drain→destroy→release；drain/release 错误 fail-closed 保留 target；GUI 独占状态；值命令/不可变快照。

## 已知局限

1. 黑帧验收待解锁后重跑 ×3（本轮 SKIP）。
2. `lexec::stop_when` 无公共导出；取消用 `write_env` stop token。
3. RenderSession 仍偏大；soft/hard 已收敛到 workflows + `then_on`，prepare/install 小步仍在 tick 内（符合 GUI 有界安装）。
4. `.superpowers/` 在 `.gitignore`；本报告因此前已跟踪而可普通 `git add`（M5：不再 `-f` 强制新报告）。

## 未做

未 push/amend/改第三方/动桌面解锁；未开始 E2。

