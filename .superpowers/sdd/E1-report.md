# E1 — lexec sender 工作流（m1–m6 验收补齐）

- **Branch:** `feat/qt-vulkan-demo`
- **BASE:** `80528df881d0c1f5c22d8c26f7e11221d986bbaa`
- **Parent (本提交之前):** `6d3c6fa785426ced605b2c4bd3cc648f132d46bb`
- **HEAD (本提交):** `e7a9956046fe60d95d0866fe0bc6a9aae353c6f5`
- **Asia/Shanghai:** 2026-11-09T17:01:00+08:00

## m1–m6 修改

| 项 | 状态 |
|----|------|
| m1 锁屏检测用 `$XDG_SESSION_ID` / `$DISPLAY` 匹配，不用 seat0 首会话 | 完成 `resolveLogindSessionId` |
| m2 `DEMO_FORCE_BLACK_FLASH_RUN` + `scripts/acceptance_gpu.sh` | 完成 |
| m3 M2 措辞：取消 → `DrainFailed` fail-closed，不是继续关机 | 完成（注释+报告；行为未改） |
| m4 `preparePathSender` 接入 `prepare()` | 完成 |
| m5 去掉重复 `associate`（`spawn(token)` 已 `token.wrap`） | 完成 AssetLoader / SessionFrameSubmit |
| m6 报告写真实 parent SHA；workflow ×6；断言实测 | 见证据 |

## 验收脚本 `scripts/acceptance_gpu.sh`

- 启动前检查锁屏/息屏（同 m1 会话解析）；锁定则 exit 2「未完成」
- 黑帧 ×3 → DPR1.25 → cycle → 5 负例（submit_fault / fault_load / fault_present / zero_shared / strict_offscreen）
- 任一项 SKIP → 未完成（exit 2）
- 汇总 blackSamples / hostChanges / sharedShownGap / latency / sharedShown / resize capture
- E2–E8 可复用（`DEMO_BUILD_DIR`）

## 屏幕状态（验收前只读）

```
session 2: LockedHint=no IdleHint=no Active=yes Display=
xset: DPMS Enabled, Monitor is On
gdbus ScreenSaver.GetActive = false
DISPLAY=:1
```

已解锁亮屏 → 直接跑验收脚本。

## 证据

### 二进制 mtime（Asia/Shanghai）

```
libdemo_core.a  2026-11-09 16:59:54 +0800
demo_tests      2026-11-09 16:59:58 +0800
lrender_demo    2026-11-09 16:59:58 +0800
demo_e2e        2026-11-09 16:59:58 +0800
```

### 单测

- `QT_QPA_PLATFORM=offscreen ./demo_tests`
- **171/171** passed，**6590** assertions
- workflow：`*workflow*` → **6** cases，**43** assertions
- 日志：`/tmp/e1-m-unit.log`

### CTest（完整）

- **27/27** passed（含 `e2e_shared_black_flash` Passed，无 SKIP）
- 日志：`/tmp/e1-m-ctest.log`

### acceptance_gpu.sh

- **ACCEPTANCE COMPLETE**（exit 0）
- 日志：`/tmp/e1-m-acceptance.log`
- 黑帧 ×3：全部 `blackSamples=0`，`hostChanges=0`，`sharedShownGap=0`；resize capture `220x160`
- DPR1.25：sharedShown≈230；cycle：sharedShown≈295
- 5 负例：全部 PASS（expect_failure）

## 上游 API：`stop_when`

- 仍仅 `lexec::detail::stop_when`；取消用 `write_env` + stop token；`spawn` 内部 wrap，不再双重 `associate`

## 保持的保证

不阻塞 GUI；soft 不 release；hard 序 drain→destroy→release；取消/错误 → DrainFailed fail-closed 保留 target；GUI 独占状态；值命令/不可变快照。

## 已知局限

1. `lexec::stop_when` 无公共导出。
2. RenderSession 仍偏大；prepare 已 sender 化，probe 仍为 Done 回调。
3. `.superpowers/` 若在 ignore 列表则勿 `-f`；本路径此前已跟踪。

## 未做

未 push / amend / 改第三方 / 动 `.vscode` / 解锁操作（本轮已自行解锁）；未开始 E2。
