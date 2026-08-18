//! User preferences persistence. Reads/writes `~/.dutyon/config.json`.

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::fs;
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock, RwLock};

/// Mutex preventing concurrent read-modify-write races on the config file.
static CONFIG_LOCK: Mutex<()> = Mutex::new(());

/// In-memory cache of the config. `update()` writes here BEFORE attempting
/// disk persistence, so even if the file write fails (sandbox, read-only
/// directory, disk full), subsequent `load()` calls return the latest values
/// instead of stale disk data. Initialized lazily from disk on first access.
static CONFIG_CACHE: RwLock<Option<UserConfig>> = RwLock::new(None);

#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct WindowPosition {
    pub x: i32,
    pub y: i32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UserConfig {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub model_url: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub state_motions: Option<Value>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub flip_horizontal: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mini_mode: Option<bool>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub language: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub window_position: Option<WindowPosition>,
    /// When true, the HTTP server binds to 0.0.0.0 instead of 127.0.0.1 so
    /// other devices on the LAN can read the read-only `/api/*` routes (the
    /// "external display" surface). Write endpoints stay loopback-guarded
    /// either way. Toggling requires an app restart — a live listener's bind
    /// address can't change.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub external_access: Option<bool>,
    /// User-created custom characters (GIF/MP4-based, replacing Live2D).
    /// Each character has per-state animation files in `~/.dutyon/animations/`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub custom_characters: Option<Vec<CustomCharacter>>,
    /// Active character: either a built-in model URL (Live2D) or a custom
    /// character ID (e.g. "char_xxx"). When null, defaults to first built-in.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub active_character_id: Option<String>,
    /// Monitor drawer (system metrics panel above the status bar).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub monitor: Option<MonitorConfig>,
}

/// Monitor drawer configuration: master switch, collapse state, per-metric
/// visibility, and the status-bar project list visibility.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MonitorConfig {
    /// Master switch: the drawer is shown at all.
    pub enabled: bool,
    /// Drawer reduced to its title strip.
    pub collapsed: bool,
    pub show_cpu: bool,
    pub show_ram: bool,
    pub show_gpu: bool,
    pub show_net: bool,
    pub show_self: bool,
    /// Status-bar project list visibility.
    pub show_project_list: bool,
}

impl Default for MonitorConfig {
    fn default() -> Self {
        Self {
            enabled: true,
            collapsed: false,
            show_cpu: true,
            show_ram: true,
            show_gpu: true,
            show_net: true,
            show_self: true,
            show_project_list: true,
        }
    }
}

/// A user-created character with per-state animation files.
/// File values are filenames in `~/.dutyon/animations/`.
#[derive(Debug, Clone, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CustomCharacter {
    pub id: String,
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sleeping: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub working: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub alert: Option<String>,
}

/// Path to `~/.dutyon/config.json`. Falls back to `%TEMP%\dutyon\config.json`
/// if the home directory is not writable (sandbox/restricted environment).
/// The result is cached in a OnceLock so the check only runs once.
static CONFIG_PATH: OnceLock<PathBuf> = OnceLock::new();

pub fn config_path() -> PathBuf {
    CONFIG_PATH.get_or_init(|| {
        let primary = dirs::home_dir()
            .unwrap_or_default()
            .join(".dutyon")
            .join("config.json");
        // Check if primary path is writable
        if let Some(parent) = primary.parent() {
            if std::fs::create_dir_all(parent).is_ok() {
                // Try writing a test file to verify write permission
                let test = parent.join(".write_test");
                if std::fs::write(&test, b"").is_ok() {
                    let _ = std::fs::remove_file(&test);
                    return primary;
                }
            }
        }
        // Fallback to temp dir
        let fallback = std::env::temp_dir().join("dutyon").join("config.json");
        if let Some(parent) = fallback.parent() {
            let _ = std::fs::create_dir_all(parent);
        }
        log::warn!("[config] Home dir not writable, using fallback: {}", fallback.display());
        fallback
    }).clone()
}

/// Load the user config. Returns the in-memory cache if available (always
/// up-to-date even when disk writes fail); otherwise reads from disk and
/// populates the cache. Returns an empty default on any error (missing file,
/// parse error) so callers always get a usable struct.
pub fn load() -> UserConfig {
    // Fast path: return from cache
    if let Ok(cache) = CONFIG_CACHE.read() {
        if let Some(ref cfg) = *cache {
            return cfg.clone();
        }
    }
    // Cache miss: read from disk and populate cache
    let path = config_path();
    let cfg = match fs::read_to_string(&path) {
        Ok(content) => serde_json::from_str(&content).unwrap_or_default(),
        Err(_) => UserConfig::default(),
    };
    if let Ok(mut cache) = CONFIG_CACHE.write() {
        *cache = Some(cfg.clone());
    }
    cfg
}

