//! State manager for tracking AI session states across multiple IDE instances.
//!
//! Maintains a map of session_id → SessionInfo, processes hook events from
//! the HTTP server, and computes the overall pet state (sleeping/working/alert).
//!
//! Session lifecycle:
//!   SessionStart → idle
//!   UserPromptSubmit → working
//!   PreToolUse / PostToolUse → working
//!   PreToolUse(AskUserQuestion) → confirmation-needed; its PostToolUse → working
//!     (Qoder has no Notification event; the ask-user tool pair is its alert signal)
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
use crate::ide_scanner::DetectedProject;
use crate::models::IdeKind;
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
    /// Which IDE owns this session; reported by the bridge or backfilled
    /// from the window scan. None when unknown.
    ide: Option<IdeKind>,
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
    pub ide: Option<IdeKind>,
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
    /// IDE source reported by the bridge; absent on events from older
    /// bridge scripts (deserialized as lowercase "trae"/"qoder").
    #[serde(default)]
    pub ide: Option<IdeKind>,
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
    /// Consecutive window-scan misses per real hook session (grace counter).
    window_miss_counts: HashMap<String, u32>,
}

/// Consecutive window-scan misses a REAL hook session tolerates before sync
/// removes it. Window titles are version/locale dependent and an IDE window
/// can briefly vanish during startup (e.g. Qoder recreates its main window
/// when startup completes), so a single miss must not kill a live hook
/// session. `__window:` placeholders are still removed on the first miss.
const WINDOW_MISS_GRACE: u32 = 3;

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
            window_miss_counts: HashMap::new(),
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
                project_path: Self::choose_project_path(event).to_string(),
                project_name: Self::choose_project_name(event),
                ide: event.ide.clone(),
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
        if event.ide.is_some() {
            session.ide = event.ide.clone();
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
                if Self::is_ask_user_tool(event) {
                    // The agent is now blocked waiting for the user's answer
                    // (Qoder has no Notification event for this).
                    session.status = SessionStatus::ConfirmationNeeded;
                    session.alert_message = Some(Self::extract_alert_message(event));
                } else {
                    // AI is about to use a tool — always working (clears alert).
                    session.status = SessionStatus::Working;
                    session.alert_message = None;
                }
            }
            "PostToolUse" => {
                if Self::is_ask_user_tool(event) {
                    // The user answered; the agent resumed.
                    session.status = SessionStatus::Working;
                    session.alert_message = None;
                } else if session.status != SessionStatus::ConfirmationNeeded {
                    // Tool completed; AI may call more tools.
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
            "PermissionRequest" => {
                // Qoder CLI-documented event (not listed among the IDE's
                // documented events, but wired in case the IDE honors it):
                // the agent is asking for tool permission — user input needed.
                session.status = SessionStatus::ConfirmationNeeded;
                session.alert_message = Some(Self::extract_alert_message(event));
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

    /// Choose the project path from a hook event, preferring project_path over cwd.
    fn choose_project_path(event: &HookEvent) -> &str {
        if !event.project_path.is_empty() {
            &event.project_path
        } else {
            &event.cwd
        }
    }

    /// Choose the project name from a hook event, preferring project_name
    /// over the last path segment of the chosen project path.
    fn choose_project_name(event: &HookEvent) -> String {
        if !event.project_name.is_empty() {
            event.project_name.clone()
        } else {
            Self::extract_project_name(Self::choose_project_path(event))
        }
    }

    /// True when the event's tool pauses the agent to collect user input
    /// (e.g. Qoder's AskUserQuestion). Qoder has no Notification event, so
    /// these tool calls are the alert signal there.
    fn is_ask_user_tool(event: &HookEvent) -> bool {
        event
            .tool_name
            .as_deref()
            .map(|t| config::ASK_USER_TOOLS.contains(&t))
            .unwrap_or(false)
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
                    "{}|{}|{}|{}|{:?}|{}|{}",
                    s.session_id,
                    s.project_name,
                    s.project_path,
                    s.ide.as_ref().map(|i| i.as_str()).unwrap_or(""),
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
                ide: s.ide.clone(),
                status: s.status,
                last_event: s.last_event.clone(),
                alert_message: s.alert_message.clone(),
            })
            .collect();
        // Collapse same-project spawns (e.g. Qoder expert/sub-agent
        // flows that share project_name+ide but use distinct session_ids)
        // into a single visible entry. Only the snapshot is affected;
        // internal per-session_id tracking, stale cleanup, and window sync
        // remain unchanged.
        sessions = deduplicate_sessions(sessions);
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
            // Sessions waiting for user confirmation (alert) are exempt from
            // ALL timeouts: no hook event fires while the ask-user dialog is
            // open, so silence here is expected — the pet must keep reminding
            // until the user answers (a resolving hook event clears it). On
            // Windows, sync_detected_windows still removes the session when
            // the IDE window is actually closed.
            if self
                .sessions
                .get(&id)
                .map(|s| s.status == SessionStatus::ConfirmationNeeded)
                .unwrap_or(false)
            {
                continue;
            }
            // Remove sessions unheard from for a long time.
            if elapsed > config::SESSION_TIMEOUT {
                self.sessions.remove(&id);
                changed = true;
                continue;
            }
            let session = self.sessions.get_mut(&id).unwrap();
            // Mark working sessions idle only after a long silence — long
            // tool runs and Qoder's ask-user dialog (no hook event) both keep
            // a session silent for minutes while still genuinely active.
            if session.status == SessionStatus::Working && elapsed > config::WORKING_TIMEOUT {
                session.status = SessionStatus::Idle;
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
    pub fn sync_detected_windows(&mut self, detected: &[DetectedProject]) {
        // Snapshot hook-project names BEFORE deletion so we don't create
        // window placeholders for projects that already have a hook session.
        let hook_project_names: std::collections::HashSet<String> = self
            .sessions
            .values()
            .filter(|s| !s.session_id.starts_with(WINDOW_PREFIX))
            .map(|s| s.project_name.clone())
            .collect();

        let detected_names: std::collections::HashSet<&str> =
            detected.iter().map(|d| d.name.as_str()).collect();
        let mut changed = false;

        // On Windows, EnumWindows is reliable — an empty list means "no IDE
        // windows open", so we proceed with removal. On other platforms
        // (Wayland without X11, macOS without screen-recording permission),
        // an empty list may mean the scan failed, so we skip removal to
        // avoid wiping real hook sessions. Stale sessions are cleaned up by
        // cleanup_stale_sessions via SESSION_TIMEOUT.
        if !detected.is_empty() || cfg!(target_os = "windows") {
            let ids: Vec<String> = self.sessions.keys().cloned().collect();
            for id in ids {
                let pname = self.sessions.get(&id).map(|s| s.project_name.clone()).unwrap_or_default();
                if detected_names.contains(pname.as_str()) {
                    // Window visible again — reset the grace counter.
                    self.window_miss_counts.remove(&id);
                    continue;
                }
                if id.starts_with(WINDOW_PREFIX) {
                    // Placeholder sessions track the window 1:1 — remove at once.
                    self.sessions.remove(&id);
                    changed = true;
                } else {
                    // Real hook sessions get a grace period: the IDE window may
                    // be briefly invisible (startup/reload) or its title format
                    // may not match our parser. Only remove after repeated
                    // misses; SESSION_TIMEOUT is the last-resort cleanup.
                    let misses = self.window_miss_counts.entry(id.clone()).or_insert(0);
                    *misses += 1;
                    if *misses >= WINDOW_MISS_GRACE {
                        self.sessions.remove(&id);
                        self.window_miss_counts.remove(&id);
                        changed = true;
                    }
                }
            }
        }

        // Add window-sessions for newly detected projects without a hook session.
        let now = self.now_ms();
        for dp in detected.iter() {
            if hook_project_names.contains(&dp.name) {
                continue;
            }
            let wid = format!("{}{}", WINDOW_PREFIX, dp.name);
            if let Some(existing) = self.sessions.get_mut(&wid) {
                existing.last_event_time = now; // keep alive while window stays open
                // The project may have been reopened in the other IDE.
                if existing.ide.as_ref().map(|i| i.as_str()) != Some(dp.ide.as_str()) {
                    existing.ide = Some(dp.ide.clone());
                    changed = true;
                }
            } else {
                self.sessions.insert(
                    wid,
                    SessionInfo {
                        session_id: format!("{}{}", WINDOW_PREFIX, dp.name),
                        project_path: String::new(),
                        project_name: dp.name.clone(),
                        ide: Some(dp.ide.clone()),
                        status: SessionStatus::Idle,
                        last_event: "WindowDetected".to_string(),
                        last_event_time: now,
                        alert_message: None,
                    },
                );
                changed = true;
            }
        }

        // Backfill the IDE kind for hook sessions whose bridge didn't report
        // it (older bridge scripts): the window scan is the source of truth.
        for s in self.sessions.values_mut() {
            if s.session_id.starts_with(WINDOW_PREFIX) || s.ide.is_some() {
                continue;
            }
            if let Some(dp) = detected.iter().find(|d| d.name == s.project_name) {
                s.ide = Some(dp.ide.clone());
                changed = true;
            }
        }

        if changed {
            self.last_signature = None; // force update emission
            self.recompute_state();
        }
    }
}

/// Deduplication priority for a session status: higher = more important to
/// keep when collapsing same-project spawns. Mirrors the overall pet-state
/// tiers — `ConfirmationNeeded` maps to alert, `Working` to working, and
/// `Idle` to the idle/sleeping tier.
fn dedup_priority(status: SessionStatus) -> u8 {
    match status {
        SessionStatus::ConfirmationNeeded => 3,
        SessionStatus::Working => 2,
        SessionStatus::Idle => 1,
    }
}

/// True for window-scan placeholder sessions (not backed by a real hook event).
fn is_window_placeholder(s: &SessionSnapshot) -> bool {
    s.session_id.starts_with(WINDOW_PREFIX)
}

/// Deduplicate sessions by `(project_name, ide)`, keeping the one with the
/// highest-priority status for each group. This collapses multi-process
/// spawns from the same IDE+project — e.g. Qoder's expert/sub-agent flows,
/// which share `project_name` and `ide` but use distinct `session_id`s —
/// into a single visible status-bar entry.
///
/// Only the rendered snapshot is collapsed; internal per-`session_id`
/// tracking, `cleanup_stale_sessions`, and `sync_detected_windows` keep
/// operating on the full set, so timeouts and window matching are unaffected.
///
/// Tie-break: when two sessions share the top status, a real hook session
/// is preferred over a `__window:` placeholder so a detected-window stub
/// merges into its live counterpart.
///
/// `IdeKind` does not derive `Hash`, so the key uses its stable `&'static
/// str` form (`IdeKind::as_str`) rather than the enum itself.
fn deduplicate_sessions(sessions: Vec<SessionSnapshot>) -> Vec<SessionSnapshot> {
    let mut groups: HashMap<(String, Option<&'static str>), SessionSnapshot> = HashMap::new();
    for s in sessions {
        let ide_str = s.ide.as_ref().map(|i| i.as_str());
        let key = (s.project_name.clone(), ide_str);
        match groups.get_mut(&key) {
            Some(existing) => {
                let replace = dedup_priority(s.status) > dedup_priority(existing.status)
                    || (dedup_priority(s.status) == dedup_priority(existing.status)
                        && is_window_placeholder(existing)
                        && !is_window_placeholder(&s));
                if replace {
                    *existing = s;
                }
            }
            None => {
                groups.insert(key, s);
            }
        }
    }
    groups.into_values().collect()
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
            ide: None,
            message: None,
            timestamp: None,
        }
    }

    fn detected(name: &str, ide: IdeKind) -> DetectedProject {
        DetectedProject {
            name: name.to_string(),
            ide,
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
    fn pre_tool_use_ask_user_question_triggers_alert() {
        let (mut sm, _t) = make_manager();
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        let mut e = event("s1", "PreToolUse");
        e.tool_name = Some("AskUserQuestion".to_string());
        sm.handle_hook_event(&e);
        assert_eq!(sm.overall_state, PetState::Alert);
    }

    #[test]
    fn post_tool_use_ask_user_question_clears_alert() {
        let (mut sm, _t) = make_manager();
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        let mut e = event("s1", "PreToolUse");
        e.tool_name = Some("AskUserQuestion".to_string());
        sm.handle_hook_event(&e);
        assert_eq!(sm.overall_state, PetState::Alert);
        let mut e2 = event("s1", "PostToolUse");
        e2.tool_name = Some("AskUserQuestion".to_string());
        sm.handle_hook_event(&e2);
        assert_eq!(sm.overall_state, PetState::Working);
    }

    #[test]
    fn pre_tool_use_regular_tool_stays_working() {
        let (mut sm, _t) = make_manager();
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        let mut e = event("s1", "PreToolUse");
        e.tool_name = Some("Bash".to_string());
        sm.handle_hook_event(&e);
        assert_eq!(sm.overall_state, PetState::Working);
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
    fn notification_permission_prompt_type_triggers_alert() {
        let (mut sm, _t) = make_manager();
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        let mut e = event("s1", "Notification");
        e.notification_type = Some("permission_prompt".to_string());
        e.message = Some("Agent is requesting permission".to_string());
        sm.handle_hook_event(&e);
        assert_eq!(sm.overall_state, PetState::Alert);
    }

    #[test]
    fn notification_idle_prompt_goes_idle() {
        let (mut sm, _t) = make_manager();
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        let mut e = event("s1", "Notification");
        e.notification_type = Some("idle_prompt".to_string());
        sm.handle_hook_event(&e);
        assert_eq!(sm.overall_state, PetState::Sleeping);
    }

    #[test]
    fn permission_request_event_triggers_alert() {
        let (mut sm, _t) = make_manager();
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        let mut e = event("s1", "PermissionRequest");
        e.tool_name = Some("Bash".to_string());
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
        sm.sync_detected_windows(&[detected("myproject", IdeKind::Trae)]);
        assert_eq!(sm.session_count(), 1);
        assert_eq!(sm.overall_state, PetState::Sleeping);
        // Window placeholder carries the IDE kind into the snapshot.
        let snap = sm.get_snapshot();
        assert_eq!(snap.sessions[0].ide.as_ref().map(|i| i.as_str()), Some("trae"));
        // Real hook session replaces the window placeholder.
        let mut e = event("s1", "SessionStart");
        e.project_name = "myproject".to_string();
        sm.handle_hook_event(&e);
        assert_eq!(sm.session_count(), 1);
    }

    #[cfg(not(target_os = "windows"))]
    #[test]
    fn sync_detected_windows_empty_does_not_wipe_sessions() {
        let (mut sm, _t) = make_manager();
        // Establish a real working session via a hook event.
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        assert_eq!(sm.overall_state, PetState::Working);
        assert_eq!(sm.session_count(), 1);
        // An empty detected list (scan failure) must not wipe real sessions.
        sm.sync_detected_windows(&[]);
        assert_eq!(sm.session_count(), 1);
        assert_eq!(sm.overall_state, PetState::Working);
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn sync_detected_windows_empty_removes_sessions_on_windows() {
        // On Windows, EnumWindows is reliable, so a persistently empty
        // detected list means "no IDE windows open" and sessions are removed
        // — but only after the grace period, so a window that briefly
        // vanishes (IDE startup/reload) doesn't kill a live hook session.
        let (mut sm, _t) = make_manager();
        sm.handle_hook_event(&event("test-session", "UserPromptSubmit"));
        assert_eq!(sm.session_count(), 1);

        // First misses keep the session alive (grace).
        sm.sync_detected_windows(&[]);
        sm.sync_detected_windows(&[]);
        assert_eq!(sm.session_count(), 1);
        // A matching window resets the counter...
        sm.sync_detected_windows(&[detected("test", IdeKind::Qoder)]);
        sm.sync_detected_windows(&[]);
        sm.sync_detected_windows(&[]);
        assert_eq!(sm.session_count(), 1);
        // ...and only WINDOW_MISS_GRACE consecutive misses remove it.
        sm.sync_detected_windows(&[]);
        assert_eq!(sm.session_count(), 0);
    }

    #[cfg(target_os = "windows")]
    #[test]
    fn window_placeholder_removed_immediately_but_hook_session_survives() {
        // Placeholder (window-detected) sessions follow the window 1:1, while
        // real hook sessions survive transient title mismatches.
        let (mut sm, _t) = make_manager();
        sm.sync_detected_windows(&[detected("projA", IdeKind::Qoder)]);
        assert_eq!(sm.session_count(), 1);
        let mut e = event("s1", "UserPromptSubmit");
        e.project_name = "projB".to_string();
        sm.handle_hook_event(&e);
        assert_eq!(sm.session_count(), 2);

        // One scan sees neither window: placeholder gone, hook session kept.
        sm.sync_detected_windows(&[]);
        assert_eq!(sm.session_count(), 1);
        assert_eq!(sm.get_snapshot().sessions[0].project_name, "projB");
    }

    #[test]
    fn hook_event_ide_is_stored_in_snapshot() {
        let (mut sm, _t) = make_manager();
        let mut e = event("s1", "UserPromptSubmit");
        e.ide = Some(IdeKind::Qoder);
        sm.handle_hook_event(&e);
        let snap = sm.get_snapshot();
        assert_eq!(snap.sessions[0].ide.as_ref().map(|i| i.as_str()), Some("qoder"));
    }

    #[test]
    fn sync_detected_windows_backfills_ide_for_hook_session() {
        let (mut sm, _t) = make_manager();
        // Hook event without an ide field (older bridge script).
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        assert!(sm.get_snapshot().sessions[0].ide.is_none());
        // The window scan sees the same project in Qoder → backfill.
        sm.sync_detected_windows(&[detected("test", IdeKind::Qoder)]);
        assert_eq!(sm.get_snapshot().sessions[0].ide.as_ref().map(|i| i.as_str()), Some("qoder"));
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

    /// An unanswered confirmation alert must survive every timeout: no hook
    /// event fires while the ask-user dialog is open, and the user may take
    /// arbitrarily long to answer. The alert only ends via a resolving hook
    /// event (or the IDE window closing on Windows).
    #[test]
    fn alert_persists_past_timeouts_until_resolved() {
        let (mut sm, t) = make_manager();
        sm.handle_hook_event(&event("s1", "UserPromptSubmit"));
        let mut e = event("s1", "Notification");
        e.notification_type = Some("permission_prompt".to_string());
        e.message = Some("Agent is requesting permission".to_string());
        sm.handle_hook_event(&e);
        assert_eq!(sm.overall_state, PetState::Alert);

        // Way past both WORKING_TIMEOUT and SESSION_TIMEOUT — still alerting.
        t.store(1000 + config::SESSION_TIMEOUT * 2, Ordering::SeqCst);
        sm.cleanup_stale_sessions();
        assert_eq!(sm.overall_state, PetState::Alert);
        assert_eq!(sm.session_count(), 1);

        // Reminders keep firing while the alert stands.
        sm.check_and_remind_alert();
        assert_eq!(sm.overall_state, PetState::Alert);

        // The user answers (Stop resolves the session) → back to sleep tier.
        t.store(1000 + config::SESSION_TIMEOUT * 2 + 1000, Ordering::SeqCst);
        sm.handle_hook_event(&event("s1", "Stop"));
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

    /// Build a `SessionSnapshot` with sensible defaults for dedup tests.
    fn snap(
        id: &str,
        project: &str,
        ide: IdeKind,
        status: SessionStatus,
        last_event: &str,
    ) -> SessionSnapshot {
        SessionSnapshot {
            session_id: id.to_string(),
            project_name: project.to_string(),
            project_path: format!("/p/{}", project),
            ide: Some(ide),
            status,
            last_event: last_event.to_string(),
            alert_message: None,
        }
    }

    /// Build a `__window:` placeholder snapshot (mimics the IDE scanner stub).
    fn window_snap(project: &str, ide: IdeKind) -> SessionSnapshot {
        SessionSnapshot {
            session_id: format!("{}{}", WINDOW_PREFIX, project),
            project_name: project.to_string(),
            project_path: String::new(),
            ide: Some(ide),
            status: SessionStatus::Idle,
            last_event: "WindowDetected".to_string(),
            alert_message: None,
        }
    }

    #[test]
    fn deduplicate_sessions_merges_same_project_different_ids() {
        // Qoder expert/sub-agent flow: two processes, same project+ide,
        // distinct session_ids. One working, one idle → keep the working one.
        let s1 = snap("s1", "myproj", IdeKind::Qoder, SessionStatus::Working, "UserPromptSubmit");
        let s2 = snap("s2", "myproj", IdeKind::Qoder, SessionStatus::Idle, "SessionStart");
        let out = deduplicate_sessions(vec![s1, s2]);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].session_id, "s1");
        assert_eq!(out[0].status, SessionStatus::Working);
    }

    #[test]
    fn deduplicate_sessions_keeps_alert_over_working() {
        let alert = snap("s1", "myproj", IdeKind::Qoder, SessionStatus::ConfirmationNeeded, "PreToolUse");
        let working = snap("s2", "myproj", IdeKind::Qoder, SessionStatus::Working, "UserPromptSubmit");
        // Working arrives first; alert must still win.
        let out = deduplicate_sessions(vec![working, alert]);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].status, SessionStatus::ConfirmationNeeded);
        assert_eq!(out[0].session_id, "s1");
    }

    #[test]
    fn deduplicate_sessions_preserves_different_projects() {
        let a = snap("s1", "projA", IdeKind::Qoder, SessionStatus::Working, "UserPromptSubmit");
        let b = snap("s2", "projB", IdeKind::Qoder, SessionStatus::Idle, "SessionStart");
        let out = deduplicate_sessions(vec![a, b]);
        assert_eq!(out.len(), 2);
    }

    #[test]
    fn deduplicate_sessions_separates_same_project_different_ide() {
        let trae = snap("s1", "myproj", IdeKind::Trae, SessionStatus::Working, "UserPromptSubmit");
        let qoder = snap("s2", "myproj", IdeKind::Qoder, SessionStatus::Idle, "SessionStart");
        let out = deduplicate_sessions(vec![trae, qoder]);
        assert_eq!(out.len(), 2);
    }

    #[test]
    fn deduplicate_sessions_prefers_real_over_placeholder_on_tie() {
        // Both idle (tie): the real hook session must win regardless of order.
        let placeholder = window_snap("myproj", IdeKind::Trae);
        let real = snap("s1", "myproj", IdeKind::Trae, SessionStatus::Idle, "SessionStart");
        let out = deduplicate_sessions(vec![placeholder, real.clone()]);
        assert_eq!(out.len(), 1);
        assert_eq!(out[0].session_id, "s1");
        let out2 = deduplicate_sessions(vec![real, window_snap("myproj", IdeKind::Trae)]);
        assert_eq!(out2.len(), 1);
        assert_eq!(out2[0].session_id, "s1");
    }

    #[test]
    fn get_snapshot_dedupes_same_project_different_sessions() {
        let (mut sm, _t) = make_manager();
        // Two hook sessions for the same project+ide, distinct session_ids
        // (mimics Qoder expert/sub-agent spawning multiple processes).
        let mut e1 = event("s1", "UserPromptSubmit");
        e1.ide = Some(IdeKind::Qoder);
        sm.handle_hook_event(&e1);
        let mut e2 = event("s2", "SessionStart");
        e2.ide = Some(IdeKind::Qoder);
        sm.handle_hook_event(&e2);
        // Internal tracking still keeps both sessions.
        assert_eq!(sm.session_count(), 2);
        // Snapshot collapses them into one entry, keeping the Working one.
        let snap = sm.get_snapshot();
        assert_eq!(snap.sessions.len(), 1);
        assert_eq!(snap.sessions[0].status, SessionStatus::Working);
        assert_eq!(snap.sessions[0].session_id, "s1");
    }
}
