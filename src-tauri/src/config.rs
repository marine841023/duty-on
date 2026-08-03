//! Centralized configuration constants — server port/host, window geometry,
//! state timeouts, hook bridge events, and IDE scanner tuning.
//!
//! Only the backend uses these. Renderer-side constants stay inline in
//! `renderer.js` (the renderer runs in WebView2 with no Rust access).

use once_cell::sync::Lazy;
use regex::Regex;

// ===== HTTP server =====
pub const PORT: u16 = 17521;
pub const HOST: &str = "127.0.0.1";

// ===== Window =====
pub const WINDOW_WIDTH: i32 = 260;
pub const WINDOW_HEIGHT: i32 = 420;
pub const WINDOW_MARGIN: i32 = 20; // offset from bottom-right corner
// Mini mode: half-size window (人物宽高 1/2, status bar half width).
pub const MINI_WINDOW_WIDTH: i32 = 130;
pub const MINI_WINDOW_HEIGHT: i32 = 210;

// ===== State timeouts (ms) =====
/// Working sessions drop to idle after this much silence. Must stay generous:
/// a working session can legitimately go silent for minutes (long tool runs,
/// or Qoder's ask-user dialog which fires no hook event at all).
pub const WORKING_TIMEOUT: u64 = 3 * 60 * 1000;
pub const SESSION_TIMEOUT: u64 = 10 * 60 * 1000; // 10 min: session removed if silent
pub const ALERT_REMINDER: u64 = 60 * 1000; // 1 min: re-alert interval while in alert
pub const CLEANUP_INTERVAL_MS: u64 = 30 * 1000; // cleanup timer tick

// ===== Hook bridge =====
pub const BRIDGE_TIMEOUT_SEC: u64 = 5;
// Events wired into ~/.trae-cn/hooks.json (keep in sync with install-hooks logic).
pub const HOOK_EVENTS: &[&str] = &[
    "SessionStart",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "Stop",
    "Notification",
];

// Events wired into ~/.qoder/settings.json. The Qoder CLI documents a much
// larger event set than the IDE (SessionStart, Notification, PermissionRequest,
// ...); the IDE officially supports only the first four below, but since IDE
// and CLI share the config file and presumably the hook engine, we additionally
// wire Notification + PermissionRequest: if the IDE's ask-user dialog fires
// them (undocumented), the pet gets its "waiting for user" signal; if not,
// the extra entries are simply never invoked. Sessions are created lazily on
// the first UserPromptSubmit / PreToolUse, and go idle on Stop or via
// WORKING_TIMEOUT.
pub const QODER_HOOK_EVENTS: &[&str] = &[
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "Stop",
    "Notification",
    "PermissionRequest",
];

// Tools whose calls bracket a "waiting for user input" period. Qoder has no
// Notification event, so AskUserQuestion is the alert signal there:
// PreToolUse(AskUserQuestion) → confirmation-needed; the matching PostToolUse
// (after the user answers) → working again. Trae fires real Notification
// events for the same situation; both paths set ConfirmationNeeded.
pub const ASK_USER_TOOLS: &[&str] = &["AskUserQuestion"];

// ===== Notification classification =====
// Used by StateManager::check_confirmation_needed.
pub const NOTIFICATION_COMPLETE_TYPES: &[&str] = &["task_complete", "idle", "done", "idle_prompt"];
pub const NOTIFICATION_CONFIRM_TYPES: &[&str] =
    &["permission_request", "permission_prompt", "confirmation", "input_needed"];

// Keywords that indicate a Notification is asking for user confirmation.
// Expanded to cover "run command" / "execute command" phrasing — Trae IDE
// prompts like "Trae wants to run command: Remove-Item ..." must trigger
// alert even without a tool_name field.
pub const NOTIFICATION_CONFIRM_KEYWORDS: &[&str] = &[
    "确认",
    "允许",
    "授权",
    "运行命令",
    "执行命令",
    "想要运行",
    "想要执行",
    "permission",
    "confirm",
    "allow",
    "approve",
    "run command",
    "execute command",
    "wants to run",
    "want to run",
    "wants to execute",
    "want to execute",
];

// Regex matching shell-command patterns in a Notification message — when the
// AI requests authorization to run a command, the message usually contains
// the command name (PowerShell Verb-Noun cmdlet like "Remove-Item", or an
// .exe call). Catches messages that don't contain the keywords above.
pub static NOTIFICATION_CMD_PATTERN: Lazy<Regex> =
    Lazy::new(|| Regex::new(r"(?i)\b[A-Z][a-z]+-[A-Z]\w+\b|\b\w+\.exe\b").unwrap());

// Ambiguous Notification received while working:
//   false = treat as task-complete (-> idle), avoids false alerts (default)
//   true  = treat as confirmation-needed
pub const ALERT_ON_AMBIGUOUS_NOTIFICATION: bool = false;

// ===== IDE scanner =====
// Adaptive interval: 4s when sessions exist, 15s when none.
pub const SCAN_INTERVAL_ACTIVE_MS: u64 = 4000;
pub const SCAN_INTERVAL_IDLE_MS: u64 = 15000;
// Window titles look like "<file> - <project> - Trae CN".
pub const TRAE_TITLE_SUFFIX: &str = " - Trae CN";
// Qoder window titles look like "<file> - <project> - Qoder".
pub const QODER_TITLE_SUFFIX: &str = " - Qoder";
