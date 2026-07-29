/**
 * State Manager - Tracks all Trae IDE sessions and derives overall pet state.
 *
 * Session lifecycle:
 *   SessionStart → idle
 *   UserPromptSubmit → working
 *   PreToolUse / PostToolUse → working
 *   Notification (confirmation) → confirmation-needed
 *   Stop → idle
 *
 * Overall pet state:
 *   - If any session is 'confirmation-needed' → 'alert'
 *   - Else if any session is 'working'       → 'working'
 *   - Else                                     → 'sleeping'
 */

const { EventEmitter } = require('events');

// How long (ms) without events before a working session is considered idle
const WORKING_TIMEOUT = 5 * 60 * 1000;       // 5 minutes
// How long (ms) without events before a session is removed entirely
const SESSION_TIMEOUT = 30 * 60 * 1000;       // 30 minutes
// How long (ms) before a confirmation-needed re-alerts
const ALERT_REMINDER = 60 * 1000;             // 1 minute

class StateManager extends EventEmitter {
  constructor() {
    super();
    /** @type {Map<string, SessionInfo>} */
    this.sessions = new Map();
    this.overallState = 'sleeping';
    this._timer = null;
    this._lastAlertTime = 0;
  }

  /**
   * Process an incoming hook event.
   * @param {HookEvent} event
   */
  handleHookEvent(event) {
    const { session_id, hook_event_name, project_path, project_name } = event;

    // Get or create session
    let session = this.sessions.get(session_id);
    if (!session) {
      session = {
        sessionId: session_id,
        projectPath: project_path || event.cwd || '',
        projectName: project_name || this._extractProjectName(project_path || event.cwd || ''),
        status: 'idle',
        lastEvent: hook_event_name,
        lastEventTime: Date.now(),
        alertMessage: null,
      };
      this.sessions.set(session_id, session);
    }

    // Update session info (in case it changed)
    if (project_name) session.projectName = project_name;
    if (project_path) session.projectPath = project_path;

    session.lastEvent = hook_event_name;
    session.lastEventTime = Date.now();

    // State transitions based on event
    switch (hook_event_name) {
      case 'SessionStart':
        session.status = 'idle';
        session.alertMessage = null;
        break;

      case 'UserPromptSubmit':
        session.status = 'working';
        session.alertMessage = null;
        break;

      case 'PreToolUse':
        // If transitioning from confirmation-needed, clear the alert
        if (session.status === 'confirmation-needed') {
          session.status = 'working';
          session.alertMessage = null;
        } else if (session.status !== 'confirmation-needed') {
          session.status = 'working';
        }
        break;

      case 'PostToolUse':
        // Tool completed, AI is still working (may call more tools)
        if (session.status !== 'confirmation-needed') {
          session.status = 'working';
        }
        break;

      case 'Notification':
        // Notification fires when:
        // 1. Tool execution needs user confirmation → confirmation-needed
        // 2. AI completed task → idle
        // We check if the event indicates confirmation is needed
        const needsConfirmation = this._checkConfirmationNeeded(event);
        if (needsConfirmation) {
          session.status = 'confirmation-needed';
          session.alertMessage = this._extractAlertMessage(event);
        } else {
          // Task completed notification → session goes idle
          if (session.status === 'working') {
            session.status = 'idle';
          }
        }
        break;

      case 'Stop':
        session.status = 'idle';
        session.alertMessage = null;
        break;
    }

    this._recomputeState();
  }

  /**
   * Determine if a Notification event indicates user confirmation is needed.
   * @param {HookEvent} event
   * @returns {boolean}
   */
  _checkConfirmationNeeded(event) {
    // The Notification event's stdin includes a "notification_type" or message
    // If the tool_name or notification content indicates waiting for user input
    if (event.tool_name) {
      // PreToolUse with ask permission → confirmation needed
      return true;
    }
    // Check if there's a notification message indicating confirmation
    if (event.notification_type && event.notification_type !== 'task_complete') {
      return true;
    }
    // Default: if Notification fires during a working state, treat as confirmation
    const session = this.sessions.get(event.session_id);
    if (session && session.status === 'working') {
      return true;
    }
    return false;
  }

