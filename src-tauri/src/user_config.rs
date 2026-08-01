//! User preferences persistence — port of loadUserConfig/saveUserConfig in
//! `src/main/index.js`. Reads/writes `~/.dutyon/config.json`.

use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::fs;
use std::path::PathBuf;

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
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
    pub language: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub window_position: Option<WindowPosition>,
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
pub fn save(cfg: &UserConfig) {
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
    let mut cfg = load();
    f(&mut cfg);
    save(&cfg);
}
