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
const config = require('./config');

// Timeouts (centralized in config.js)
const { WORKING_TIMEOUT, SESSION_TIMEOUT, ALERT_REMINDER } = config;

class StateManager extends EventEmitter {
  constructor(options = {}) {
    super();
    /** @type {Map<string, SessionInfo>} */
    this.sessions = new Map();
    this.overallState = 'sleeping';
    this._timer = null;
    this._alertTimer = null;
    this._lastAlertTime = 0;
    this._lastSignature = null;
    this.lastEventAt = 0;
    // Injectable clock for testability (defaults to real time).
    this._now = options.now || (() => Date.now());
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
        lastEventTime: this._now(),
        alertMessage: null,
      };
      this.sessions.set(session_id, session);
    }

    // Update session info (in case it changed)
    if (project_name) session.projectName = project_name;
    if (project_path) session.projectPath = project_path;

    session.lastEvent = hook_event_name;
    session.lastEventTime = this._now();
    this.lastEventAt = session.lastEventTime;

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
        // AI is about to use a tool — always working (clears any pending alert).
        session.status = 'working';
        session.alertMessage = null;
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
    // Classify a Notification event as "needs user confirmation" vs "task complete".
    // Priority (tune the whitelists in config.js after inspecting real payloads):
    //   1. explicit completion type  -> false (task done)
    //   2. explicit confirmation type -> true
    //   3. tool_name on a Notification -> true (tool awaiting authorization)
    //   4. message matches confirm keyword -> true
    //   5. ambiguous -> ALERT_ON_AMBIGUOUS_NOTIFICATION (default false)
    if (event.notification_type && config.NOTIFICATION_COMPLETE_TYPES.includes(event.notification_type)) {
      return false;
    }
    if (event.notification_type && config.NOTIFICATION_CONFIRM_TYPES.includes(event.notification_type)) {
      return true;
    }
    if (event.tool_name) {
      return true;
    }
    if (event.message) {
      const lower = String(event.message).toLowerCase();
      if (config.NOTIFICATION_CONFIRM_KEYWORDS.some((kw) => lower.includes(kw.toLowerCase()))) {
        return true;
      }
    }
    return config.ALERT_ON_AMBIGUOUS_NOTIFICATION;
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

    // Dirty check: only emit 'update' when the meaningful snapshot changed.
    // lastEventTime/timestamp are excluded so repeated events don't spam updates.
    const signature = this._snapshotSignature();
    if (this._lastSignature !== signature) {
      this._lastSignature = signature;
      this.emit('update', this.getSnapshot());
    }

    if (oldState !== newState) {
      this.emit('state-change', { from: oldState, to: newState });
      if (newState === 'alert') {
        this._lastAlertTime = this._now();
        this.emit('alert', this.getSnapshot());
      }
    }
    // Re-reminders are handled by _checkAndRemindAlert() on an independent timer
    // (see startCleanupTimer), so they fire even without new events arriving.
  }

  /**
   * Build a stable signature of the meaningful snapshot fields (excludes
   * lastEventTime and timestamp) for dirty-checking in _recomputeState.
   */
  _snapshotSignature() {
    const parts = [];
    for (const s of this.sessions.values()) {
      parts.push(`${s.sessionId}|${s.projectName}|${s.projectPath}|${s.status}|${s.lastEvent}|${s.alertMessage}`);
    }
    parts.sort();
    return `${this.overallState}::${parts.join(';;')}`;
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
      lastEventAt: this.lastEventAt,
      timestamp: this._now(),
    };
  }

  /**
   * Clean up stale sessions (called periodically).
   */
  cleanupStaleSessions() {
    const now = this._now();
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
    this._timer = setInterval(() => this.cleanupStaleSessions(), config.CLEANUP_INTERVAL_MS);

    // Independent timer for alert re-reminders (does not rely on new events arriving).
    if (this._alertTimer) clearInterval(this._alertTimer);
    this._alertTimer = setInterval(() => this._checkAndRemindAlert(), config.ALERT_REMINDER);
  }

  /**
   * Re-emit 'alert' if still in alert state and the reminder interval has elapsed.
   * Driven by an independent timer so reminders fire even without new events.
   */
  _checkAndRemindAlert() {
    if (this.overallState !== 'alert') return;
    if (this._now() - this._lastAlertTime >= ALERT_REMINDER) {
      this._lastAlertTime = this._now();
      this.emit('alert', this.getSnapshot());
    }
  }

  /**
   * Stop the cleanup timer.
   */
  stop() {
    if (this._timer) {
      clearInterval(this._timer);
      this._timer = null;
    }
    if (this._alertTimer) {
      clearInterval(this._alertTimer);
      this._alertTimer = null;
    }
  }

  /**
   * Remove a session (e.g., when an IDE closes).
   * @param {string} sessionId
   */
  removeSession(sessionId) {
    if (this.sessions.delete(sessionId)) {
      this._lastSignature = null; // force update emission
      this._recomputeState();
    }
  }

  /**
   * Clear all sessions.
   */
  clearAll() {
    this.sessions.clear();
    this._lastSignature = null; // force update emission
    this._recomputeState();
  }
}

module.exports = { StateManager };
