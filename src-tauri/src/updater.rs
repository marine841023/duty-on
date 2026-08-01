//! Updater — checks a remote server for a newer version and downloads/runs
//! the installer. Replaces `electron-updater`.
//!
//! Status: the server address is not yet provided by the user, so while the
//! URL is empty the updater reports "not-available" silently. Once a URL is
//! configured in `~/.trae-pet/config.json` (`updateServerUrl`), this will
//! fetch `<url>/updates/manifest.json`, compare versions, download the NSIS
//! installer, and run it (overwriting the current install). Network failures
//! surface the configured "无法连接升级服务器…" message.

use crate::user_config;
use serde_json::json;
use tauri::{AppHandle, Emitter};

pub fn check_for_updates(app: &AppHandle) {
    let url = user_config::load().update_server_url.unwrap_or_default();
    if url.trim().is_empty() {
        // No server configured yet — stay silent (per user: address comes later).
        let _ = app.emit("update-status", json!({ "status": "not-available" }));
        return;
    }
    // TODO(stage 7): fetch `<url>/updates/manifest.json`, compare versions,
    // download installer, emit 'downloading'/'downloaded'. Until the real
    // server exists, report not-available so the UI is honest.
    let _ = app.emit("update-status", json!({ "status": "not-available" }));
}

pub fn install_update(app: &AppHandle) {
    let _ = app.emit(
        "update-status",
        json!({ "status": "error", "message": "尚未下载更新" }),
    );
}
