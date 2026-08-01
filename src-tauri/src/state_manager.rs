//! State Manager — 1:1 port of `src/main/state-manager.js`.
//!
//! Tracks all Trae IDE sessions and derives the overall pet state.
//!
//! Session lifecycle:
//!   SessionStart → idle
//!   UserPromptSubmit → working
//!   PreToolUse / PostToolUse → working
//!   Notification (confirmation) → confirmation-needed
//!   Stop → idle
//!
//! Overall pet state:
//!   - If any session is confirmation-needed → alert
//!   - Else if any session is working        → working
//!   - Else                                   → sleeping
//!
//! Events are delivered to the frontend via tokio broadcast channels
//! (replacing Node's EventEmitter). `update` fires on any meaningful
//! snapshot change; `alert` fires on transition into alert + periodic
//! re-reminders.

use crate::config;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};
use tokio::sync::{broadcast, Mutex};

/// Current overall pet state shown to the renderer.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum PetState {
    Sleeping,
    Working,
    Alert,
}

/// Per-session status.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum SessionStatus {
    Idle,
    Working,
    ConfirmationNeeded,
}

impl SessionStatus {
    /// Sort priority for snapshots: confirmation-needed < working < idle.
    fn priority(self) -> u8 {
        match self {
            SessionStatus::ConfirmationNeeded => 0,
            SessionStatus::Working => 1,
            SessionStatus::Idle => 2,
        }
    }
}

/// Internal session record (carries timing fields not exposed in snapshots).
#[derive(Debug, Clone)]
struct SessionInfo {
    session_id: String,
    project_path: String,
    project_name: String,
    status: SessionStatus,
    last_event: String,
    last_event_time: u64,
    alert_message: Option<String>,
}

/// Session as exposed to the renderer (no timing fields).
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SessionSnapshot {
    pub session_id: String,
    pub project_name: String,
    pub project_path: String,
    pub status: SessionStatus,
    pub last_event: String,
    pub alert_message: Option<String>,
}

/// Full state snapshot delivered to the renderer.
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Snapshot {
    pub overall_state: PetState,
    pub sessions: Vec<SessionSnapshot>,
    pub last_event_at: u64,
    pub timestamp: u64,
}

/// Hook event received from the Trae IDE bridge script (POST /hook).
/// All optional fields default to empty/None via serde.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct HookEvent {
    pub session_id: String,
    pub hook_event_name: String,
    #[serde(default)]
    pub project_path: String,
    #[serde(default)]
    pub project_name: String,
    #[serde(default)]
    pub cwd: String,
    #[serde(default)]
    pub notification_type: Option<String>,
    #[serde(default)]
    pub tool_name: Option<String>,
    #[serde(default)]
    pub message: Option<String>,
    #[serde(default)]
    pub timestamp: Option<u64>,
}

const WINDOW_PREFIX: &str = "__window:";

/// Current wall-clock time in milliseconds (matches JS Date.now()).
pub fn current_millis() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

pub type Clock = Arc<dyn Fn() -> u64 + Send + Sync>;

pub struct StateManager {
    sessions: HashMap<String, SessionInfo>,
    overall_state: PetState,
    last_alert_time: u64,
    last_signature: Option<String>,
    pub last_event_at: u64,
    update_tx: broadcast::Sender<Snapshot>,
    alert_tx: broadcast::Sender<Snapshot>,
    now: Clock,
}

impl StateManager {
    pub fn new() -> Self {
        Self::with_clock(Arc::new(current_millis) as Clock)
    }

    /// Constructor with an injectable clock for deterministic tests.
    pub fn with_clock(now: Clock) -> Self {
        let (update_tx, _) = broadcast::channel(64);
        let (alert_tx, _) = broadcast::channel(64);
        Self {
            sessions: HashMap::new(),
            overall_state: PetState::Sleeping,
            last_alert_time: 0,
            last_signature: None,
            last_event_at: 0,
            update_tx,
            alert_tx,
            now,
        }
    }

    /// Subscribe to snapshot updates (fires on any meaningful change).
    pub fn subscribe_update(&self) -> broadcast::Receiver<Snapshot> {
        self.update_tx.subscribe()
    }

