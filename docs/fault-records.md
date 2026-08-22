# 故障记录：状态机 Hook 事件处理（2026-08-22）

本文档记录 2.0（`v2.0-dev`，C++ 单进程）状态机修复的三个故障。**三个故障在 1.x（`master`，Tauri + Rust）中同样存在**（已核对 `src-tauri/src/state_manager.rs` 与 `config.rs`），文末附 1.x 修复建议。

官方规则依据：[Trae Hook 配置详解](https://docs.trae.cn/ide/reference-for-hooks-configuration)

---

## 故障 1：子代理/长工具执行期间宠物误睡

### 现象

子代理（Task 工具）还在执行长命令，宠物已切换到睡觉状态；主代理跑长构建/长测试（超过 3 分钟）同样误睡。用户反馈："子代理还在执行，主进程已经变成空闲了"。

### 根因

`WORKING_TIMEOUT = 3 分钟`：会话静默 3 分钟即降 Idle。但工具执行期间（`PreToolUse` → `PostToolUse` 之间）**完全静默是常态**——没有任何 hook 事件发出，长构建、长测试、子代理长任务动辄几分钟到十几分钟。

### 排查证据（抓包 `~/.dutyon/hook-received.log`）

```
18:38:02 PreToolUse  agent=general_purpose_task  ← 子代理的 RunCommand 开始
（4.5 分钟完全无事件）
18:42:35 PostToolUse agent=general_purpose_task  ← 工具完成
```

关键事实：
- **Trae 新版是原生 HTTP 直连 POST 17521**（payload 带 `agent_id`/`tool_name`/`tool_use_id`），不走 bridge 脚本（bridge.log 长期无记录）
- 子代理的工具调用**确实触发 hook**（与主代理同 `session_id`，`agent_id` 不同）
- **Task 工具本身不发 hook**（启动子代理无 PreToolUse）
- 4.5 分钟探针实验：旧版 3 分 20 秒处 overall 降 sleeping

### 2.0 修复（`v2.0-dev`）

pending 工具配对记账（`state_manager.cpp`）+ 放宽超时（`backend_config.h`）：

- `PreToolUse` → 记 `Session::pending_tool`（优先 `tool_use_id`，退化 `tool_name`）
- `PostToolUse` / `SessionStart` / `UserPromptSubmit` / `Stop` → 清账
- **Notification 不清账**（异步事件不推进工具生命周期，工具待批准期间仍算 in-flight）
- 有 pending 的会话：降级/删除超时放宽到 `kToolRunningTimeout = 15 分钟`

验证：同场景 4.5 分钟探针，全程 `overall=working` 不降级。

### 1.x 修复建议（`master`）

`src-tauri/src/state_manager.rs` / `config.rs`：

1. `Session` 结构加 `pending_tool: Option<String>` 字段
2. 事件分发处加同样的记账逻辑（注意 Notification 分支不清账）
3. `cleanup_stale_sessions` 中：`pending_tool.is_some()` 时降级/删除阈值改用新常量 `TOOL_RUNNING_TIMEOUT: u64 = 15 * 60 * 1000`
4. HookEvent 解析处补 `tool_use_id: Option<String>` 字段（PreToolUse/PostToolUse payload 均携带）

---

## 故障 2：Notification 通知类型遗漏 3 个官方取值

### 现象

Plan/Spec 文档审阅、智能体提问（AskUserQuestion 通知形态）、浏览器交互等待三类场景，宠物不提醒（落入歧义兜底判定；`ALERT_ON_AMBIGUOUS_NOTIFICATION = false` 时直接当任务完成 → 睡觉）。

### 根因

官方 `notification_type` 共 5 个取值，系统只分类了 2 个：

| 官方类型 | 触发时机 | 修复前 |
|---|---|---|
| idle_prompt | 智能体完成当前任务 | ✅ 完成类 |
| permission_prompt | 工具调用需用户确认 | ✅ 确认类 |
| **document_review** | Plan/Spec 工作流文档审阅 | ❌ 未分类 |
| **ask_user_question** | 智能体需要用户补充信息 | ❌ 未分类 |
| **browser_interaction** | 浏览器交互等待 | ❌ 未分类 |

### 2.0 修复（`v2.0-dev`）

`backend_config.h` 的 `kNotificationConfirmTypes` 补入 `document_review` / `ask_user_question` / `browser_interaction`（4 → 7 项）。

### 1.x 修复建议（`master`）

`config.rs` 的 `NOTIFICATION_CONFIRM_TYPES` 同样补入这 3 个字符串。

---

## 故障 3：异步通知乱序导致 alert 永久卡死

### 现象

真实会话上抓到的活体 bug：会话卡在 `confirmation-needed`，`lastEvent=Notification`，宠物持续警报，但 IDE 里根本没有任何等待确认的对话框。

### 根因

官方文档明确 **Notification 是异步事件、不阻塞主流程**——`permission_prompt` 可能在对应工具已经完成（`PostToolUse` 已到）**之后**才送达。迟到的确认通知把已经恢复工作（thinking）的会话又打成 ConfirmationNeeded；而 **ConfirmationNeeded 豁免所有超时清理**（ask-user 对话框期间无事件是预期的）→ 假警报永久卡死。

### 2.0 修复（`v2.0-dev`）

乱序防护（`state_manager.cpp`）：

- `permission_prompt` 类（`kNotificationToolBoundConfirmTypes`：permission_request / permission_prompt / confirmation / input_needed）定义上绑定 in-flight 工具（payload 带 `tool_use_id`）→ 到达时 `pending_tool` 已空即判定过期，忽略（保持现状态）
- **UI 流程类不设此防护**：document_review / browser_interaction / ask_user_question 是界面流程等待，不一定有 in-flight 工具，无 pending 也必须警报
- 依赖故障 1 的修复（pending 记账 + Notification 不清账）——若 Notification 清账，合法的待确认通知会被误判过期

### 1.x 修复建议（`master`）

依赖故障 1 的 `pending_tool` 字段，在 `check_confirmation_needed` 或 Notification 分支加同样的乱序防护。注意 1.x 的 Qoder 靠 AskUserQuestion 工具对（Pre/PostToolUse）而非 Notification 触发 alert，不受此乱序影响；Trae 原生 Notification 路径受影响。

---

## 验证方法（两版本通用）

```powershell
# 场景 A：合法待确认（Pre → permission_prompt → Post）
@{session_id='t'; cwd='D:\t'; hook_event_name='PreToolUse'; tool_name='RunCommand'; tool_use_id='t1'} | ConvertTo-Json -Compress
# → tool-use；补发 Notification permission_prompt → confirmation-needed；PostToolUse → thinking

# 场景 B：迟到通知（Post 之后再发 permission_prompt）→ 必须保持 thinking（不卡 alert）

# 场景 C：UI 流程类（无 pending 直接发）→ document_review / ask_user_question / browser_interaction 均 → confirmation-needed

# 场景 D：完成类 → idle_prompt → idle（不被乱序防护误伤）

# 场景 E：长工具静默（子代理跑 4.5 分钟单命令，每 30 秒查 /status）→ 全程 working
```

2.0 已全场景回归通过（2026-08-22）。

---

## 附：本次核对的官方规则要点

- 6 类事件：SessionStart / UserPromptSubmit / PreToolUse / PostToolUse / Stop / Notification（`PermissionRequest` 是 Claude Code 兼容名，非 Trae 官方个人版事件）
- Notification 异步、不阻塞、matcher 按 notification_type 匹配
- PreToolUse/PostToolUse 的 stdin 带 `tool_use_id`（配对依据）、`tool_name`（标准化名）、`llm_tool_name`（原始名）
- 标准工具名 12 个：Read/Write/Edit/Glob/Grep/LS/RunCommand/WebSearch/WebFetch/AskUserQuestion/Skill/mcp__*
- 全局配置 `~/.trae-cn/hooks.json`，version:1，matcher 缺省=全匹配
