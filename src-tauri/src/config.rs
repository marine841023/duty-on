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
pub const WINDOW_HEIGHT: i32 = 520;
pub const WINDOW_MARGIN: i32 = 20; // offset from bottom-right corner
// Mini mode: half-width window with full-height project list.
// Canvas is half-size (120×130); the extra height (280−130=150px) gives
// the status bar enough room to show ~6 project items without scrolling.
pub const MINI_WINDOW_WIDTH: i32 = 130;
pub const MINI_WINDOW_HEIGHT: i32 = 280;
// Character canvas sits flush at the top of the window (centered
// horizontally), so the pet's visible center is at (window_width/2,
// canvas_height/2) — used to place the character under the cursor when
// leaving edge-dock. Must match PIXI_HEIGHT / MINI_PIXI_HEIGHT in renderer.js.
pub const PET_CANVAS_HEIGHT: i32 = 260;
pub const MINI_PET_CANVAS_HEIGHT: i32 = 130;
// Edge dock: compact "traffic-light" bar snapped to the left/right screen
// edge (拖到屏幕边缘自动吸附). THICKNESS is the bar's narrow dimension
// (logical px); the bar height follows its content (see enter_edge_dock).
pub const EDGE_DOCK_THICKNESS: i32 = 40;
// Undock offset (logical px): when leaving edge-dock mode the window is
// pulled this far away from the edge. Snap detection itself is purely
// "crossed the edge by >20% of the window width" (see detect_edge_dock).
pub const EDGE_SNAP_THRESHOLD: i32 = 60;

// ===== State timeouts (ms) =====
/// Working sessions drop to idle after this much silence. Must stay generous:
/// a working session can legitimately go silent for minutes (long tool runs,
/// or Qoder's ask-user dialog which fires no hook event at all).
pub const WORKING_TIMEOUT: u64 = 3 * 60 * 1000;
/// Thinking sessions (LLM generation phase) drop to idle after this much
/// silence instead. Between UserPromptSubmit/PostToolUse and the next event
/// the agent emits NO hook event — the silence equals the reply generation
/// time, measured at 3m52s in the wild (a 3-min WORKING_TIMEOUT demoted a
/// still-generating session to idle; see docs/fault-records.md 故障 4).
pub const THINKING_TIMEOUT: u64 = 10 * 60 * 1000;
/// Sessions with an in-flight tool (PreToolUse seen, matching PostToolUse not
/// yet) stay working for this much silence instead. Between PreToolUse and
/// PostToolUse the agent emits NO hook event at all — long builds, long tests
/// and sub-agent tasks routinely run 5-15 minutes; WORKING_TIMEOUT would
/// wrongly show the pet sleeping mid-run (see docs/fault-records.md 故障 1).
pub const TOOL_RUNNING_TIMEOUT: u64 = 15 * 60 * 1000;
pub const SESSION_TIMEOUT: u64 = 10 * 60 * 1000; // 10 min: session removed if silent
pub const ALERT_REMINDER: u64 = 60 * 1000; // 1 min: re-alert interval while in alert
pub const CLEANUP_INTERVAL_MS: u64 = 30 * 1000; // cleanup timer tick

// ===== System monitor =====
/// Metrics sampling interval for the monitor drawer (CPU/RAM/GPU/NET/self).
/// 1.5s: well above sysinfo's MINIMUM_CPU_UPDATE_INTERVAL (200ms) so CPU
/// percentages are meaningful, and cheap enough to keep the total sampling
/// cost under ~0.5% CPU.
pub const MONITOR_INTERVAL_MS: u64 = 1500;

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

// Events wired into ~/.cursor/hooks.json. Cursor uses its own camelCase
// event names and a flatter schema (event -> [{command, timeout}]); the
// bridge normalizes them to the canonical names the state machine knows:
//   sessionStart       -> SessionStart
//   beforeSubmitPrompt -> UserPromptSubmit
//   preToolUse         -> PreToolUse   \  the AskUserQuestion tool pair is
//   postToolUse        -> PostToolUse  /  the alert signal (Qoder's trick —
//                                        Cursor has no notification event)
//   stop               -> Stop
// Cursor's stdin payload carries hook_event_name in camelCase (which the
// state machine does not match), so the installed command bakes the event
// into the `-HookEvent` argument and the bridge overrides with the canonical
// PascalCase name (see hooks_installer::hook_command).
pub const CURSOR_HOOK_EVENTS: &[&str] = &[
    "sessionStart",
    "beforeSubmitPrompt",
    "preToolUse",
    "postToolUse",
    "stop",
];

// Events wired into ~/.codex/hooks.json. Codex CLI uses the same PascalCase
// event names and nested JSON schema as Trae/Claude Code, so no event-name
// normalization is needed (unlike Cursor). We wire the 6 events the pet state
// machine cares about; SubagentStart/Stop and Pre/PostCompact are omitted
// (not relevant for pet state). PermissionRequest is the alert signal (like
// Qoder). Note: Codex requires hooks to be trusted via `/hooks` in the CLI
// after installation — see install result hint.
pub const CODEX_HOOK_EVENTS: &[&str] = &[
    "SessionStart",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "Stop",
    "PermissionRequest",
];