    /// Subscribe to alert events (fires on alert transition + re-reminders).
    pub fn subscribe_alert(&self) -> broadcast::Receiver<Snapshot> {
        self.alert_tx.subscribe()
    }

    fn now_ms(&self) -> u64 {
        (self.now)()
    }

    /// Process an incoming hook event.
    pub fn handle_hook_event(&mut self, event: &HookEvent) {
        let session_id = event.session_id.clone();
        let hook_event_name = event.hook_event_name.clone();

        // Get or create session.
        let now = self.now_ms();
        let session = self.sessions.entry(session_id.clone()).or_insert_with(|| {
            SessionInfo {
                session_id: session_id.clone(),
                project_path: if !event.project_path.is_empty() {
                    event.project_path.clone()
                } else {
                    event.cwd.clone()
                },
                project_name: if !event.project_name.is_empty() {
                    event.project_name.clone()
                } else {
                    Self::extract_project_name(&if !event.project_path.is_empty() {
                        event.project_path.clone()
                    } else {
                        event.cwd.clone()
                    })
                },
                status: SessionStatus::Idle,
                last_event: hook_event_name.clone(),
                last_event_time: now,
                alert_message: None,
            }
        });

        // Update session info (keep previous non-empty value if the field is
        // missing on a later event, so a session doesn't lose its label).
        if !event.project_name.is_empty() {
            session.project_name = event.project_name.clone();
        }
        if !event.project_path.is_empty() {
            session.project_path = event.project_path.clone();
        }
        session.last_event = hook_event_name.clone();
        session.last_event_time = now;
        self.last_event_at = now;

        // State transitions based on event.
        match hook_event_name.as_str() {
            "SessionStart" => {
                session.status = SessionStatus::Idle;
                session.alert_message = None;
            }
            "UserPromptSubmit" => {
                session.status = SessionStatus::Working;
                session.alert_message = None;
            }
            "PreToolUse" => {
                // AI is about to use a tool — always working (clears alert).
                session.status = SessionStatus::Working;
                session.alert_message = None;
            }
            "PostToolUse" => {
                // Tool completed; AI may call more tools.
                if session.status != SessionStatus::ConfirmationNeeded {
                    session.status = SessionStatus::Working;
                }
            }
            "Notification" => {
                // Notification fires when:
                // 1. Tool execution needs user confirmation → confirmation-needed
                // 2. AI completed task → idle
                let needs_confirmation = Self::check_confirmation_needed(event, session);
                if needs_confirmation {
                    session.status = SessionStatus::ConfirmationNeeded;
                    session.alert_message = Some(Self::extract_alert_message(event));
                } else {
                    // Task completed notification → clear alert and go idle.
                    session.status = SessionStatus::Idle;
                    session.alert_message = None;
                }
            }
            "Stop" => {
                session.status = SessionStatus::Idle;
                session.alert_message = None;
            }
            _ => {}
        }

        // A real hook session now covers this project; remove any
        // window-detected placeholder so the list doesn't show a duplicate.
        if !session.project_name.is_empty() {
            let wid = format!("{}{}", WINDOW_PREFIX, session.project_name);
            self.sessions.remove(&wid);
        }

        self.recompute_state();
    }

    /// Determine if a Notification event indicates user confirmation is needed.
    ///
    /// Uses session status as context: if the AI is currently confirmation-
    /// needed (user confirmed but PreToolUse hasn't arrived, or AI finished
    /// and sent a completion Notification), a new Notification is most likely
    /// a completion notice. Only treat as confirmation if the message
    /// explicitly asks for it.
    fn check_confirmation_needed(event: &HookEvent, session: &SessionInfo) -> bool {
        // 1. Explicit type classification (highest priority).
        if let Some(nt) = &event.notification_type {
            if config::NOTIFICATION_COMPLETE_TYPES.contains(&nt.as_str()) {
                return false;
            }
            if config::NOTIFICATION_CONFIRM_TYPES.contains(&nt.as_str()) {
                return true;
            }
        }

        // 2. State context.
        if session.status == SessionStatus::ConfirmationNeeded {
            if let Some(msg) = &event.message {
                let lower = msg.to_lowercase();
                if config::NOTIFICATION_CONFIRM_KEYWORDS
                    .iter()
                    .any(|kw| lower.contains(&kw.to_lowercase()))
                {
                    return true;
                }
            }
            return false; // confirmation-needed + ambiguous = completion
        }

        // 3. Non-confirmation-needed state (idle or working):
        //    tool_name suggests the tool is awaiting authorization.
        if event.tool_name.is_some() {
            return true;
        }
        if let Some(msg) = &event.message {
            let lower = msg.to_lowercase();
            if config::NOTIFICATION_CONFIRM_KEYWORDS
                .iter()
                .any(|kw| lower.contains(&kw.to_lowercase()))
            {
                return true;
            }
            // Command execution pattern: message mentions a shell command.
            if config::NOTIFICATION_CMD_PATTERN.is_match(msg) {
                return true;
            }
        }
        config::ALERT_ON_AMBIGUOUS_NOTIFICATION
    }

