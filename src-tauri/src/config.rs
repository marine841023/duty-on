//! Centralized configuration constants — 1:1 port of `src/main/config.js`.
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

// ===== State timeouts (ms) =====
pub const WORKING_TIMEOUT: u64 = 3 * 60 * 1000; // 3 min: working -> idle if silent
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

// Events wired into ~/.qoder/settings.json. Qoder supports a subset of Trae's
// events — it has no SessionStart or Notification (its task-complete signal is
// Stop), and adds PostToolUseFailure (not wired here; the state manager has no
// handler for it). Sessions are created lazily on the first UserPromptSubmit /
// PreToolUse, and go idle on Stop or via WORKING_TIMEOUT.
pub const QODER_HOOK_EVENTS: &[&str] = &["UserPromptSubmit", "PreToolUse", "PostToolUse", "Stop"];

// ===== Notification classification =====
// Used by StateManager::check_confirmation_needed.
pub const NOTIFICATION_COMPLETE_TYPES: &[&str] = &["task_complete", "idle", "done"];
pub const NOTIFICATION_CONFIRM_TYPES: &[&str] =
    &["permission_request", "confirmation", "input_needed"];

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
