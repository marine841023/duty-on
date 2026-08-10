//! Hook installer for Trae, Qoder, Cursor, Codex CLI and OpenCode integration.
//! Installs bridge scripts to `~/.dutyon/hooks/` and registers hook entries
//! in `~/.trae-cn/hooks.json`, `~/.qoder/settings.json`,
//! `~/.cursor/hooks.json` and `~/.codex/hooks.json`. OpenCode has no
//! config-file hook, so for it we write a JS plugin to
//! `~/.config/opencode/plugins/dutyon-bridge.js` instead.

use crate::config;
use serde::Serialize;
use serde_json::{json, Value};
use std::fs;
use std::path::{Path, PathBuf};

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InstallResult {
    pub success: bool,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub error: Option<String>,
    /// Non-fatal notice (e.g. a corrupt config was backed up and rebuilt).
    /// Install succeeded, but the user should know something happened.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub warning: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub hook_dir: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub hooks_path: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub qoder_hooks_path: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub cursor_hooks_path: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub codex_hooks_path: Option<String>,
    /// OpenCode plugin path (~/.config/opencode/plugins/dutyon-bridge.js) when
    /// the plugin was written. OpenCode has no hook config; integration is a
    /// JS plugin that POSTs events to this app's HTTP server.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub opencode_plugin_path: Option<String>,
    pub needs_enable: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InstalledStatus {
    pub installed: bool,
    pub hooks_exist: bool,
    pub bridge_exists: bool,
    pub qoder_hooks_exist: bool,
    pub cursor_hooks_exist: bool,
    pub codex_hooks_exist: bool,
    pub opencode_plugin_exist: bool,
}

/// Hook config shapes differ per IDE:
/// - `Nested` (Trae/Qoder/Codex, Claude-Code style): event -> [{"hooks": [...]}]
/// - `Flat`   (Cursor): event -> [{"command": ..., "timeout": ...}]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum HookFormat {
    Nested,
    Flat,
}

fn home() -> PathBuf {
    dirs::home_dir().unwrap_or_else(|| PathBuf::from("."))
}

/// Bridge script filename for the current platform. Windows uses PowerShell
/// (Trae IDE runs hook commands via PowerShell there); macOS/Linux use a bash
/// script (Trae runs hook commands via sh).
fn bridge_filename() -> &'static str {
    if cfg!(windows) {
        "trae-hook-bridge.ps1"
    } else {
        "trae-hook-bridge.sh"
    }
}

/// The hook command string written into the IDE's hooks config, executed by
/// its hook runner. On Windows we launch an explicit `powershell -File` child
/// with `-ExecutionPolicy Bypass`: corporate/managed machines often set the
/// script execution policy to Restricted, which would silently refuse the
/// .ps1 bridge (the hook fires but nothing happens, and bridge.log never gets
/// its first line). `-NoProfile` keeps startup fast. `$env:USERPROFILE` is
/// expanded by the runner's own PowerShell before the child starts.
/// On macOS/Linux: `bash "$HOME/..."` (sh-compatible) — explicit `bash`
/// avoids depending on the executable bit and the `~` expansion quirks of
/// some shells.
/// The `ide` argument ("trae" / "qoder" / "cursor" / "codex") is forwarded by
/// the bridge so the pet can badge each session with its source IDE. Cursor's
/// stdin payload names events in camelCase (which the state machine does
/// not match), so for Cursor the event is baked into the command (`event`
/// arg) and the bridge overrides the payload's name with the canonical one.
fn hook_command(ide: &str, event: Option<&str>) -> String {
    if cfg!(windows) {
        // Named -HookEvent, NOT -Event: a param named $Event collides with
        // PowerShell's automatic $Event variable and degraded every parsed
        // JSON object to a String (verified in the field: all POSTs 422).
        let event_arg = event.map(|e| format!(" -HookEvent {}", e)).unwrap_or_default();
        format!(
            r#"powershell -NoProfile -ExecutionPolicy Bypass -File "$env:USERPROFILE\.dutyon\hooks\trae-hook-bridge.ps1" -Ide {}{}"#,
            ide, event_arg
        )
    } else {
        let event_arg = event.map(|e| format!(" {}", e)).unwrap_or_default();
        format!(r#"bash "$HOME/.dutyon/hooks/trae-hook-bridge.sh" {}{}"#, ide, event_arg)
    }
}

/// True if a hook command belongs to this app. Matches both the current
/// `.dutyon` dir and the legacy `.trae-pet` dir so re-installs after the
/// rename dedupe/replace pre-rename entries instead of stacking alongside.
fn is_pet_command(cmd: &str) -> bool {
    cmd.contains(".dutyon") || cmd.contains(".trae-pet")
}

/// Strip a UTF-8 BOM if present. Some editors/IDEs write JSON configs with a
/// BOM, which makes serde_json reject an otherwise-valid file.
fn strip_bom(s: &str) -> &str {
    s.strip_prefix('\u{feff}').unwrap_or(s)
}

/// Copy `path` to a sibling backup file (`<name>.dutyon-backup-<unix-ts>`)
/// before rewriting it, so a user whose config we can't merge can recover
/// the original. Returns the backup path on success.
fn backup_file(path: &Path) -> Option<String> {
    let ts = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    let name = format!(
        "{}.dutyon-backup-{}",
        path.file_name()
            .map(|n| n.to_string_lossy().to_string())
            .unwrap_or_default(),
        ts
    );
    let backup = path.with_file_name(name);
    fs::copy(path, &backup)
        .ok()
        .map(|_| backup.to_string_lossy().to_string())
}

/// Qoder's `shell` field value per platform. Qoder supports an explicit
/// `shell` per hook entry ("powershell" / "bash"); setting it guarantees the
/// bridge command runs under the right interpreter regardless of the system
/// default shell. The command string itself is the same as Trae's.
fn qoder_shell() -> &'static str {
    if cfg!(windows) {
        "powershell"
    } else {
        "bash"
    }
}

