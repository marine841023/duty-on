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
  WINDOW_WIDTH: 320,
  WINDOW_HEIGHT: 520,
  WINDOW_MARGIN: 20, // offset from bottom-right corner

  // ===== State timeouts (ms) =====
  WORKING_TIMEOUT: 5 * 60 * 1000,   // 5 min: working -> idle if silent
  SESSION_TIMEOUT: 30 * 60 * 1000,  // 30 min: session removed if silent
  ALERT_REMINDER: 60 * 1000,        // 1 min: re-alert interval while in alert
  CLEANUP_INTERVAL_MS: 60 * 1000,   // cleanup timer tick

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
  NOTIFICATION_CONFIRM_KEYWORDS: ['确认', '允许', '授权', 'permission', 'confirm', 'allow', 'approve'],
  // Ambiguous Notification received while working:
  //   false = treat as task-complete (-> idle), avoids false alerts (default)
  //   true  = treat as confirmation-needed (preserves original "alert on any" behavior)
  ALERT_ON_AMBIGUOUS_NOTIFICATION: false,
};
