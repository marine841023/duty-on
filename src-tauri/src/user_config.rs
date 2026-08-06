//! User preferences persistence. Reads/writes `~/.dutyon/config.json`.

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::fs;
use std::path::PathBuf;
use std::sync::Mutex;

/// Mutex preventing concurrent read-modify-write races on the config file.
static CONFIG_LOCK: Mutex<()> = Mutex::new(());

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
}

/// Path to `~/.dutyon/config.json`.
pub fn config_path() -> PathBuf {
    let home = dirs::home_dir().unwrap_or_else(|| PathBuf::from("."));
    home.join(".dutyon").join("config.json")
}

/// Load the user config. Returns an empty default on any error (missing file,
/// parse error) so callers always get a usable struct.
pub fn load() -> UserConfig {
    let path = config_path();
    match fs::read_to_string(&path) {
        Ok(content) => serde_json::from_str(&content).unwrap_or_default(),
        Err(_) => UserConfig::default(),
    }
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
pub fn update<F: FnOnce(&mut UserConfig)>(f: F) {
    let _guard = CONFIG_LOCK.lock().unwrap_or_else(|e| e.into_inner());
    let mut cfg = load();
    f(&mut cfg);
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
    fn config_path_is_in_dutyon_dir() {
        let path = config_path();
        assert!(path.ends_with("config.json"));
        assert!(path.to_string_lossy().contains(".dutyon"));
    }
}