/// Merge pet hook entries into a hooks config file, preserving any other keys
/// already present (e.g. Qoder's `enabledPlugins` and hooks from other tools).
/// For each event in `events`, removes existing pet entries (to avoid
/// duplicates on re-install) then appends fresh ones. `hook_command` maps an
/// event name to its command string (Trae/Qoder use one command for all
/// events; Cursor bakes the event into each command). `shell` is written into
/// each hook entry when given (Qoder honors it; Trae ignores the unknown
/// field, so we only set it for Qoder). `add_version` writes a `version: 1`
/// top-level field (Trae-style hooks.json and Cursor's schema both use it).
/// `strip_version` removes any existing top-level `version` field — used for
/// Codex, whose CLI rejects `version` as an unknown field (`unknown field
/// \`version\`, expected \`description\` or \`hooks\``); setting it on re-install
/// also repairs configs written by older DutyOn builds that incorrectly
/// included it. `format` selects the entry shape (see `HookFormat`).
///
/// Robustness against configs modified by other tools:
/// - A UTF-8 BOM is tolerated.
/// - If the file exists but is unparseable (or its root isn't an object, or
///   `hooks` isn't an object), the ORIGINAL file is backed up first, then we
///   rebuild from `{}` and return a warning — we never silently wipe the
///   user's existing config.
/// - If an event's value isn't an array, the original value is wrapped as the
///   first element of the merged array instead of being dropped.
///
/// Returns `Ok(Some(warning))` when a recovery step was taken.
fn merge_hooks_into_file(
    path: &Path,
    events: &[&str],
    hook_command: &dyn Fn(&str) -> String,
    shell: Option<&str>,
    add_version: bool,
    strip_version: bool,
    format: HookFormat,
) -> Result<Option<String>, String> {
    if let Some(parent) = path.parent() {
        let _ = fs::create_dir_all(parent);
    }

    let mut warning: Option<String> = None;
    let mut existing: Value = json!({});
    match fs::read_to_string(path) {
        Ok(content) => {
            let content = strip_bom(&content);
            if !content.trim().is_empty() {
                match serde_json::from_str::<Value>(content) {
                    Ok(v) if v.is_object() => existing = v,
                    Ok(_) => {
                        // Root isn't an object (array/string/...) — can't
                        // merge into it. Back up, rebuild, and warn.
                        let backup = backup_file(path);
                        warning = Some(format!(
                            "{}: root was not a JSON object; backed up to {} and rebuilt",
                            path.display(),
                            backup.unwrap_or_else(|| "<backup failed>".to_string())
                        ));
                        log::warn!("[hooks] {}", warning.as_deref().unwrap_or(""));
                    }
                    Err(e) => {
                        // Exists but not valid JSON — another tool may have
                        // left it in a foreign format. Never overwrite
                        // silently: back up the original, rebuild from {},
                        // and surface the incident.
                        let backup = backup_file(path);
                        warning = Some(format!(
                            "{}: not valid JSON ({}); backed up to {} and rebuilt",
                            path.display(),
                            e,
                            backup.unwrap_or_else(|| "<backup failed>".to_string())
                        ));
                        log::warn!("[hooks] {}", warning.as_deref().unwrap_or(""));
                    }
                }
            }
        }
        Err(_) => {} // Missing file -> start fresh.
    }

    if strip_version {
        // Codex CLI rejects `version` as an unknown field; remove it so
        // re-installs also fix configs written by older DutyOn versions.
        if let Some(o) = existing.as_object_mut() {
            o.remove("version");
        }
    } else if add_version && existing.get("version").is_none() {
        existing["version"] = json!(1);
    }
    if existing.get("hooks").is_some() && !existing["hooks"].is_object() {
        // `hooks` present but wrong shape — back up before resetting it.
        let backup = backup_file(path);
        let note = format!(
            "{}: \"hooks\" was not a JSON object; backed up to {} and reset it",
            path.display(),
            backup.unwrap_or_else(|| "<backup failed>".to_string())
        );
        log::warn!("[hooks] {}", note);
        warning = Some(match warning.take() {
            Some(prev) => format!("{}; {}", prev, note),
            None => note,
        });
        existing["hooks"] = json!({});
    } else if existing.get("hooks").is_none() {
        existing["hooks"] = json!({});
    }

    if let Some(hooks) = existing["hooks"].as_object_mut() {
        for event in events {
            // Build the pet entry for this event in the IDE's native shape.
            let group = match format {
                HookFormat::Nested => {
                    let mut g = json!({
                        "hooks": [{
                            "type": "command",
                            "command": hook_command(event),
                            "timeout": config::BRIDGE_TIMEOUT_SEC
                        }]
                    });
                    if let Some(shell) = shell {
                        g["hooks"][0]["shell"] = json!(shell);
                    }
                    g
                }
                // Cursor's flat entry. `loop_limit: null` lifts Cursor's
                // default limit of 5 invocations for stop-style events, so
                // the pet keeps receiving events across a long session.
                HookFormat::Flat => json!({
                    "command": hook_command(event),
                    "timeout": config::BRIDGE_TIMEOUT_SEC,
                    "loop_limit": null
                }),
            };
            let arr = hooks.entry(event.to_string()).or_insert(json!([]));
            if !arr.is_array() {
                // Another tool wrote a non-array value for this event. Keep
                // the original value by wrapping it as the first element
                // instead of dropping it, then append our entry.
                let old = arr.take();
                *arr = json!([old]);
                if warning.is_none() {
                    warning = Some(format!(
                        "{}: event \"{}\" had a non-array value; preserved it inside the merged array",
                        path.display(),
                        event
                    ));
                }
            }
            if let Some(a) = arr.as_array_mut() {
                // Remove existing pet entries (avoid duplicates on re-install).
                match format {
                    HookFormat::Nested => a.retain(|g| {
                        g["hooks"]
                            .as_array()
                            .map(|hs| {
                                !hs.iter().any(|h| {
                                    h["command"]
                                        .as_str()
                                        .map(is_pet_command)
                                        .unwrap_or(false)
                                })
                            })
                            .unwrap_or(true)
                    }),
                    HookFormat::Flat => a.retain(|e| {
                        !e["command"]
                            .as_str()
                            .map(is_pet_command)
                            .unwrap_or(false)
                    }),
                }
                a.push(group);
            }
        }
    }

    let pretty = serde_json::to_string_pretty(&existing).unwrap_or_else(|_| "{}".to_string());
    fs::write(path, pretty).map_err(|e| format!("Failed to write {}: {}", path.display(), e))?;
    Ok(warning)
}

/// Create a failed InstallResult with the given error message.
fn fail(error: &str) -> InstallResult {
    InstallResult {
        success: false,
        error: Some(error.to_string()),
        warning: None,
        hook_dir: None,
        hooks_path: None,
        qoder_hooks_path: None,
        cursor_hooks_path: None,
        codex_hooks_path: None,
        opencode_plugin_path: None,
        needs_enable: false,
    }
}

