/**
 * Centralized configuration for the main process.
 *
 * NOTE: The renderer process is non-ESM with contextIsolation enabled, so it
 * cannot require this module. Renderer-side constants are kept inline in
 * renderer.js. Only main-process code imports this file.
 */
module.exports = {
  // ===== HTTP server =====
  PORT: 17521,
  HOST: '127.0.0.1',

  // ===== Window =====
  WINDOW_WIDTH: 260,
  WINDOW_HEIGHT: 420,
  WINDOW_MARGIN: 20, // offset from bottom-right corner

  // ===== State timeouts (ms) =====
  WORKING_TIMEOUT: 3 * 60 * 1000,   // 3 min: working -> idle if silent
  SESSION_TIMEOUT: 10 * 60 * 1000,  // 10 min: session removed if silent (fallback; window scan is primary)
  ALERT_REMINDER: 60 * 1000,        // 1 min: re-alert interval while in alert
  CLEANUP_INTERVAL_MS: 30 * 1000,   // cleanup timer tick

  // ===== Hook bridge =====
  BRIDGE_TIMEOUT_SEC: 5,
  // Events wired into ~/.trae-cn/hooks.json (keep in sync with hooks/install-hooks.ps1)
  HOOK_EVENTS: [
    'SessionStart',
    'UserPromptSubmit',
    'PreToolUse',
    'PostToolUse',
    'Stop',
    'Notification',
  ],

  // ===== Notification classification =====
  // Used by StateManager._checkConfirmationNeeded.
  // Tunable after diagnosing real Trae Notification payloads — see README.
  NOTIFICATION_COMPLETE_TYPES: ['task_complete', 'idle', 'done'],
  NOTIFICATION_CONFIRM_TYPES: ['permission_request', 'confirmation', 'input_needed'],
  // Keywords that indicate a Notification is asking for user confirmation.
  // Expanded to cover "run command" / "execute command" phrasing — Trae IDE
  // prompts like "Trae wants to run command: Remove-Item ..." must trigger
  // alert even without a tool_name field.
  NOTIFICATION_CONFIRM_KEYWORDS: [
    '确认', '允许', '授权', '运行命令', '执行命令', '想要运行', '想要执行',
    'permission', 'confirm', 'allow', 'approve',
    'run command', 'execute command', 'wants to run', 'want to run',
    'wants to execute', 'want to execute',
  ],
  // Regex matching shell-command patterns in a Notification message — when the
  // AI requests authorization to run a command, the message usually contains
  // the command name (PowerShell Verb-Noun cmdlet like "Remove-Item", or an
  // .exe call). This catches messages that don't contain the keywords above.
  NOTIFICATION_CMD_PATTERN: /\b[A-Z][a-z]+-[A-Z]\w+\b|\b\w+\.exe\b/i,
  // Ambiguous Notification received while working:
  //   false = treat as task-complete (-> idle), avoids false alerts (default)
  //   true  = treat as confirmation-needed (preserves original "alert on any" behavior)
  ALERT_ON_AMBIGUOUS_NOTIFICATION: false,
};
