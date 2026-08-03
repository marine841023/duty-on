//! Hook installer for Trae and Qoder IDE integration.
//! Installs bridge scripts to `~/.dutyon/hooks/` and registers hook entries
//! in `~/.trae-cn/hooks.json` and `~/.qoder/settings.json`.

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
    pub needs_enable: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct InstalledStatus {
    pub installed: bool,
    pub hooks_exist: bool,
    pub bridge_exists: bool,
    pub qoder_hooks_exist: bool,
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
/// its hook runner. `$env:USERPROFILE` (PowerShell) on Windows; `bash
/// "$HOME/..."` (sh-compatible) on macOS/Linux — explicit `bash` avoids
/// depending on the executable bit and the `~` expansion quirks of some shells.
/// The `ide` argument ("trae" / "qoder") is forwarded by the bridge so the
/// pet can badge each session with its source IDE.
fn hook_command(ide: &str) -> String {
    if cfg!(windows) {
        format!(r#"& "$env:USERPROFILE\.dutyon\hooks\trae-hook-bridge.ps1" -Ide {}"#, ide)
    } else {
        format!(r#"bash "$HOME/.dutyon/hooks/trae-hook-bridge.sh" {}"#, ide)
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
/// For each event in `events`, removes existing pet groups (to avoid
/// duplicates on re-install) then appends a fresh group. `shell` is written
/// into each hook entry when given (Qoder honors it; Trae ignores the unknown
/// field, so we only set it for Qoder). `add_version` writes a Trae-style
/// `version: 1` top-level field.
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
    hook_command: &str,
    shell: Option<&str>,
    add_version: bool,
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

    if add_version && existing.get("version").is_none() {
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
            let mut group = json!({
                "hooks": [{
                    "type": "command",
                    "command": hook_command,
                    "timeout": config::BRIDGE_TIMEOUT_SEC
                }]
            });
            if let Some(shell) = shell {
                group["hooks"][0]["shell"] = json!(shell);
            }
            let arr = hooks.entry(event.to_string()).or_insert(json!([]));
            if !arr.is_array() {
                // Another tool wrote a non-array value for this event. Keep
                // the original value by wrapping it as the first element
                // instead of dropping it, then append our group.
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
                // Remove existing pet hook groups (avoid duplicates).
                a.retain(|g| {
                    g["hooks"]
                        .as_array()
                        .map(|hs| {
                            !hs.iter().any(|h| {
                                h["command"]
                                    .as_str()
                                    .map(|c| is_pet_command(c))
                                    .unwrap_or(false)
                            })
                        })
                        .unwrap_or(true)
                });
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
        needs_enable: false,
    }
}

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
    let trae_hook_command = hook_command("trae");
    let mut warnings: Vec<String> = Vec::new();
    match merge_hooks_into_file(
        &trae_hooks_path,
        config::HOOK_EVENTS,
        &trae_hook_command,
        None,
        true,
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
        let qoder_hook_command = hook_command("qoder");
        match merge_hooks_into_file(
            &qoder_hooks_path,
            config::QODER_HOOK_EVENTS,
            &qoder_hook_command,
            Some(qoder_shell()),
            false,
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
        needs_enable: true,
    }
}

/// Check whether hooks are already installed. Reports Trae and Qoder
/// independently; `installed` is true when the bridge exists AND at least one
/// IDE has the pet hooks wired.
pub fn is_installed() -> InstalledStatus {
    let home = home();
    let trae_hooks_path = home.join(".trae-cn").join("hooks.json");
    let qoder_hooks_path = home.join(".qoder").join("settings.json");
    let bridge_path = home.join(".dutyon").join("hooks").join(bridge_filename());

    let bridge_exists = bridge_path.exists();

    let trae_hooks_contain_pet = if trae_hooks_path.exists() {
        fs::read_to_string(&trae_hooks_path)
            .map(|c| is_pet_command(&c))
            .unwrap_or(false)
    } else {
        false
    };

    let qoder_hooks_contain_pet = if qoder_hooks_path.exists() {
        fs::read_to_string(&qoder_hooks_path)
            .map(|c| is_pet_command(&c))
            .unwrap_or(false)
    } else {
        false
    };

    InstalledStatus {
        installed: bridge_exists && (trae_hooks_contain_pet || qoder_hooks_contain_pet),
        hooks_exist: trae_hooks_contain_pet,
        bridge_exists,
        qoder_hooks_exist: qoder_hooks_contain_pet,
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

        let cmd = r#"& "$env:USERPROFILE\.dutyon\hooks\trae-hook-bridge.ps1""#;
        merge_hooks_into_file(&path, config::QODER_HOOK_EVENTS, cmd, Some("powershell"), false)
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

        let cmd = r#"& "$env:USERPROFILE\.dutyon\hooks\trae-hook-bridge.ps1""#;
        merge_hooks_into_file(&path, config::HOOK_EVENTS, cmd, None, true).unwrap();

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

        let cmd = r#"bash "$HOME/.dutyon/hooks/trae-hook-bridge.sh""#;
        merge_hooks_into_file(&path, config::QODER_HOOK_EVENTS, cmd, Some("bash"), false).unwrap();
        merge_hooks_into_file(&path, config::QODER_HOOK_EVENTS, cmd, Some("bash"), false).unwrap();

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

        let cmd = r#"bash "$HOME/.dutyon/hooks/trae-hook-bridge.sh""#;
        merge_hooks_into_file(&path, &["Stop"], cmd, Some("bash"), false).unwrap();

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

        let cmd = r#"& "$env:USERPROFILE\.dutyon\hooks\trae-hook-bridge.ps1""#;
        let warning = merge_hooks_into_file(&path, &["Stop"], cmd, None, true).unwrap();
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

        let cmd = r#"& "$env:USERPROFILE\.dutyon\hooks\trae-hook-bridge.ps1""#;
        let warning = merge_hooks_into_file(&path, &["Stop"], cmd, None, true).unwrap();
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

        let cmd = r#"bash "$HOME/.dutyon/hooks/trae-hook-bridge.sh""#;
        let warning = merge_hooks_into_file(&path, &["Stop"], cmd, Some("bash"), false).unwrap();
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

        let cmd = r#"& "$env:USERPROFILE\.dutyon\hooks\trae-hook-bridge.ps1""#;
        let warning = merge_hooks_into_file(&path, &["Stop"], cmd, None, true).unwrap();
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
}