  /**
   * Extract a human-readable alert message from the event.
   * @param {HookEvent} event
   * @returns {string}
   */
  _extractAlertMessage(event) {
    if (event.tool_name) {
      return `需要确认: ${event.tool_name}`;
    }
    if (event.message) {
      return event.message;
    }
    return '需要你的确认';
  }

  /**
   * Extract project name from a path.
   * @param {string} path
   * @returns {string}
   */
  _extractProjectName(path) {
    if (!path) return 'Unknown';
    const parts = path.replace(/\\/g, '/').split('/').filter(Boolean);
    return parts[parts.length - 1] || 'Unknown';
  }

  /**
   * Recompute the overall pet state from all sessions.
   * Emits 'state-change' if the state changed.
   */
  _recomputeState() {
    let newState = 'sleeping';

    for (const session of this.sessions.values()) {
      if (session.status === 'confirmation-needed') {
        newState = 'alert';
        break;
      }
      if (session.status === 'working') {
        newState = 'working';
      }
    }

    const oldState = this.overallState;
    this.overallState = newState;

    // Always emit update (sessions may have changed even if overall state didn't)
    this.emit('update', this.getSnapshot());

    if (oldState !== newState) {
      this.emit('state-change', { from: oldState, to: newState });
      if (newState === 'alert') {
        this._lastAlertTime = Date.now();
        this.emit('alert', this.getSnapshot());
      }
    }

    // Check if we need to re-remind about an alert
    if (newState === 'alert' && Date.now() - this._lastAlertTime > ALERT_REMINDER) {
      this._lastAlertTime = Date.now();
      this.emit('alert', this.getSnapshot());
    }
  }

  /**
   * Get a snapshot of the current state for the renderer.
   * @returns {StateSnapshot}
   */
  getSnapshot() {
    const sessions = [];
    for (const session of this.sessions.values()) {
      sessions.push({
        sessionId: session.sessionId,
        projectName: session.projectName,
        projectPath: session.projectPath,
        status: session.status,
        lastEvent: session.lastEvent,
        alertMessage: session.alertMessage,
      });
    }

    // Sort: alert first, then working, then idle
    const priority = { 'confirmation-needed': 0, 'working': 1, 'idle': 2 };
    sessions.sort((a, b) => (priority[a.status] ?? 3) - (priority[b.status] ?? 3));

    return {
      overallState: this.overallState,
      sessions,
      timestamp: Date.now(),
    };
  }

  /**
   * Clean up stale sessions (called periodically).
   */
  cleanupStaleSessions() {
    const now = Date.now();
    let changed = false;

    for (const [id, session] of this.sessions.entries()) {
      const elapsed = now - session.lastEventTime;

      // Remove sessions that haven't been heard from in a long time
      if (elapsed > SESSION_TIMEOUT) {
        this.sessions.delete(id);
        changed = true;
        continue;
      }

      // Mark working sessions as idle if they've been silent
      if (session.status === 'working' && elapsed > WORKING_TIMEOUT) {
        session.status = 'idle';
        changed = true;
      }

      // Clear confirmation-needed if it's been pending too long
      if (session.status === 'confirmation-needed' && elapsed > WORKING_TIMEOUT) {
        session.status = 'idle';
        session.alertMessage = null;
        changed = true;
      }
    }

    if (changed) {
      this._recomputeState();
    }
  }

  /**
   * Start the periodic cleanup timer.
   */
  startCleanupTimer() {
    if (this._timer) clearInterval(this._timer);
    this._timer = setInterval(() => this.cleanupStaleSessions(), 60 * 1000); // Every minute
  }

  /**
   * Stop the cleanup timer.
   */
  stop() {
    if (this._timer) {
      clearInterval(this._timer);
      this._timer = null;
    }
  }

  /**
   * Remove a session (e.g., when an IDE closes).
   * @param {string} sessionId
   */
  removeSession(sessionId) {
    if (this.sessions.delete(sessionId)) {
      this._recomputeState();
    }
  }

  /**
   * Clear all sessions.
   */
  clearAll() {
    this.sessions.clear();
    this._recomputeState();
  }
}

module.exports = { StateManager };