// OpenCode (opencode.ai) integration. Unlike Trae/Cursor/Codex, OpenCode has
// NO config-file shell hook — its extension points are JS/TS plugins auto-
// loaded from `~/.config/opencode/plugins/` at startup. So the installer does
// NOT merge hook entries; instead it writes a self-contained plugin file
// (`dutyon-bridge.js`) that subscribes to OpenCode's event bus and POSTs the
// canonical hook events below to this app's HTTP server. The list is purely
// documentary (the plugin emits these names); it is not consumed by any merge.
// Event mapping (OpenCode -> canonical):
//   session.created            -> SessionStart
//   message.part.updated(text,user) -> UserPromptSubmit
//   message.part.updated(tool,running)   -> PreToolUse
//   message.part.updated(tool,completed) -> PostToolUse
//   session.status(idle)/session.deleted -> Stop
//   permission.asked/question.asked      -> PermissionRequest
pub const OPENCODE_HOOK_EVENTS: &[&str] = &[
    "SessionStart",
    "UserPromptSubmit",
    "PreToolUse",
    "PostToolUse",
    "Stop",
    "PermissionRequest",
];
/// Global plugin dir where OpenCode auto-loads JS/TS plugins at startup.
/// opencode uses `~/.config/opencode/` consistently across platforms, so this
/// is relative to the user's home directory.
pub const OPENCODE_PLUGIN_SUBDIR: &str = ".config/opencode/plugins";
/// Filename of the generated bridge plugin inside `OPENCODE_PLUGIN_SUBDIR`.
pub const OPENCODE_PLUGIN_FILENAME: &str = "dutyon-bridge.js";

// Tools whose calls bracket a "waiting for user input" period. Qoder has no
// Notification event, so AskUserQuestion is the alert signal there:
// PreToolUse(AskUserQuestion) → confirmation-needed; the matching PostToolUse
// (after the user answers) → working again. Trae fires real Notification
// events for the same situation; both paths set ConfirmationNeeded.
//
// The AskQuestion variants are pre-wired for Cursor: its AskQuestion tool
// currently fires NO hook events at all (confirmed Cursor bug, forum thread
// "AskQuestion tool skips preToolUse and postToolUse hooks"), so these never
// match today — but once Cursor fixes it, the alert lights up automatically.
pub const ASK_USER_TOOLS: &[&str] = &[
    "AskUserQuestion",
    "AskQuestion",
    "ask_question",
    "askQuestion",
];

// Tools that always require an explicit approval click in the IDE (the
// "运行/Run" button on computer-use cards) before they execute. Claude-style
// hooks fire PreToolUse when the approval card APPEARS — i.e. while the agent
// is blocked waiting for the user — so a PreToolUse naming one of these tools
// means confirmation-needed, not tool-use. The matching PostToolUse (after
// the user clicked Run and the tool finished) returns the session to Thinking.
// Exact-match (case variants enumerated) to avoid false positives on benign
// tools whose names merely CONTAIN these words.
pub const CONFIRM_TOOLS: &[&str] = &[
    // computer use family
    "computer_use",
    "computer-use",
    "computerUse",
    "ComputerUse",
    "COMPUTER_USE",
    "use_computer",
    "useComputer",
    "UseComputer",
    "computer",
    "Computer",
    // browser/desktop automation family
    "browser_use",
    "browser-use",
    "browserUse",
    "BrowserUse",
    "browser",
    "Browser",
];

// ===== Notification classification =====
// Used by StateManager::check_confirmation_needed.
pub const NOTIFICATION_COMPLETE_TYPES: &[&str] = &["task_complete", "idle", "done", "idle_prompt"];
// Trae's official notification_type has 5 values (docs.trae.cn); the last
// three are UI-flow waits that were previously unclassified and fell into the
// ambiguous branch (= task complete -> pet sleeps while a review dialog is
// open). All five are now classified (docs/fault-records.md 故障 2).
pub const NOTIFICATION_CONFIRM_TYPES: &[&str] = &[
    "permission_request",
    "permission_prompt",
    "confirmation",
    "input_needed",
    "document_review",
    "ask_user_question",
    "browser_interaction",
];
// Subset of NOTIFICATION_CONFIRM_TYPES that by definition brackets an
// in-flight tool (payload carries the matching tool_use_id). Notifications
// are ASYNC per Trae docs: these can arrive AFTER the tool's PostToolUse —
// a late one with no pending tool is stale and must not re-trigger a
// permanent alert (docs/fault-records.md 故障 3). The UI-flow types
// (document_review / ask_user_question / browser_interaction) are NOT
// tool-bound and skip this guard.
pub const NOTIFICATION_TOOL_BOUND_CONFIRM_TYPES: &[&str] = &[
    "permission_request",
    "permission_prompt",
    "confirmation",
    "input_needed",
];

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
// Newer Trae builds rebranded the suffix to "TraeCode CN" — both are matched
// so users on either version are detected. See ide_scanner::parse_title.
pub const TRAE_TITLE_SUFFIX: &str = " - Trae CN";
pub const TRAE_CODE_TITLE_SUFFIX: &str = " - TraeCode CN";
// Qoder window titles look like "<file> - <project> - Qoder".
pub const QODER_TITLE_SUFFIX: &str = " - Qoder";
// Cursor (VS Code fork) window titles look like "<file> - <project> - Cursor".
pub const CURSOR_TITLE_SUFFIX: &str = " - Cursor";
// Cursor 3.x also has a standalone Agents panel window whose editor main
// window may carry an empty title; the panel itself is the visible surface,
// titled "Cursor Agents" (no workspace) or "<folder> - Cursor Agents".
pub const CURSOR_AGENTS_TITLE: &str = "Cursor Agents";
pub const CURSOR_AGENTS_TITLE_SUFFIX: &str = " - Cursor Agents";