    /// Extract a human-readable alert message from the event.
    fn extract_alert_message(event: &HookEvent) -> String {
        if let Some(tool) = &event.tool_name {
            return format!("需要确认: {}", tool);
        }
        if let Some(msg) = &event.message {
            return msg.clone();
        }
        "需要你的确认".to_string()
    }

    /// Extract project name from a path (last path segment).
    fn extract_project_name(path: &str) -> String {
        if path.is_empty() {
            return "Unknown".to_string();
        }
        let normalized = path.replace('\\', "/");
        let parts: Vec<&str> = normalized.split('/').filter(|s| !s.is_empty()).collect();
        parts.last().map(|s| s.to_string()).unwrap_or_else(|| "Unknown".to_string())
    }

    /// Recompute the overall pet state from all sessions. Emits an `update`
    /// if the meaningful snapshot changed, and an `alert` on transition into
    /// the alert state.
    fn recompute_state(&mut self) {
        let mut new_state = PetState::Sleeping;
        for session in self.sessions.values() {
            match session.status {
                SessionStatus::ConfirmationNeeded => {
                    new_state = PetState::Alert;
                    break;
                }
                SessionStatus::Working => {
                    new_state = PetState::Working;
                }
                SessionStatus::Idle => {}
            }
        }

        let old_state = self.overall_state;
        self.overall_state = new_state;

        // Dirty check: only emit 'update' when the meaningful snapshot changed.
        let signature = self.snapshot_signature();
        let sig_changed = self.last_signature.as_deref() != Some(signature.as_str());
        if sig_changed {
            self.last_signature = Some(signature);
            let _ = self.update_tx.send(self.get_snapshot());
        }

        if old_state != new_state && new_state == PetState::Alert {
            self.last_alert_time = self.now_ms();
            let _ = self.alert_tx.send(self.get_snapshot());
        }
        // Re-reminders are handled by check_and_remind_alert() on an
        // independent timer, so they fire even without new events arriving.
    }

    /// Build a stable signature of the meaningful snapshot fields (excludes
    /// lastEventTime and timestamp) for dirty-checking.
    fn snapshot_signature(&self) -> String {
        let mut parts: Vec<String> = self
            .sessions
            .values()
            .map(|s| {
                format!(
                    "{}|{}|{}|{:?}|{}|{}",
                    s.session_id,
                    s.project_name,
                    s.project_path,
                    s.status,
                    s.last_event,
                    s.alert_message.as_deref().unwrap_or("")
                )
            })
            .collect();
        parts.sort();
        format!("{:?}::{}", self.overall_state, parts.join(";;"))
    }

    /// Get a snapshot of the current state for the renderer.
    pub fn get_snapshot(&self) -> Snapshot {
        let mut sessions: Vec<SessionSnapshot> = self
            .sessions
            .values()
            .map(|s| SessionSnapshot {
                session_id: s.session_id.clone(),
                project_name: s.project_name.clone(),
                project_path: s.project_path.clone(),
                status: s.status,
                last_event: s.last_event.clone(),
                alert_message: s.alert_message.clone(),
            })
            .collect();
        // Sort: alert first, then working, then idle.
        sessions.sort_by_key(|s| s.status.priority());

        Snapshot {
            overall_state: self.overall_state,
            sessions,
            last_event_at: self.last_event_at,
            timestamp: self.now_ms(),
        }
    }

