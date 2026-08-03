//! Tauri commands — IPC handlers backed by Rust. Each `#[tauri::command]`
//! maps 1:1 to a `window.petAPI` method exposed to the renderer via
//! `frontend/tauri-bridge.js`.

use crate::click_through::{ClickRegion, ClickThroughState};
use crate::config;
use crate::hooks_installer;
use crate::ide_scanner;
use crate::models;
use crate::state_manager::{HookEvent, SharedStateManager};
use crate::user_config;
use serde_json::{json, Value};
use std::path::PathBuf;
use std::sync::atomic::Ordering;
use tauri::{AppHandle, Emitter, Manager, State, WebviewWindow};
use tauri_plugin_autostart::ManagerExt;

// ===== State =====

/// Pull-mode state fetch. The scanner's first state-update is emitted while
/// the webview is still loading (when an IDE was opened before the pet), so
/// the renderer pulls the current snapshot once at startup — otherwise those
/// IDE windows stay invisible until the next state change.
#[tauri::command]
pub async fn get_state(state: State<'_, SharedStateManager>) -> Result<Value, String> {
    let s = state.lock().await;
    serde_json::to_value(s.get_snapshot()).map_err(|e| e.to_string())
}

// ===== Hooks =====

#[tauri::command]
pub fn install_hooks(app: AppHandle) -> Result<Value, String> {
    let hooks_dir = resolve_hooks_source_dir(&app);
    // Silent install: writes the bridge script and merges hook configs. No
    // longer opens hooks.json in an editor — the renderer shows the result
    // in-pet instead ("written, enable in IDE" vs "already active").
    let result = hooks_installer::install(&hooks_dir);
    Ok(serde_json::to_value(result).unwrap_or(json!({ "success": false })))
}

#[tauri::command]
pub fn is_hooks_installed() -> Result<Value, String> {
    Ok(serde_json::to_value(hooks_installer::is_installed()).unwrap_or(json!({ "installed": false })))
}

/// Resolve the bundled hooks directory. Tries the Tauri resource dir first
/// (packaged — the bundler may place `resources/hooks/**` at either
/// `<resource>/hooks/` or `<resource>/resources/hooks/`), then the dev-time
/// source path.
fn resolve_hooks_source_dir(app: &AppHandle) -> PathBuf {
    let bridge_name = if cfg!(windows) {
        "trae-hook-bridge.ps1"
    } else {
        "trae-hook-bridge.sh"
    };
    let mut candidates: Vec<PathBuf> = Vec::new();
    if let Ok(resource_dir) = app.path().resource_dir() {
        candidates.push(resource_dir.join("hooks"));
        candidates.push(resource_dir.join("resources").join("hooks"));
        candidates.push(resource_dir);
    }
    // Dev-time fallback only (env! bakes the build machine's path into the
    // binary, so it never hits on user machines — packaged candidates above
    // must take priority).
    candidates.push(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("..").join("hooks"));
    for c in &candidates {
        if c.join(bridge_name).exists() {
            return c.clone();
        }
    }
    // Fall back to the last candidate so install() returns a clear error.
    candidates.pop().unwrap_or_default()
}

// ===== Models =====

#[tauri::command]
pub fn get_models() -> Result<Value, String> {
    let (models, current) = models::get_models();
    Ok(json!({ "models": models, "currentModelUrl": current }))
}

#[tauri::command]
pub fn switch_model(model_url: String) {
    user_config::update(|cfg| cfg.model_url = Some(model_url));
}

// ===== Live2D user models =====

/// Open the user Live2D model folder in the OS file manager, creating it
/// (plus a multilingual README) on first use.
#[tauri::command]
pub fn open_live2d_folder() -> Result<(), String> {
    let dir = models::user_models_dir();
    std::fs::create_dir_all(&dir)
        .map_err(|e| format!("Failed to create {}: {}", dir.display(), e))?;
    let readme = dir.join("README.txt");
    if !readme.exists() {
        std::fs::write(&readme, models::USER_MODELS_README)
            .map_err(|e| format!("Failed to write README: {}", e))?;
    }
    open_in_file_manager(&dir)
}