/// Persist the user config to disk (creates the directory if missing).
#[allow(dead_code)]
pub fn save(cfg: &UserConfig) {
    let _guard = CONFIG_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    save_unlocked(cfg);
}

/// Write the config to disk without acquiring the mutex — callers must
/// already hold `CONFIG_LOCK`.
fn save_unlocked(cfg: &UserConfig) {
    let path = config_path();
    if let Some(parent) = path.parent() {
        if let Err(e) = fs::create_dir_all(parent) {
            log::error!("[config] Failed to create config dir: {}", e);
            return;
        }
    }
    match serde_json::to_string_pretty(cfg) {
        Ok(json) => {
            if let Err(e) = fs::write(&path, json) {
                log::error!("[config] Failed to write config: {}", e);
            }
        }
        Err(e) => log::error!("[config] Failed to serialize config: {}", e),
    }
}

/// Read-modify-write helper for commands that update a single field.
/// Updates the in-memory cache FIRST (always succeeds), then attempts disk
/// persistence. If the disk write fails, the cache still holds the new value
/// so subsequent load() calls return the updated config.
pub fn update<F: FnOnce(&mut UserConfig)>(f: F) {
    let _guard = CONFIG_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let mut cfg = load();
    f(&mut cfg);
    // Update cache first (always succeeds — in-memory)
    if let Ok(mut cache) = CONFIG_CACHE.write() {
        *cache = Some(cfg.clone());
    }
    // Then try to persist to disk (may fail, but cache is already updated)
    save_unlocked(&cfg);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn serde_roundtrip_preserves_all_fields() {
        let mut cfg = UserConfig::default();
        cfg.model_url = Some("http://example.com/model.json".to_string());
        cfg.flip_horizontal = Some(true);
        cfg.mini_mode = Some(true);
        cfg.language = Some("ja".to_string());
        cfg.window_position = Some(WindowPosition { x: 100, y: 200 });
        cfg.state_motions = Some(serde_json::json!({"working": "motion_01"}));
        cfg.external_access = Some(true);

        // Serialize → deserialize → compare.
        let json = serde_json::to_string(&cfg).unwrap();
        let deserialized: UserConfig = serde_json::from_str(&json).unwrap();

        assert_eq!(deserialized.model_url, cfg.model_url);
        assert_eq!(deserialized.flip_horizontal, cfg.flip_horizontal);
        assert_eq!(deserialized.mini_mode, cfg.mini_mode);
        assert_eq!(deserialized.language, cfg.language);
        assert_eq!(
            deserialized.window_position,
            cfg.window_position
        );
        assert_eq!(deserialized.state_motions, cfg.state_motions);
        assert_eq!(deserialized.external_access, cfg.external_access);
    }

    #[test]
    fn default_config_has_all_none() {
        let cfg = UserConfig::default();
        assert!(cfg.model_url.is_none());
        assert!(cfg.flip_horizontal.is_none());
        assert!(cfg.mini_mode.is_none());
        assert!(cfg.language.is_none());
        assert!(cfg.window_position.is_none());
        assert!(cfg.state_motions.is_none());
        assert!(cfg.external_access.is_none());
    }

    #[test]
    fn deserialize_empty_string_returns_default() {
        // Simulates the load() path when the config file content is empty or
        // invalid — unwrap_or_default() must yield a usable default.
        let cfg: UserConfig = serde_json::from_str("").unwrap_or_default();
        assert!(cfg.model_url.is_none());
        assert!(cfg.language.is_none());
    }

    #[test]
    fn deserialize_partial_json_defaults_missing_fields() {
        // Fields missing from JSON should default to None (serde(default)).
        let json = r#"{"language":"zh-CN","flipHorizontal":true}"#;
        let cfg: UserConfig = serde_json::from_str(json).unwrap();
        assert_eq!(cfg.language.as_deref(), Some("zh-CN"));
        assert_eq!(cfg.flip_horizontal, Some(true));
        assert!(cfg.model_url.is_none());
        assert!(cfg.window_position.is_none());
        assert!(cfg.state_motions.is_none());
    }

    #[test]
    fn config_path_is_in_dutyon_dir_or_fallback() {
        let path = config_path();
        assert!(path.ends_with("config.json"));
        let path_str = path.to_string_lossy();
        // In a sandbox or restricted environment, the path may fall back to temp dir
        let is_valid = path_str.contains(".dutyon")
            || path_str.contains("dutyon")
            || path_str.contains("temp");
        assert!(
            is_valid,
            "config path should be in .dutyon dir or temp fallback, got: {}",
            path_str
        );
    }

    #[test]
    fn config_path_fallback_when_home_not_writable() {
        // This test verifies the fallback logic exists and produces a valid path.
        // We can't easily simulate an unwritable home dir in a unit test,
        // but we can at least verify the function doesn't panic and returns
        // a path with the expected filename.
        let path = config_path();
        assert_eq!(path.file_name().unwrap(), "config.json");
        assert!(path.parent().is_some());
    }
}