    /// Clean up stale sessions (called periodically).
    pub fn cleanup_stale_sessions(&mut self) {
        let now = self.now_ms();
        let mut changed = false;

        let session_ids: Vec<String> = self.sessions.keys().cloned().collect();
        for id in session_ids {
            // saturating_sub: a system clock rollback (NTP sync, manual change)
            // would otherwise make this u64 subtraction underflow and panic.
            let elapsed = now.saturating_sub(self.sessions.get(&id).map(|s| s.last_event_time).unwrap_or(0));
            // Remove sessions unheard from for a long time.
            if elapsed > config::SESSION_TIMEOUT {
                self.sessions.remove(&id);
                changed = true;
                continue;
            }
            let session = self.sessions.get_mut(&id).unwrap();
            // Mark working sessions idle if silent.
            if session.status == SessionStatus::Working && elapsed > config::WORKING_TIMEOUT {
                session.status = SessionStatus::Idle;
                changed = true;
            }
            // Clear confirmation-needed if pending too long.
            if session.status == SessionStatus::ConfirmationNeeded
                && elapsed > config::WORKING_TIMEOUT
            {
                session.status = SessionStatus::Idle;
                session.alert_message = None;
                changed = true;
            }
        }

        if changed {
            self.recompute_state();
        }
    }

    /// Re-emit `alert` if still in alert state and the reminder interval has
    /// elapsed. Driven by an independent timer so reminders fire even without
    /// new events. The signature is invalidated so `update` also re-fires.
    pub fn check_and_remind_alert(&mut self) {
        if self.overall_state != PetState::Alert {
            return;
        }
        if self.now_ms().saturating_sub(self.last_alert_time) >= config::ALERT_REMINDER {
            self.last_alert_time = self.now_ms();
            self.last_signature = None;
            let snap = self.get_snapshot();
            let _ = self.alert_tx.send(snap.clone());
            let _ = self.update_tx.send(snap);
        }
    }

    /// Remove a session (e.g., when an IDE closes).
    pub fn remove_session(&mut self, session_id: &str) {
        if self.sessions.remove(session_id).is_some() {
            self.last_signature = None; // force update emission
            self.recompute_state();
        }
    }

    /// Number of sessions (used by the IDE scanner for adaptive interval).
    pub fn session_count(&self) -> usize {
        self.sessions.len()
    }

    /// Reconcile window-detected idle sessions with real hook sessions.
    /// Called periodically by the IDE window scanner. For each detected Trae
    /// window (by project name), ensure an idle session exists unless a real
    /// hook session already covers that project. Removes sessions whose IDE
    /// window closed.
    pub fn sync_detected_windows(&mut self, detected: &[String]) {
        // Snapshot hook-project names BEFORE deletion so we don't create
        // window placeholders for projects that already have a hook session.
        let hook_project_names: std::collections::HashSet<String> = self
            .sessions
            .values()
            .filter(|s| !s.session_id.starts_with(WINDOW_PREFIX))
            .map(|s| s.project_name.clone())
            .collect();

        let detected_names: std::collections::HashSet<&String> = detected.iter().collect();
        let mut changed = false;

        // Remove ALL sessions for projects whose IDE window is no longer open.
        let ids: Vec<String> = self.sessions.keys().cloned().collect();
        for id in ids {
            let pname = self.sessions.get(&id).map(|s| s.project_name.clone()).unwrap_or_default();
            if !detected_names.contains(&pname) {
                self.sessions.remove(&id);
                changed = true;
            }
        }

        // Add window-sessions for newly detected projects without a hook session.
        let now = self.now_ms();
        for name in detected.iter() {
            if hook_project_names.contains(name) {
                continue;
            }
            let wid = format!("{}{}", WINDOW_PREFIX, name);
            if let Some(existing) = self.sessions.get_mut(&wid) {
                existing.last_event_time = now; // keep alive while window stays open
            } else {
                self.sessions.insert(
                    wid,
                    SessionInfo {
                        session_id: format!("{}{}", WINDOW_PREFIX, name),
                        project_path: String::new(),
                        project_name: name.clone(),
                        status: SessionStatus::Idle,
                        last_event: "WindowDetected".to_string(),
                        last_event_time: now,
                        alert_message: None,
                    },
                );
                changed = true;
            }
        }

        if changed {
            self.last_signature = None; // force update emission
            self.recompute_state();
        }
    }
}