/// Marker string the plugin source always contains; `is_installed` looks for
/// it to tell our plugin from a user's own file with the same name.
const OPENCODE_PLUGIN_MARKER: &str = "id: 'dutyon'";

/// The self-contained OpenCode bridge plugin source, written verbatim to
/// `~/.config/opencode/plugins/dutyon-bridge.js`. OpenCode has no config-file
/// hook (unlike Trae/Cursor/Codex), so integration is a JS plugin that
/// subscribes to OpenCode's event bus and POSTs canonical hook events to this
/// app's HTTP server. Shape (`export default { id, server }`) matches the
/// proven Copiwaifu plugin; the `server` entry ensures the `event` hook runs
/// on OpenCode's server side where session/message/permission events fire.
/// Event mapping mirrors Copiwaifu's verified field paths:
///   session.created                       -> SessionStart
///   message.part.updated(text, role=user) -> UserPromptSubmit
///   message.part.updated(tool, running)   -> PreToolUse
///   message.part.updated(tool, completed) -> PostToolUse
///   session.status(idle)/session.deleted  -> Stop
///   permission.asked/question.asked       -> PermissionRequest
const OPENCODE_PLUGIN_SOURCE: &str = r#"// DutyOn (开工啦) bridge plugin for OpenCode.
// Auto-generated by the DutyOn desktop pet. Subscribes to OpenCode lifecycle
// events and POSTs canonical hook events to DutyOn's local HTTP server
// (http://127.0.0.1:17521/hook) so the pet reflects your OpenCode session
// state (idle/working/alert) in real time.
//
// OpenCode auto-loads JS plugins from ~/.config/opencode/plugins/ at startup,
// so if OpenCode is already running, restart it after installing. Safe to
// delete — re-installing from DutyOn recreates this file.
//
// Why a plugin (not a config hook): OpenCode has no Claude-Code-style shell
// hook config; its only extension point is a JS plugin on the event bus. The
// `export default { id, server }` shape registers the `event` hook on
// OpenCode's server side, where session/message/permission events fire.

import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const ENDPOINT = 'http://127.0.0.1:17521/hook'
const IDE = 'opencode'
const LOG_DIR = path.join(os.homedir(), '.dutyon', 'hooks')
const LOG_PATH = path.join(LOG_DIR, 'opencode-bridge.log')

function log(msg) {
  try {
    fs.mkdirSync(LOG_DIR, { recursive: true })
    try {
      const lines = fs.readFileSync(LOG_PATH, 'utf8').split('\n')
      if (lines.length > 500) fs.writeFileSync(LOG_PATH, lines.slice(-250).join('\n'))
    } catch (e) {}
    const ts = new Date().toISOString().replace('T', ' ').slice(0, 19)
    fs.appendFileSync(LOG_PATH, '[' + ts + '] ' + msg + '\n')
  } catch (e) {}
}

async function post(payload) {
  try {
    const ctrl = new AbortController()
    const timer = setTimeout(() => ctrl.abort(), 2000)
    const res = await fetch(ENDPOINT, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
      signal: ctrl.signal,
    })
    clearTimeout(timer)
    log('POST ' + payload.hook_event_name + ' -> ' + (res ? res.status : '?'))
  } catch (e) {
    log('POST ' + payload.hook_event_name + ' failed: ' + ((e && e.message) || e))
  }
}

function sid(raw) {
  return raw ? 'opencode-' + raw : 'opencode-session'
}

function projectName(dir) {
  if (!dir) return ''
  const parts = String(dir).replace(/\\/g, '/').split('/').filter(Boolean)
  return parts[parts.length - 1] || ''
}

function makeHooks() {
  // messageID -> { role, sessionID }, populated from message.updated. Needed
  // because message.part.updated (text) carries messageID, not sessionID.
  const messageRoles = new Map()
  // OpenCode session id -> cwd, captured from session.created/updated so later
  // events (which may omit the directory) still report the right project.
  const sessionCwd = new Map()

  return {
    event: async ({ event }) => {
      const type = event && event.type
      const p = (event && event.properties) || {}
      try {
        if (type === 'session.created' && p.info && p.info.id) {
          const cwd = p.info.directory || ''
          if (cwd) sessionCwd.set(p.info.id, cwd)
          await post({ hook_event_name: 'SessionStart', session_id: sid(p.info.id), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd) })
          return
        }
        if (type === 'session.updated' && p.info && p.info.id) {
          if (p.info.directory) sessionCwd.set(p.info.id, p.info.directory)
          if (p.info.time && p.info.time.archived) {
            const cwd = sessionCwd.get(p.info.id) || ''
            await post({ hook_event_name: 'Stop', session_id: sid(p.info.id), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd) })
          }
          return
        }
        if (type === 'session.deleted' && p.info && p.info.id) {
          const cwd = sessionCwd.get(p.info.id) || ''
          await post({ hook_event_name: 'Stop', session_id: sid(p.info.id), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd) })
          return
        }
        // session.status(idle) = the agent finished its turn -> Stop.
        if (type === 'session.status' && p.sessionID && p.status && p.status.type === 'idle') {
          const cwd = sessionCwd.get(p.sessionID) || ''
          await post({ hook_event_name: 'Stop', session_id: sid(p.sessionID), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd) })
          return
        }
        // Track message role/session so the text-part event below can resolve
        // its session. Cap the map to avoid unbounded growth across long runs.
        if (type === 'message.updated' && p.info && p.info.id && p.info.sessionID) {
          messageRoles.set(p.info.id, { role: p.info.role, sessionID: p.info.sessionID })
          if (messageRoles.size > 300) messageRoles.delete(messageRoles.keys().next().value)
          return
        }
        // User-submitted prompt text -> the agent is now working (thinking).
        if (type === 'message.part.updated' && p.part && p.part.messageID && p.part.type === 'text') {
          const meta = messageRoles.get(p.part.messageID)
          if (!meta) return
          if (meta.role === 'user' && p.part.text) {
            const cwd = sessionCwd.get(meta.sessionID) || ''
            await post({ hook_event_name: 'UserPromptSubmit', session_id: sid(meta.sessionID), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd) })
          }
          return
        }
        // Tool calls arrive as message parts with type=tool; map running/
        // completed/error to PreToolUse/PostToolUse (keeps the session Working;
        // session.status(idle) later returns it to Idle).
        if (type === 'message.part.updated' && p.part && p.part.sessionID && p.part.type === 'tool') {
          const cwd = sessionCwd.get(p.part.sessionID) || ''
          const toolName = p.part.tool || 'tool'
          const status = p.part.state && p.part.state.status
          const base = { session_id: sid(p.part.sessionID), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd), tool_name: toolName }
          if (status === 'running' || status === 'pending') {
            await post(Object.assign({}, base, { hook_event_name: 'PreToolUse' }))
          } else if (status === 'completed' || status === 'error') {
            await post(Object.assign({}, base, { hook_event_name: 'PostToolUse' }))
          }
          return
        }
        // Agent needs your input (permission prompt or a direct question).
        if (type === 'permission.asked' && p.sessionID) {
          const cwd = sessionCwd.get(p.sessionID) || ''
          await post({ hook_event_name: 'PermissionRequest', session_id: sid(p.sessionID), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd) })
          return
        }
        if (type === 'question.asked' && p.sessionID) {
          const cwd = sessionCwd.get(p.sessionID) || ''
          await post({ hook_event_name: 'PermissionRequest', session_id: sid(p.sessionID), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd), tool_name: 'AskUserQuestion' })
        }
      } catch (e) {
        log('event ' + type + ' failed: ' + ((e && e.message) || e))
      }
    },
  }
}

