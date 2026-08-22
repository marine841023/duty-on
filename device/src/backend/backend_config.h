#pragma once

// ---------------------------------------------------------------------------
// 后端集中配置常量 —— 从 src-tauri/src/config.rs 1:1 移植。
// 仅后端使用；客户端渲染侧常量在 ui_renderer.cpp 内联。
// ---------------------------------------------------------------------------

#include <cstdint>

namespace dutyon::backend::bc {

// ===== HTTP 服务 =====
constexpr uint16_t kPort = 17521;
constexpr const char* kHost = "127.0.0.1";

// ===== 状态超时（毫秒）=====
// Working 会话静默这么久后降为 Idle。必须宽松：Qoder 的 ask-user 对话框
// （完全不发 hook 事件）会让会话合法地静默几分钟。
constexpr uint64_t kWorkingTimeout = 3 * 60 * 1000;
constexpr uint64_t kSessionTimeout = 10 * 60 * 1000;  // 静默 10 分钟删除会话
// 工具执行中（PreToolUse 已到、PostToolUse 未到）的放宽超时。实测
// （~/.dutyon/hook-received.log 抓包）：工具执行期 Pre/Post 之间完全静默
// 是常态 —— 长构建、长测试、子代理跑长命令动辄几分钟（子代理工具调用
// 与主代理同 session 发 hook，agent_id 不同）；3 分钟工作超时会在工具
// 还在跑时把宠物置睡（用户反馈"子代理还在执行就变空闲"）。
// 有 pending 工具的会话降级/删除超时放宽到此值。
constexpr uint64_t kToolRunningTimeout = 15 * 60 * 1000;
constexpr uint64_t kAlertReminder = 60 * 1000;        // alert 状态下每分钟重提醒
constexpr uint64_t kCleanupIntervalMs = 30 * 1000;    // 清理定时器节拍

// ===== 系统监控 =====
constexpr uint64_t kMonitorIntervalMs = 1500;  // 采样间隔（秒以上才有意义的 CPU%）

// ===== Hook 桥接 =====
constexpr uint64_t kBridgeTimeoutSec = 5;
// 写入 ~/.trae-cn/hooks.json 的事件（与安装逻辑保持同步）
inline const char* const kHookEvents[] = {
    "SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop", "Notification",
};
constexpr int kHookEventsCount = 6;

// 写入 ~/.qoder/settings.json 的事件：IDE 官方只支持前四个，但 IDE 与 CLI
// 共用配置文件，额外接上 Notification/PermissionRequest —— 若 IDE 的
// ask-user 对话框触发它们（未文档化），宠物就能收到"等待用户"信号。
inline const char* const kQoderHookEvents[] = {
    "UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop", "Notification", "PermissionRequest",
};
constexpr int kQoderHookEventsCount = 6;

// 写入 ~/.cursor/hooks.json 的事件：Cursor 用自己的 camelCase 事件名和更
// 扁平的 schema；桥接脚本把它们归一化为状态机认识的规范名：
//   sessionStart -> SessionStart / beforeSubmitPrompt -> UserPromptSubmit /
//   preToolUse -> PreToolUse / postToolUse -> PostToolUse / stop -> Stop
inline const char* const kCursorHookEvents[] = {
    "sessionStart", "beforeSubmitPrompt", "preToolUse", "postToolUse", "stop",
};
constexpr int kCursorHookEventsCount = 5;

// 写入 ~/.codex/hooks.json 的事件：与 Trae 同名同 schema，无需归一化。
inline const char* const kCodexHookEvents[] = {
    "SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop", "PermissionRequest",
};
constexpr int kCodexHookEventsCount = 6;

// OpenCode 插件目录（~/.config/opencode/plugins，无配置式 hook）
constexpr const char* kOpencodePluginSubdir = ".config/opencode/plugins";
constexpr const char* kOpencodePluginFilename = "dutyon-bridge.js";

// 调用即代表"等待用户输入"的工具（Qoder 无 Notification 事件，AskUserQuestion
// 就是它的 alert 信号：PreToolUse -> confirmation-needed，用户作答后的
// PostToolUse -> thinking）。
inline const char* const kAskUserTools[] = {
    "AskUserQuestion", "AskQuestion", "ask_question", "askQuestion",
};
constexpr int kAskUserToolsCount = 4;

// 必须在 IDE 里点"运行"确认后才执行的工具（computer-use 卡片等）。
// Claude 风格 hook 在确认卡片出现时就触发 PreToolUse —— 即代理正被阻塞
// 等用户 —— 所以是 confirmation-needed 而非 tool-use。精确匹配（大小写
// 变体枚举）避免误伤名字里恰好含这些词的良性工具。
inline const char* const kConfirmTools[] = {
    // computer use 家族
    "computer_use", "computer-use", "computerUse", "ComputerUse", "COMPUTER_USE",
    "use_computer", "useComputer", "UseComputer", "computer", "Computer",
    // browser/desktop 自动化家族
    "browser_use", "browser-use", "browserUse", "BrowserUse", "browser", "Browser",
};
constexpr int kConfirmToolsCount = 16;

// ===== Notification 分类（StateManager::checkConfirmationNeeded 用）=====
// 官方完成类仅 idle_prompt（智能体完成当前任务）；其余为其他 IDE/旧版
// 兼容名。见 https://docs.trae.cn/ide/reference-for-hooks-configuration
inline const char* const kNotificationCompleteTypes[] = {"task_complete", "idle", "done", "idle_prompt"};
constexpr int kNotificationCompleteTypesCount = 4;
// 官方等待用户类（TraeCode 文档全部 4 个）：permission_prompt（工具待确认）、
// document_review（Plan/Spec 文档审阅）、ask_user_question（提问等补充
// 信息）、browser_interaction（浏览器交互等待）。permission_request /
// confirmation / input_needed 为其他 IDE 兼容名。
inline const char* const kNotificationConfirmTypes[] = {
    "permission_request", "permission_prompt", "confirmation", "input_needed",
    "document_review", "ask_user_question", "browser_interaction",
};
constexpr int kNotificationConfirmTypesCount = 7;
// 其中定义上绑定 in-flight 工具的子集（迟于 PostToolUse 送达即过期，
// 见 StateManager 乱序防护）：permission_prompt 类。document_review /
// browser_interaction / ask_user_question 是 UI 流程等待，不在此列。
inline const char* const kNotificationToolBoundConfirmTypes[] = {
    "permission_request", "permission_prompt", "confirmation", "input_needed",
};
constexpr int kNotificationToolBoundConfirmTypesCount = 4;

// 表明 Notification 在请求用户确认的关键词（覆盖"运行命令/执行命令"措辞 ——
// Trae IDE 的 "Trae wants to run command: ..." 即使没有 tool_name 也要触发 alert）
inline const char* const kNotificationConfirmKeywords[] = {
    "确认", "允许", "授权", "运行命令", "执行命令", "想要运行", "想要执行",
    "permission", "confirm", "allow", "approve",
    "run command", "execute command", "wants to run", "want to run",
    "wants to execute", "want to execute",
};
constexpr int kNotificationConfirmKeywordsCount = 18;

// 歧义 Notification（working 中收到、无法分类）的处理：
//   false = 当作任务完成（-> idle），避免误报 alert（默认）
constexpr bool kAlertOnAmbiguousNotification = false;

// ===== IDE 窗口扫描 =====
// 自适应间隔：有会话 4s，无会话 15s。
constexpr uint64_t kScanIntervalActiveMs = 4000;
constexpr uint64_t kScanIntervalIdleMs = 15000;
// 窗口标题形如 "<file> - <project> - Trae CN"；新版 Trae 改叫 "TraeCode CN"，两者都匹配。
constexpr const char* kTraeTitleSuffix = " - Trae CN";
constexpr const char* kTraeCodeTitleSuffix = " - TraeCode CN";
constexpr const char* kQoderTitleSuffix = " - Qoder";
constexpr const char* kCursorTitleSuffix = " - Cursor";
// Cursor 3.x 独立的 Agents 面板窗口：无工作区时标题就叫 "Cursor Agents"
constexpr const char* kCursorAgentsTitle = "Cursor Agents";
constexpr const char* kCursorAgentsTitleSuffix = " - Cursor Agents";

} // namespace dutyon::backend::bc