/// Shared state manager type used across tasks (HTTP server, timers, scanner).
pub type SharedStateManager = Arc<Mutex<StateManager>>;

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    fn event(session: &str, name: &str) -> HookEvent {
        HookEvent {
            session_id: session.to_string(),
            hook_event_name: name.to_string(),
            project_path: "/proj/test".to_string(),
            project_name: "test".to_string(),
            cwd: String::new(),
            notification_type: None,
            tool_name: None,
            message: None,
            timestamp: None,
        }
    }

    fn make_manager() -> (StateManager, Arc<AtomicU64>) {
        let tick = Arc::new(AtomicU64::new(1000));
        let tick_clone = tick.clone();
        let now: Clock = Arc::new(move || tick_clone.load(Ordering::SeqCst));
        (StateManager::with_clock(now), tick)
    }

    #[test]
    fn session_start_then_prompt_goes_working() {
        let (mut sm, _t) = make_manager();
        sm.handle_hook_event(&event("s1", "SessionStart"));
        assert_eq!(sm.overall_state, PetState::Sleeping);
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        assert_eq!(sm.overall_state, PetState::Working);
    }

    #[test]
    fn notification_with_tool_name_triggers_alert() {
        let (mut sm, _t) = make_manager();
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        let mut e = event("s1", "Notification");
        e.tool_name = Some("Remove-Item".to_string());
        sm.handle_hook_event(&e);
        assert_eq!(sm.overall_state, PetState::Alert);
    }

    #[test]
    fn notification_complete_type_clears_alert() {
        let (mut sm, _t) = make_manager();
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        let mut e = event("s1", "Notification");
        e.tool_name = Some("rm".to_string());
        sm.handle_hook_event(&e);
        assert_eq!(sm.overall_state, PetState::Alert);
        let mut e2 = event("s1", "Notification");
        e2.notification_type = Some("task_complete".to_string());
        sm.handle_hook_event(&e2);
        assert_eq!(sm.overall_state, PetState::Sleeping);
    }

    #[test]
    fn cmd_pattern_in_message_triggers_alert() {
        let (mut sm, _t) = make_manager();
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        let mut e = event("s1", "Notification");
        e.message = Some("Trae wants to run command: Remove-Item foo".to_string());
        sm.handle_hook_event(&e);
        assert_eq!(sm.overall_state, PetState::Alert);
    }

    #[test]
    fn stop_clears_to_idle() {
        let (mut sm, _t) = make_manager();
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        sm.handle_hook_event(&event("s1", "Stop"));
        assert_eq!(sm.overall_state, PetState::Sleeping);
    }

    #[test]
    fn sync_detected_windows_adds_idle_session() {
        let (mut sm, _t) = make_manager();
        sm.sync_detected_windows(&["myproject".to_string()]);
        assert_eq!(sm.session_count(), 1);
        assert_eq!(sm.overall_state, PetState::Sleeping);
        // Real hook session replaces the window placeholder.
        let mut e = event("s1", "SessionStart");
        e.project_name = "myproject".to_string();
        sm.handle_hook_event(&e);
        assert_eq!(sm.session_count(), 1);
    }

    #[test]
    fn cleanup_marks_working_idle_after_timeout() {
        let (mut sm, t) = make_manager();
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        assert_eq!(sm.overall_state, PetState::Working);
        t.store(1000 + config::WORKING_TIMEOUT + 1, Ordering::SeqCst);
        sm.cleanup_stale_sessions();
        assert_eq!(sm.overall_state, PetState::Sleeping);
    }

    #[test]
    fn alert_reminder_refires() {
        let (mut sm, t) = make_manager();
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        let mut e = event("s1", "Notification");
        e.tool_name = Some("rm".to_string());
        sm.handle_hook_event(&e);
        assert_eq!(sm.overall_state, PetState::Alert);
        t.store(1000 + config::ALERT_REMINDER + 1, Ordering::SeqCst);
        sm.check_and_remind_alert();
        // Still alert; reminder just re-emits.
        assert_eq!(sm.overall_state, PetState::Alert);
    }
}