export default {
  id: 'dutyon',
  server: async () => makeHooks(),
}
"#;

/// Install the hook bridge script and merge hooks.json.
/// `hooks_source_dir` is the bundled resource dir containing the .ps1 files
/// (resolved by the caller via the Tauri resource path).
pub fn install(hooks_source_dir: &Path) -> InstallResult {
    let bridge_name = bridge_filename();
    let bridge_src = hooks_source_dir.join(bridge_name);
    if !bridge_src.exists() {
        return fail(&format!("Bridge script not found: {}", bridge_src.display()));
    }

    let home = home();
    let target_hook_dir = home.join(".dutyon").join("hooks");
    if let Err(e) = fs::create_dir_all(&target_hook_dir) {
        return fail(&format!("Failed to create hook dir: {}", e));
    }

    // Copy bridge script.
    let bridge_dst = target_hook_dir.join(bridge_name);
    if let Err(e) = copy_text(&bridge_src, &bridge_dst) {
        return fail(&format!("Failed to copy bridge: {}", e));
    }

    // On Unix the bridge must be executable (Windows uses `bash <path>`
    // explicitly so the exec bit isn't required, but set it for direct
    // invocation robustness).
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = fs::set_permissions(&bridge_dst, fs::Permissions::from_mode(0o755));
    }

    // Copy the standalone Windows installer script if present (harmless on
    // macOS/Linux; skipped otherwise via the exists() check).
    let installer_src = hooks_source_dir.join("install-hooks.ps1");
    if installer_src.exists() {
        let _ = copy_text(&installer_src, &target_hook_dir.join("install-hooks.ps1"));
    }

    // Merge hooks into Trae's ~/.trae-cn/hooks.json (Trae events, no shell
    // field, Trae-style version key).
    let trae_hooks_path = home.join(".trae-cn").join("hooks.json");
    let trae_hook_command = hook_command("trae", None);
    let mut warnings: Vec<String> = Vec::new();
    match merge_hooks_into_file(
        &trae_hooks_path,
        config::HOOK_EVENTS,
        &|_| trae_hook_command.clone(),
        None,
        true,
        false,
        HookFormat::Nested,
    ) {
        Ok(Some(w)) => warnings.push(w),
        Ok(None) => {}
        Err(e) => return fail(&e),
    }

    // Merge hooks into Qoder's ~/.qoder/settings.json when Qoder is installed.
    // Wires the 4 documented IDE events plus Notification/PermissionRequest
    // (CLI-documented; harmless if the IDE never fires them). Qoder honors
    // the `shell` field. Non-fatal: a Qoder failure doesn't undo the Trae
    // install — the user can re-run install after installing Qoder.
    let qoder_dir = home.join(".qoder");
    let mut qoder_hooks_path_str: Option<String> = None;
    if qoder_dir.exists() {
        let qoder_hooks_path = qoder_dir.join("settings.json");
        let qoder_hook_command = hook_command("qoder", None);
        match merge_hooks_into_file(
            &qoder_hooks_path,
            config::QODER_HOOK_EVENTS,
            &|_| qoder_hook_command.clone(),
            Some(qoder_shell()),
            false,
            false,
            HookFormat::Nested,
        ) {
            Ok(maybe_warning) => {
                if let Some(w) = maybe_warning {
                    warnings.push(format!("Qoder: {}", w));
                }
                qoder_hooks_path_str = Some(qoder_hooks_path.to_string_lossy().to_string());
            }
            Err(e) => {
                log::warn!("[hooks] Qoder settings.json merge failed: {}", e);
                warnings.push(format!("Qoder settings.json merge failed: {}", e));
            }
        }
    } else {
        log::info!("[hooks] Qoder not detected (~/.qoder absent); skipping Qoder hook install");
    }

    // Merge hooks into Cursor's ~/.cursor/hooks.json when Cursor is
    // installed. Cursor uses its own flatter schema and camelCase events;
    // each command carries the event name because Cursor's stdin payload
    // doesn't include one (the bridge normalizes it). Cursor reloads
    // hooks.json on save, so no IDE restart is needed. Non-fatal like Qoder.
    let cursor_dir = home.join(".cursor");
    let mut cursor_hooks_path_str: Option<String> = None;
    if cursor_dir.exists() {
        let cursor_hooks_path = cursor_dir.join("hooks.json");
        match merge_hooks_into_file(
            &cursor_hooks_path,
            config::CURSOR_HOOK_EVENTS,
            &|event| hook_command("cursor", Some(event)),
            None,
            true,
            false,
            HookFormat::Flat,
        ) {
            Ok(maybe_warning) => {
                if let Some(w) = maybe_warning {
                    warnings.push(format!("Cursor: {}", w));
                }
                cursor_hooks_path_str =
                    Some(cursor_hooks_path.to_string_lossy().to_string());
            }
            Err(e) => {
                log::warn!("[hooks] Cursor hooks.json merge failed: {}", e);
                warnings.push(format!("Cursor hooks.json merge failed: {}", e));
            }
        }
    } else {
        log::info!("[hooks] Cursor not detected (~/.cursor absent); skipping Cursor hook install");
    }

    // Merge hooks into Codex CLI's ~/.codex/hooks.json when Codex is installed.
    // Codex uses the same PascalCase event names and nested JSON schema as
    // Trae (Claude Code style), so no event-name baking is needed. Unlike
    // Trae, Codex's CLI does NOT accept a top-level `version` field (codex
    // 0.147.0 rejects it as `unknown field \`version\``, expecting only
    // `description` or `hooks`), so we pass add_version=false and
    // strip_version=true — the latter also repairs configs written by older
    // DutyOn builds that incorrectly included `version: 1`. Codex requires
    // hooks to be trusted via `/hooks` in the CLI after install. Non-fatal
    // like Qoder/Cursor.
    let codex_dir = home.join(".codex");
    let mut codex_hooks_path_str: Option<String> = None;
    if codex_dir.exists() {
        let codex_hooks_path = codex_dir.join("hooks.json");
        let codex_hook_command = hook_command("codex", None);
        match merge_hooks_into_file(
            &codex_hooks_path,
            config::CODEX_HOOK_EVENTS,
            &|_| codex_hook_command.clone(),
            None,
            false,
            true,
            HookFormat::Nested,
        ) {
            Ok(maybe_warning) => {
                if let Some(w) = maybe_warning {
                    warnings.push(format!("Codex: {}", w));
                }
                codex_hooks_path_str = Some(codex_hooks_path.to_string_lossy().to_string());
            }
            Err(e) => {
                log::warn!("[hooks] Codex hooks.json merge failed: {}", e);
                warnings.push(format!("Codex hooks.json merge failed: {}", e));
            }
        }
    } else {
        log::info!("[hooks] Codex not detected (~/.codex absent); skipping Codex hook install");
    }

    // Write the OpenCode bridge plugin when OpenCode is installed. OpenCode
    // has no config-file hook; it auto-loads JS plugins from
    // ~/.config/opencode/plugins/ at startup. The plugin subscribes to
    // OpenCode's event bus and POSTs canonical hook events to this app's HTTP
    // server. Only written when ~/.config/opencode/ exists (OpenCode creates
    // it on first run); if absent, the user runs OpenCode once then re-
    // installs. Non-fatal like Qoder/Cursor/Codex.
    // OpenCode creates ~/.config/opencode/ on first run; only write the plugin
    // when that root exists, so we don't spawn a stray plugins/ tree on
    // machines without OpenCode. `OPENCODE_PLUGIN_SUBDIR` is the full plugins
    // path (~/.config/opencode/plugins); its parent is the OpenCode root.
    let plugins_dir = home.join(config::OPENCODE_PLUGIN_SUBDIR);
    let opencode_detected = plugins_dir.parent().map(|p| p.exists()).unwrap_or(false);
    let mut opencode_plugin_path_str: Option<String> = None;
    if opencode_detected {
        if let Err(e) = fs::create_dir_all(&plugins_dir) {
            log::warn!("[hooks] OpenCode plugins dir create failed: {}", e);
            warnings.push(format!("OpenCode plugins dir create failed: {}", e));
        } else {
            let plugin_path = plugins_dir.join(config::OPENCODE_PLUGIN_FILENAME);
            match fs::write(&plugin_path, OPENCODE_PLUGIN_SOURCE) {
                Ok(_) => {
                    log::info!(
                        "[hooks] OpenCode plugin installed at {}; emits events: {:?}",
                        plugin_path.display(),
                        config::OPENCODE_HOOK_EVENTS
                    );
                    opencode_plugin_path_str =
                        Some(plugin_path.to_string_lossy().to_string());
                }
                Err(e) => {
                    log::warn!("[hooks] OpenCode plugin write failed: {}", e);
                    warnings.push(format!("OpenCode plugin write failed: {}", e));
                }
            }
        }
    } else {
        log::info!("[hooks] OpenCode not detected (~/.config/opencode absent); skipping OpenCode plugin install");
    }

    InstallResult {
        success: true,
        error: None,
        warning: if warnings.is_empty() {
            None
        } else {
            Some(warnings.join("\n"))
        },
        hook_dir: Some(target_hook_dir.to_string_lossy().to_string()),
        hooks_path: Some(trae_hooks_path.to_string_lossy().to_string()),
        qoder_hooks_path: qoder_hooks_path_str,
        cursor_hooks_path: cursor_hooks_path_str,
        codex_hooks_path: codex_hooks_path_str,
        opencode_plugin_path: opencode_plugin_path_str,
        needs_enable: true,
    }
}

