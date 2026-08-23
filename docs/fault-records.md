# Hook 状态机故障记录

来源：v2.0（C++ 版）开发期实测发现，1.x（Rust 版）同源存在，均已在两分支修复。
完整版（含 2.0 代码位置）见 `v2.0-dev` 分支同名文件。

## 故障 1：工具执行静默期误判空闲（已修复）

- 现象：子代理跑长命令/长构建时宠物提前睡觉。
- 根因：PreToolUse → PostToolUse 之间完全无 hook 事件，3 分钟
  `WORKING_TIMEOUT` 先到，把执行中的会话降为 Idle。
- 修复：pending_tool 记账（Pre 记 / Post·SessionStart·UserPromptSubmit·Stop
  清；Notification 不清——异步事件不推进工具生命周期），有 pending 工具的
  会话降级/删除超时放宽到 `TOOL_RUNNING_TIMEOUT`（15 分钟）。

## 故障 2：Notification 类型遗漏（已修复）

- 现象：Plan/Spec 审阅、ask_user_question、browser_interaction 等官方
  notification_type 未纳入确认分类，该弹叹号时不弹。
- 修复：确认分类补全为 7 值；歧义通知默认按完成处理
  （`ALERT_ON_AMBIGUOUS_NOTIFICATION = false`）。

## 故障 3：乱序 Notification 卡死 alert（已修复）

- 现象：迟到的 permission_prompt 类通知（PostToolUse 已到、无 in-flight
  工具）触发 ConfirmationNeeded，而该状态豁免一切超时 → 红叹号永久不消。
- 修复：工具绑定类确认通知（permission_request/permission_prompt/
  confirmation/input_needed）到达时若无 pending_tool 判过期忽略；
  UI 流程类（document_review/ask_user_question/browser_interaction）不设防。

## 故障 4：LLM 长生成被误判空闲（已修复，v1.3.3）

- 现象：项目正在跑（模型生成长回复中），宠物显示空闲睡觉。
- 实测：PostToolUse 后 LLM 连续生成 3 分 52 秒才发出下一 PreToolUse；
  生成期完全静默，3 分钟 `WORKING_TIMEOUT` 先到把 Thinking 会话误降 Idle。
- 修复：新增 `THINKING_TIMEOUT`（10 分钟）单独兜底 Thinking（LLM 生成期）
  降级；Working/ToolUse/pending_tool 分档不变。
  - 2.0：`device/src/backend/backend_config.h` `kThinkingTimeout`
  - 1.x：`src-tauri/src/config.rs` `THINKING_TIMEOUT`
- 回归测试：`thinking_survives_working_timeout_and_decays_on_its_own`