/// Open a directory in the OS file manager. Windows `explorer` returns a
/// non-zero exit code even on success, so spawn-and-forget everywhere.
#[cfg(windows)]
fn open_in_file_manager(dir: &std::path::Path) -> Result<(), String> {
    std::process::Command::new("explorer")
        .arg(dir)
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

#[cfg(target_os = "macos")]
fn open_in_file_manager(dir: &std::path::Path) -> Result<(), String> {
    std::process::Command::new("open")
        .arg(dir)
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

#[cfg(target_os = "linux")]
fn open_in_file_manager(dir: &std::path::Path) -> Result<(), String> {
    std::process::Command::new("xdg-open")
        .arg(dir)
        .spawn()
        .map(|_| ())
        .map_err(|e| e.to_string())
}

#[cfg(not(any(windows, target_os = "macos", target_os = "linux")))]
fn open_in_file_manager(_dir: &std::path::Path) -> Result<(), String> {
    Err("opening a file manager is not supported on this platform".to_string())
}

// ===== Per-state motions =====

#[tauri::command]
pub fn get_state_motions() -> Result<Value, String> {
    Ok(user_config::load().state_motions.unwrap_or(json!({})))
}

#[tauri::command]
pub fn set_state_motions(motions: Value) {
    user_config::update(|cfg| cfg.state_motions = Some(motions));
}

// ===== Appearance =====

#[tauri::command]
pub fn get_appearance() -> Result<Value, String> {
    let cfg = user_config::load();
    let flip = cfg.flip_horizontal.unwrap_or(false);
    let mini = cfg.mini_mode.unwrap_or(false);
    Ok(json!({ "flipHorizontal": flip, "miniMode": mini }))
}

#[tauri::command]
pub fn set_flip_horizontal(enabled: bool) {
    user_config::update(|cfg| cfg.flip_horizontal = Some(enabled));
}

/// Toggle mini mode (half-size window). Persists the choice, resizes the
/// window, and re-anchors it at bottom-center so the pet stays planted
/// instead of jumping relative to its top-left corner.
#[tauri::command]
pub fn set_mini_mode(window: tauri::Window, enabled: bool) -> Result<(), String> {
    user_config::update(|cfg| cfg.mini_mode = Some(enabled));
    let (w, h) = if enabled {
        (config::MINI_WINDOW_WIDTH, config::MINI_WINDOW_HEIGHT)
    } else {
        (config::WINDOW_WIDTH, config::WINDOW_HEIGHT)
    };
    let factor = window.scale_factor().unwrap_or(1.0);
    let anchor = window
        .outer_position()
        .ok()
        .zip(window.outer_size().ok())
        .map(|(pos, size)| {
            let new_w = w as f64 * factor;
            let new_h = h as f64 * factor;
            (
                (pos.x as f64 + (size.width as f64 - new_w) / 2.0).round() as i32,
                (pos.y as f64 + (size.height as f64 - new_h)).round() as i32,
            )
        });
    window
        .set_size(tauri::Size::Logical(tauri::LogicalSize::new(
            w as f64, h as f64,
        )))
        .map_err(|e| e.to_string())?;
    if let Some((x, y)) = anchor {
        let _ = window.set_position(tauri::Position::Physical(
            tauri::PhysicalPosition::new(x, y),
        ));
    }
    Ok(())
}

// ===== Language =====

#[tauri::command]
pub fn get_language() -> Result<Value, String> {
    let lang = if let Some(lang) = user_config::load().language {
        lang
    } else {
        // Fall back to the OS locale on first launch.
        sys_locale::get_locale().unwrap_or_else(|| "en".to_string())
    };
    Ok(json!(lang))
}

#[tauri::command]
pub fn set_language(lang: String) {
    user_config::update(|cfg| cfg.language = Some(lang));
}

// ===== Auto-launch (start on boot) =====

#[tauri::command]
pub fn get_auto_launch(app: AppHandle) -> bool {
    app.autolaunch().is_enabled().unwrap_or(false)
}

#[tauri::command]
pub fn set_auto_launch(app: AppHandle, enabled: bool) {
    let mgr = app.autolaunch();
    let _ = if enabled { mgr.enable() } else { mgr.disable() };
}

// ===== Test alert =====

#[tauri::command]
pub async fn test_alert(state: State<'_, SharedStateManager>) -> Result<(), String> {
    let test_id = "__duty-on-test-alert__".to_string();
    let event = HookEvent {
        session_id: test_id.clone(),
        hook_event_name: "Notification".to_string(),
        project_path: String::new(),
        project_name: "预览提醒".to_string(),
        cwd: String::new(),
        notification_type: Some("permission_request".to_string()),
        ide: None,
        tool_name: None,
        message: Some("预览提醒效果".to_string()),
        timestamp: None,
    };
    {
        let mut sm = state.lock().await;
        sm.handle_hook_event(&event);
    }
    // Auto-clear after 8 seconds.
    let state2 = state.inner().clone();
    tauri::async_runtime::spawn(async move {
        tokio::time::sleep(std::time::Duration::from_secs(8)).await;
        let mut sm = state2.lock().await;
        sm.remove_session(&test_id);
    });
    Ok(())
}

// ===== Window control =====

#[tauri::command]
pub fn drag_window(window: WebviewWindow, delta_x: i32, delta_y: i32) -> Result<(), String> {
    let pos = window.outer_position().map_err(|e| e.to_string())?;
    window
        .set_position(tauri::PhysicalPosition::new(pos.x + delta_x, pos.y + delta_y))
        .map_err(|e| e.to_string())
}

/// Replace the set of "clickable rectangles" (model bounds / status bar /
/// menu) the polling thread hit-tests against. Reported in physical pixels,
/// window-local coords. Called ~8Hz by the renderer's PixiJS ticker.
#[tauri::command]
pub fn update_click_regions(
    state: State<'_, ClickThroughState>,
    regions: Vec<ClickRegion>,
) -> Result<(), String> {
    let mut r = state.regions.lock().map_err(|e| e.to_string())?;
    *r = regions;
    Ok(())
}

/// Force the window to stay clickable regardless of cursor position (used
/// while dragging or while the menu is open). The polling loop honors this
/// flag with priority over the region hit-test.
#[tauri::command]
pub fn set_force_clickable(
    state: State<'_, ClickThroughState>,
    force: bool,
) -> Result<(), String> {
    state.force_clickable.store(force, Ordering::SeqCst);
    Ok(())
}

#[tauri::command]
pub fn bring_to_front(project_path: String) {
    if project_path.is_empty() {
        return;
    }
    let name = std::path::Path::new(&project_path)
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or(&project_path)
        .to_string();
    ide_scanner::focus_project_window(&name);
}

#[tauri::command]
pub fn flash_attention(window: WebviewWindow) {
    // The pet window is skipTaskbar + always-on-top, so taskbar flash is
    // invisible. Emit a 'flash' event the renderer turns into a CSS pulse;
    // also nudge focus as a mild signal.
    let _ = window.emit("flash", ());
    let _ = window.set_focus();
}

#[tauri::command]
pub fn quit(app: AppHandle) {
    app.exit(0);
}

#[tauri::command]
pub fn uninstall_app(app: AppHandle) {
    // In dev, just quit. In packaged mode, the NSIS uninstaller lives next to
    // the exe as "Uninstall DutyOn.exe"; launch it detached, then exit.
    #[cfg(windows)]
    {
        let exe = std::env::current_exe().ok();
        if let Some(exe_path) = exe {
            if let Some(dir) = exe_path.parent() {
                if let Ok(entries) = std::fs::read_dir(dir) {
                    for entry in entries.flatten() {
                        if let Some(name) = entry.file_name().to_str() {
                            if name.to_lowercase().starts_with("uninstall") && name.ends_with(".exe") {
                                use std::os::windows::process::CommandExt;
                                let _ = std::process::Command::new(entry.path())
                                    .creation_flags(0x00000008) // DETACHED_PROCESS
                                    .spawn();
                                app.exit(0);
                                return;
                            }
                        }
                    }
                }
            }
        }
    }
    #[cfg(not(windows))]
    {
        // macOS/Linux have no unified uninstaller convention. Quit the app; the
        // user removes the .app bundle (macOS) or uninstalls the package (Linux).
        log::info!("[uninstall] non-Windows: quitting; remove the app bundle/package manually");
    }
    app.exit(0);
}