/// Check whether hooks are already installed. Reports Trae, Qoder, Cursor,
/// Codex and OpenCode independently; `installed` is true when the bridge
/// exists AND at least one IDE has the pet hooks wired (OpenCode counts when
/// its plugin file carrying the DutyOn marker is present).
pub fn is_installed() -> InstalledStatus {
    let home = home();
    let trae_hooks_path = home.join(".trae-cn").join("hooks.json");
    let qoder_hooks_path = home.join(".qoder").join("settings.json");
    let cursor_hooks_path = home.join(".cursor").join("hooks.json");
    let codex_hooks_path = home.join(".codex").join("hooks.json");
    let opencode_plugin_path = home
        .join(config::OPENCODE_PLUGIN_SUBDIR)
        .join(config::OPENCODE_PLUGIN_FILENAME);
    let bridge_path = home.join(".dutyon").join("hooks").join(bridge_filename());

    let bridge_exists = bridge_path.exists();

    let contains_pet = |path: &Path| -> bool {
        path.exists()
            && fs::read_to_string(path)
                .map(|c| is_pet_command(&c))
                .unwrap_or(false)
    };
    let trae_hooks_contain_pet = contains_pet(&trae_hooks_path);
    let qoder_hooks_contain_pet = contains_pet(&qoder_hooks_path);
    let cursor_hooks_contain_pet = contains_pet(&cursor_hooks_path);
    let codex_hooks_contain_pet = contains_pet(&codex_hooks_path);
    // OpenCode has no hook config; "installed" = our plugin file exists and
    // carries the DutyOn marker (not just any file with the same name).
    let opencode_plugin_exists = opencode_plugin_path.exists()
        && fs::read_to_string(&opencode_plugin_path)
            .map(|c| c.contains(OPENCODE_PLUGIN_MARKER))
            .unwrap_or(false);

    InstalledStatus {
        installed: bridge_exists
            && (trae_hooks_contain_pet
                || qoder_hooks_contain_pet
                || cursor_hooks_contain_pet
                || codex_hooks_contain_pet
                || opencode_plugin_exists),
        hooks_exist: trae_hooks_contain_pet,
        bridge_exists,
        qoder_hooks_exist: qoder_hooks_contain_pet,
        cursor_hooks_exist: cursor_hooks_contain_pet,
        codex_hooks_exist: codex_hooks_contain_pet,
        opencode_plugin_exist: opencode_plugin_exists,
    }
}

fn copy_text(src: &Path, dst: &Path) -> std::io::Result<()> {
    let content = fs::read_to_string(src)?;
    fs::write(dst, content)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Qoder merge must preserve non-hooks keys (enabledPlugins etc.), must NOT
    /// add a Trae-style `version`, must wire only the Qoder event subset, and
    /// must set the `shell` field on each hook entry.
    #[test]
    fn qoder_merge_preserves_other_keys_and_adds_hooks() {
        let dir = std::env::temp_dir().join("duty-on-qoder-merge-test");
        let _ = fs::create_dir_all(&dir);
        let path = dir.join("settings.json");
        let original = r#"{
            "aicodingPluginSettingsMigrationVersion": 1,
            "enabledPlugins": { "foo@bar": true, "baz@qux": false }
        }"#;
        fs::write(&path, original).unwrap();

        let cmd = r#"& "$env:USERPROFILE\.dutyon\hooks\trae-hook-bridge.ps1""#.to_string();
        merge_hooks_into_file(&path, config::QODER_HOOK_EVENTS, &|_| cmd.clone(), Some("powershell"), false, false, HookFormat::Nested)
            .unwrap();

        let v: Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        // Non-hooks keys preserved exactly.
        assert_eq!(v["aicodingPluginSettingsMigrationVersion"], 1);
        assert_eq!(v["enabledPlugins"]["foo@bar"], true);
        assert_eq!(v["enabledPlugins"]["baz@qux"], false);
        // No Trae-style version key for Qoder.
        assert!(v.get("version").is_none());
        // Each Qoder event wired exactly once, with shell field.
        for ev in config::QODER_HOOK_EVENTS {
            let arr = v["hooks"][ev].as_array().expect(ev);
            assert_eq!(arr.len(), 1, "expected 1 group for {}", ev);
            assert_eq!(arr[0]["hooks"][0]["type"], "command");
            assert_eq!(arr[0]["hooks"][0]["shell"], "powershell");
            assert_eq!(arr[0]["hooks"][0]["timeout"], config::BRIDGE_TIMEOUT_SEC);
        }
        // SessionStart remains Trae-only; Notification is deliberately wired
        // for Qoder now (CLI-documented, may fire for the ask-user dialog).
        assert!(v["hooks"].get("SessionStart").is_none());
        assert_eq!(v["hooks"]["Notification"].as_array().unwrap().len(), 1);

        let _ = fs::remove_dir_all(&dir);
    }

    /// Trae merge adds the `version` key, wires all Trae events, and does NOT
    /// set a `shell` field (Trae ignores it; we leave it off for cleanliness).
    #[test]
    fn trae_merge_adds_version_and_all_events_no_shell() {
        let dir = std::env::temp_dir().join("duty-on-trae-merge-test");
        let _ = fs::create_dir_all(&dir);
        let path = dir.join("hooks.json");
        fs::write(&path, "{}").unwrap();

        let cmd = r#"& "$env:USERPROFILE\.dutyon\hooks\trae-hook-bridge.ps1""#.to_string();
        merge_hooks_into_file(&path, config::HOOK_EVENTS, &|_| cmd.clone(), None, true, false, HookFormat::Nested).unwrap();

        let v: Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        assert_eq!(v["version"], 1);
        for ev in config::HOOK_EVENTS {
            assert_eq!(v["hooks"][ev].as_array().unwrap().len(), 1);
        }
        // No shell field for Trae entries.
        assert!(v["hooks"]["SessionStart"][0]["hooks"][0].get("shell").is_none());

        let _ = fs::remove_dir_all(&dir);
    }

    /// Re-running install must not stack duplicate hook groups (dedup by the
    /// pet marker in the command string).
    #[test]
    fn merge_is_idempotent_no_duplicates() {
        let dir = std::env::temp_dir().join("duty-on-idempotent-test");
        let _ = fs::create_dir_all(&dir);
        let path = dir.join("settings.json");
        fs::write(&path, "{}").unwrap();

        let cmd = r#"bash "$HOME/.dutyon/hooks/trae-hook-bridge.sh""#.to_string();
        merge_hooks_into_file(&path, config::QODER_HOOK_EVENTS, &|_| cmd.clone(), Some("bash"), false, false, HookFormat::Nested).unwrap();
        merge_hooks_into_file(&path, config::QODER_HOOK_EVENTS, &|_| cmd.clone(), Some("bash"), false, false, HookFormat::Nested).unwrap();

        let v: Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        for ev in config::QODER_HOOK_EVENTS {
            assert_eq!(
                v["hooks"][ev].as_array().unwrap().len(),
                1,
                "duplicate group for {}",
                ev
            );
        }

        let _ = fs::remove_dir_all(&dir);
    }

    /// Pre-rename installs used the `.trae-pet` dir. A fresh install must
    /// replace those legacy groups (not stack alongside them), and non-pet
    /// groups must be preserved.
    #[test]
    fn merge_replaces_legacy_trae_pet_entries() {
        let dir = std::env::temp_dir().join("duty-on-legacy-dedup-test");
        let _ = fs::create_dir_all(&dir);
        let path = dir.join("settings.json");
        let legacy = r#"{
            "hooks": {
                "Stop": [
                    { "hooks": [{ "type": "command", "command": "bash \"$HOME/.trae-pet/hooks/trae-hook-bridge.sh\"" }] },
                    { "hooks": [{ "type": "command", "command": "echo someone-else" }] }
                ]
            }
        }"#;
        fs::write(&path, legacy).unwrap();

        let cmd = r#"bash "$HOME/.dutyon/hooks/trae-hook-bridge.sh""#.to_string();
        merge_hooks_into_file(&path, &["Stop"], &|_| cmd.clone(), Some("bash"), false, false, HookFormat::Nested).unwrap();

        let v: Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        let arr = v["hooks"]["Stop"].as_array().unwrap();
        // Legacy pet group replaced by the new one; the third-party group kept.
        assert_eq!(arr.len(), 2);
        let cmds: Vec<&str> = arr
            .iter()
            .filter_map(|g| g["hooks"][0]["command"].as_str())
            .collect();
        assert!(cmds.iter().any(|c| c.contains(".dutyon")));
        assert!(!cmds.iter().any(|c| c.contains(".trae-pet")));
        assert!(cmds.iter().any(|c| c.contains("someone-else")));

        let _ = fs::remove_dir_all(&dir);
    }

    /// A UTF-8 BOM (some editors/IDEs write configs with one) must not break
    /// parsing: the merge succeeds on the parsed content with no warning.
    #[test]
    fn merge_tolerates_utf8_bom() {
        let dir = std::env::temp_dir().join("duty-on-bom-test");
        let _ = fs::create_dir_all(&dir);
        let path = dir.join("hooks.json");
        fs::write(
            &path,
            "\u{feff}{\"hooks\": {\"Stop\": [{\"hooks\": [{\"type\": \"command\", \"command\": \"echo other-tool\"}]}]}}",
        )
        .unwrap();

        let cmd = r#"& "$env:USERPROFILE\.dutyon\hooks\trae-hook-bridge.ps1""#.to_string();
        let warning = merge_hooks_into_file(&path, &["Stop"], &|_| cmd.clone(), None, true, false, HookFormat::Nested).unwrap();
        assert!(warning.is_none(), "BOM should not trigger a warning");

        let v: Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        let arr = v["hooks"]["Stop"].as_array().unwrap();
        assert_eq!(arr.len(), 2);
        assert!(arr[0]["hooks"][0]["command"] == "echo other-tool");

        let _ = fs::remove_dir_all(&dir);
    }

    /// A config that isn't valid JSON (e.g. left in a foreign format by
    /// another tool) must be BACKED UP before rebuilding — never silently
    /// wiped — and the incident must be reported as a warning.
    #[test]
    fn merge_backs_up_corrupt_file_instead_of_wiping() {
        let dir = std::env::temp_dir().join("duty-on-corrupt-test");
        let _ = fs::create_dir_all(&dir);
        let path = dir.join("hooks.json");
        let corrupt = "this is not json {{{";
        fs::write(&path, corrupt).unwrap();

        let cmd = r#"& "$env:USERPROFILE\.dutyon\hooks\trae-hook-bridge.ps1""#.to_string();
        let warning = merge_hooks_into_file(&path, &["Stop"], &|_| cmd.clone(), None, true, false, HookFormat::Nested).unwrap();
        let warning = warning.expect("corrupt file must produce a warning");
        assert!(warning.contains("backed up"), "warning: {}", warning);

        // The rebuilt file is valid JSON with our hook wired.
        let v: Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        assert_eq!(v["hooks"]["Stop"].as_array().unwrap().len(), 1);

        // A backup sibling exists and still holds the original content.
        let backups: Vec<_> = fs::read_dir(&dir)
            .unwrap()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_name().to_string_lossy().contains(".dutyon-backup-"))
            .collect();
        assert_eq!(backups.len(), 1);
        assert_eq!(fs::read_to_string(backups[0].path()).unwrap(), corrupt);

        let _ = fs::remove_dir_all(&dir);
    }

    /// When an event's value isn't an array (another tool wrote a different
    /// shape), the original value must be preserved inside the merged array
    /// instead of being dropped.
    #[test]
    fn merge_preserves_non_array_event_value() {
        let dir = std::env::temp_dir().join("duty-on-nonarray-test");
        let _ = fs::create_dir_all(&dir);
        let path = dir.join("settings.json");
        fs::write(&path, r#"{ "hooks": { "Stop": "echo legacy-string-form" } }"#).unwrap();

        let cmd = r#"bash "$HOME/.dutyon/hooks/trae-hook-bridge.sh""#.to_string();
        let warning = merge_hooks_into_file(&path, &["Stop"], &|_| cmd.clone(), Some("bash"), false, false, HookFormat::Nested).unwrap();
        assert!(warning.is_some(), "non-array event value must warn");

        let v: Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        let arr = v["hooks"]["Stop"].as_array().unwrap();
        assert_eq!(arr.len(), 2);
        assert_eq!(arr[0], json!("echo legacy-string-form"));
        assert!(arr[1]["hooks"][0]["command"].as_str().unwrap().contains(".dutyon"));

        let _ = fs::remove_dir_all(&dir);
    }

    /// A `hooks` key with the wrong shape must be backed up + reset, not
    /// silently destroyed.
    #[test]
    fn merge_backs_up_wrong_shape_hooks_key() {
        let dir = std::env::temp_dir().join("duty-on-hooks-shape-test");
        let _ = fs::create_dir_all(&dir);
        let path = dir.join("hooks.json");
        fs::write(&path, r#"{ "hooks": "weird" }"#).unwrap();

        let cmd = r#"& "$env:USERPROFILE\.dutyon\hooks\trae-hook-bridge.ps1""#.to_string();
        let warning = merge_hooks_into_file(&path, &["Stop"], &|_| cmd.clone(), None, true, false, HookFormat::Nested).unwrap();
        let warning = warning.expect("wrong-shape hooks must warn");
        assert!(warning.contains("backed up"), "warning: {}", warning);

        let v: Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        assert_eq!(v["hooks"]["Stop"].as_array().unwrap().len(), 1);
        let backups: Vec<_> = fs::read_dir(&dir)
            .unwrap()
            .filter_map(|e| e.ok())
            .filter(|e| e.file_name().to_string_lossy().contains(".dutyon-backup-"))
            .collect();
        assert_eq!(backups.len(), 1);

        let _ = fs::remove_dir_all(&dir);
    }

    /// Cursor's ~/.cursor/hooks.json uses a FLAT schema (event ->
    /// [{command, timeout}]) with per-event commands and version: 1. Merge
    /// must produce that shape, bake the event into each command, set
    /// loop_limit to null (lifts Cursor's stop-event invocation cap), and be
    /// idempotent while preserving third-party flat entries.
    #[test]
    fn cursor_flat_merge_shape_and_idempotent() {
        let dir = std::env::temp_dir().join("duty-on-cursor-merge-test");
        let _ = fs::create_dir_all(&dir);
        let path = dir.join("hooks.json");
        let original = r#"{
            "version": 1,
            "hooks": {
                "afterFileEdit": [{ "command": ".cursor/hooks/format.sh" }]
            }
        }"#;
        fs::write(&path, original).unwrap();

        // The command must carry the pet marker (.dutyon) or dedup won't
        // recognize it on re-install.
        let cmd = |ev: &str| {
            format!(
                "powershell -File \"$env:USERPROFILE\\.dutyon\\hooks\\trae-hook-bridge.ps1\" -Ide cursor -HookEvent {}",
                ev
            )
        };
        merge_hooks_into_file(
            &path,
            config::CURSOR_HOOK_EVENTS,
            &cmd,
            None,
            true,
            false,
            HookFormat::Flat,
        )
        .unwrap();
        // Re-install must not stack duplicates flat entries.
        merge_hooks_into_file(
            &path,
            config::CURSOR_HOOK_EVENTS,
            &cmd,
            None,
            true,
            false,
            HookFormat::Flat,
        )
        .unwrap();

        let v: Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        assert_eq!(v["version"], 1);
        // Third-party flat entry untouched.
        assert_eq!(v["hooks"]["afterFileEdit"][0]["command"], ".cursor/hooks/format.sh");
        for ev in config::CURSOR_HOOK_EVENTS {
            let arr = v["hooks"][ev].as_array().expect(ev);
            assert_eq!(arr.len(), 1, "expected exactly 1 flat entry for {}", ev);
            let entry = &arr[0];
            // Flat shape: command/timeout/loop_limit at the top level, no
            // nested "hooks" array.
            assert!(entry.get("hooks").is_none());
            assert_eq!(entry["timeout"], config::BRIDGE_TIMEOUT_SEC);
            assert!(entry["loop_limit"].is_null());
            assert!(
                entry["command"]
                    .as_str()
                    .unwrap()
                    .ends_with(&format!("-HookEvent {}", ev)),
                "event must be baked into the command for {}",
                ev
            );
        }

        let _ = fs::remove_dir_all(&dir);
    }

    /// Codex's ~/.codex/hooks.json uses the same Nested schema as Trae
    /// (PascalCase events, no shell field, no event baking) BUT, unlike Trae,
    /// must NOT carry a top-level `version` field — the official codex CLI
    /// (0.147.0+) rejects it as `unknown field \`version\``, expecting only
    /// `description` or `hooks`. So we pass add_version=false and
    /// strip_version=true: an existing `version` (e.g. left by an older
    /// DutyOn build) must be removed on re-install. Merge must be idempotent
    /// and preserve third-party entries.
    #[test]
    fn codex_nested_merge_shape_and_idempotent() {
        let dir = std::env::temp_dir().join("duty-on-codex-merge-test");
        let _ = fs::create_dir_all(&dir);
        let path = dir.join("hooks.json");
        // Input carries `version: 1` (as older DutyOn builds wrote it) to
        // verify that re-install STRIPS it — codex CLI rejects the field.
        let original = r#"{
            "version": 1,
            "hooks": {
                "PreToolUse": [
                    { "hooks": [{ "type": "command", "command": "echo other-tool" }] }
                ]
            }
        }"#;
        fs::write(&path, original).unwrap();

        let cmd = r#"powershell -File "$env:USERPROFILE\.dutyon\hooks\trae-hook-bridge.ps1" -Ide codex"#.to_string();
        merge_hooks_into_file(&path, config::CODEX_HOOK_EVENTS, &|_| cmd.clone(), None, false, true, HookFormat::Nested)
            .unwrap();
        // Re-install must not stack duplicates.
        merge_hooks_into_file(&path, config::CODEX_HOOK_EVENTS, &|_| cmd.clone(), None, false, true, HookFormat::Nested)
            .unwrap();

        let v: Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        // No version field — codex CLI rejects it as an unknown field.
        assert!(v.get("version").is_none());
        // Third-party nested entry untouched.
        assert_eq!(v["hooks"]["PreToolUse"][0]["hooks"][0]["command"], "echo other-tool");
        for ev in config::CODEX_HOOK_EVENTS {
            let arr = v["hooks"][ev].as_array().expect(ev);
            // PreToolUse has the third-party entry + our entry = 2; others = 1.
            let expected = if *ev == "PreToolUse" { 2 } else { 1 };
            assert_eq!(arr.len(), expected, "expected {} entries for {}", expected, ev);
            // Our entry is the last one (pet marker in command).
            let our_entry = &arr[arr.len() - 1];
            assert_eq!(our_entry["hooks"][0]["type"], "command");
            assert_eq!(our_entry["hooks"][0]["timeout"], config::BRIDGE_TIMEOUT_SEC);
            // No shell field for Codex (same as Trae).
            assert!(our_entry["hooks"][0].get("shell").is_none());
            // No event baking (unlike Cursor) — command ends with -Ide codex.
            assert!(
                our_entry["hooks"][0]["command"]
                    .as_str()
                    .unwrap()
                    .contains("-Ide codex"),
                "command must carry -Ide codex for {}",
                ev
            );
        }

        let _ = fs::remove_dir_all(&dir);
    }

    /// OpenCode integration is a JS plugin file (no config merge). The source
    /// must carry the DutyOn marker, reference every canonical event we map,
    /// and write through idempotently (re-install overwrites with the same
    /// bytes, and `is_installed`'s marker check recognises it).
    #[test]
    fn opencode_plugin_source_has_marker_and_writes_idempotent() {
        // Marker present (used by is_installed).
        assert!(OPENCODE_PLUGIN_SOURCE.contains(OPENCODE_PLUGIN_MARKER));
        // Every canonical event we advertise is emitted somewhere in the source.
        for ev in config::OPENCODE_HOOK_EVENTS {
            assert!(
                OPENCODE_PLUGIN_SOURCE.contains(ev),
                "plugin source missing canonical event {}",
                ev
            );
        }
        // Targets the pet's HTTP server and tags the IDE.
        assert!(OPENCODE_PLUGIN_SOURCE.contains("127.0.0.1:17521"));
        assert!(OPENCODE_PLUGIN_SOURCE.contains("'opencode'"));

        let dir = std::env::temp_dir().join("duty-on-opencode-plugin-test");
        let _ = fs::create_dir_all(&dir);
        let path = dir.join(config::OPENCODE_PLUGIN_FILENAME);
        // Write twice — second install must overwrite cleanly (same bytes).
        fs::write(&path, OPENCODE_PLUGIN_SOURCE).unwrap();
        let first = fs::read_to_string(&path).unwrap();
        fs::write(&path, OPENCODE_PLUGIN_SOURCE).unwrap();
        let second = fs::read_to_string(&path).unwrap();
        assert_eq!(first, second);
        assert!(second.contains(OPENCODE_PLUGIN_MARKER));

        let _ = fs::remove_dir_all(&dir);
    }
}
