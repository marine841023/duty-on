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
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
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

// ===== Edge dock (screen-edge snap) =====

/// While docked: the pre-snap window rect (physical px) plus the docked
/// edge, so `exit_edge_dock` can restore the size and pull the window away
/// from the right edge. None when not docked. `active` is a cheap flag read
/// by the window-position persistence handler (docked positions are not
/// persisted). Cloneable via Arc so lib.rs can hold a copy.
#[derive(Default, Clone)]
pub struct EdgeDockState {
    pub docked: Arc<Mutex<Option<((i32, i32, u32, u32), String)>>>,
    pub active: Arc<AtomicBool>,
}

/// Pick the monitor that contains the LARGEST AREA of the given rect — the
/// "most content wins" rule for a window straddling two screens. Fallback:
/// the monitor containing the rect center, then the first monitor.
fn best_monitor<'a>(
    monitors: &'a [tauri::Monitor],
    pos: tauri::PhysicalPosition<i32>,
    size: tauri::PhysicalSize<u32>,
) -> Option<&'a tauri::Monitor> {
    let wl = pos.x;
    let wt = pos.y;
    let wr = pos.x + size.width as i32;
    let wb = pos.y + size.height as i32;
    let mut best: Option<(&'a tauri::Monitor, i64)> = None;
    for m in monitors {
        let p = m.position();
        let s = m.size();
        let ox = (wr.min(p.x + s.width as i32) - wl.max(p.x)).max(0) as i64;
        let oy = (wb.min(p.y + s.height as i32) - wt.max(p.y)).max(0) as i64;
        let area = ox * oy;
        if area > 0 && best.map_or(true, |(_, a)| area > a) {
            best = Some((m, area));
        }
    }
    if let Some((m, _)) = best {
        return Some(m);
    }
    let cx = pos.x + size.width as i32 / 2;
    let cy = pos.y + size.height as i32 / 2;
    monitors
        .iter()
        .find(|m| {
            let p = m.position();
            let s = m.size();
            cx >= p.x && cx < p.x + s.width as i32 && cy >= p.y && cy < p.y + s.height as i32
        })
        .or_else(|| monitors.first())
}

/// Check whether the window has crossed the left/right edge of its monitor
/// by more than 20% of its width — the ONLY condition that triggers docking;
/// every other position (including resting right at the edge) does not snap.
/// On multi-monitor setups EVERY monitor boundary is a valid snap target,
/// including internal boundaries shared with a neighbouring monitor; when the
/// window straddles two screens, the monitor holding most of the window wins
/// and its crossed edge is the snap target.
/// Returns the nearest crossed edge ("left" | "right"). Top/bottom edges are
/// intentionally not supported. Called by the renderer on drag-end (after
/// flushing the last drag delta).
#[tauri::command]
pub fn detect_edge_dock(
    window: WebviewWindow,
    app: AppHandle,
    state: State<'_, EdgeDockState>,
) -> Result<Option<String>, String> {
    if state.active.load(Ordering::SeqCst) {
        return Ok(None); // already docked
    }
    let pos = window.outer_position().map_err(|e| e.to_string())?;
    let size = window.outer_size().map_err(|e| e.to_string())?;

    // Monitor holding most of the window — when straddling two screens this
    // is the one whose edge gets the snap.
    let monitors = app.available_monitors().map_err(|e| e.to_string())?;
    let mon = best_monitor(&monitors, pos, size)
        .ok_or_else(|| "no monitors found".to_string())?;
    let mp = mon.position();
    let ms = mon.size();

    // How far the window has crossed PAST each edge of that monitor
    // (physical px): >0 means part of the window is beyond the edge — either
    // off-screen (outer edges) or on a neighbouring monitor (internal
    // boundaries count too). Docking fires only when the crossed amount
    // exceeds 20% of the window width.
    let cross_left = mp.x - pos.x;
    let cross_right = (pos.x + size.width as i32) - (mp.x + ms.width as i32);
    let min_cross = (size.width as f64 * 0.2).round() as i32;
    let edge = match (cross_left > min_cross, cross_right > min_cross) {
        (true, true) => {
            if cross_left >= cross_right {
                "left"
            } else {
                "right"
            }
        }
        (true, false) => "left",
        (false, true) => "right",
        (false, false) => return Ok(None),
    };
    Ok(Some(edge.to_string()))
}

/// Snap the window into a compact bar at the given left/right edge. The bar
/// is EDGE_DOCK_THICKNESS wide and as tall as its content (the renderer
/// passes `content_height` in logical px, clamped to sane bounds), vertically
/// centered on where the window was dropped. Remembers the previous rect and
/// edge for `exit_edge_dock`.
#[tauri::command]
pub fn enter_edge_dock(
    window: WebviewWindow,
    app: AppHandle,
    state: State<'_, EdgeDockState>,
    edge: String,
    content_height: u32,
) -> Result<(), String> {
    if edge != "left" && edge != "right" {
        return Err("only left/right edge docking is supported".to_string());
    }
    let pos = window.outer_position().map_err(|e| e.to_string())?;
    let size = window.outer_size().map_err(|e| e.to_string())?;
    let factor = window.scale_factor().unwrap_or(1.0);

    let monitors = app.available_monitors().map_err(|e| e.to_string())?;
    let cy = pos.y + size.height as i32 / 2;
    // Same "most content wins" selection as detect_edge_dock so the bar is
    // placed on the edge that actually triggered the snap.
    let mon = best_monitor(&monitors, pos, size)
        .ok_or_else(|| "no monitors found".to_string())?;
    let mp = mon.position();
    let ms = mon.size();

    let w = (config::EDGE_DOCK_THICKNESS as f64 * factor).round() as u32;
    let min_h = (80.0 * factor).round() as u32;
    let max_h = ms.height.saturating_sub((16.0 * factor) as u32).max(min_h);
    let h = ((content_height as f64 * factor).round() as u32).clamp(min_h, max_h);
    let x = if edge == "left" {
        mp.x
    } else {
        mp.x + ms.width as i32 - w as i32
    };
    // Vertically center on the drop position, clamped inside the monitor.
    let margin = (8.0 * factor).round() as i32;
    let y_min = mp.y + margin;
    let y_max = (mp.y + ms.height as i32 - h as i32 - margin).max(y_min);
    let y = (cy - h as i32 / 2).clamp(y_min, y_max);

    *state.docked.lock().unwrap() =
        Some(((pos.x, pos.y, size.width, size.height), edge.clone()));
    window
        .set_size(tauri::Size::Physical(tauri::PhysicalSize::new(w, h)))
        .map_err(|e| e.to_string())?;
    window
        .set_position(tauri::Position::Physical(tauri::PhysicalPosition::new(x, y)))
        .map_err(|e| e.to_string())?;
    state.active.store(true, Ordering::SeqCst);
    Ok(())
}

/// Leave edge-dock mode: restore the pre-snap size at the current (docked)
/// position, pulled one threshold away from the edge so the drag can continue
/// seamlessly and `detect_edge_dock` won't immediately re-snap on release.
#[tauri::command]
pub fn exit_edge_dock(
    window: WebviewWindow,
    state: State<'_, EdgeDockState>,
) -> Result<(), String> {
    let docked = state.docked.lock().unwrap().take();
    state.active.store(false, Ordering::SeqCst);
    let prev_size = docked.as_ref().map(|(r, _)| (r.2, r.3));
    let edge = docked.map(|(_, e)| e);
    let factor = window.scale_factor().unwrap_or(1.0);

    // Restore the pre-dock size; fall back to the configured normal/mini size
    // (scaled to the current monitor) if nothing was saved.
    let (w, h) = match prev_size {
        Some(s) => s,
        None => {
            let mini = user_config::load().mini_mode.unwrap_or(false);
            let (lw, lh) = if mini {
                (config::MINI_WINDOW_WIDTH, config::MINI_WINDOW_HEIGHT)
            } else {
                (config::WINDOW_WIDTH, config::WINDOW_HEIGHT)
            };
            ((lw as f64 * factor) as u32, (lh as f64 * factor) as u32)
        }
    };
    window
        .set_size(tauri::Size::Physical(tauri::PhysicalSize::new(w, h)))
        .map_err(|e| e.to_string())?;

    let pos = window.outer_position().map_err(|e| e.to_string())?;
    let offset = (config::EDGE_SNAP_THRESHOLD as f64 * factor).round() as i32;
    let (mut x, y) = (pos.x, pos.y);
    match edge.as_deref() {
        Some("left") => x += offset,
        Some("right") => x -= offset,
        _ => x += offset,
    }
    window
        .set_position(tauri::Position::Physical(tauri::PhysicalPosition::new(x, y)))
        .map_err(|e| e.to_string())
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

/// Tracks the temporary horizontal growth of the window that makes room for
/// the context menu beside the character. `active` lets the position-persist
/// handler skip the shifted geometry; `grow` remembers (side, physical delta)
/// so `close_menu_space` restores exactly what was applied.
#[derive(Default, Clone)]
pub struct MenuSpaceState {
    pub active: Arc<AtomicBool>,
    pub grow: Arc<Mutex<Option<(String, i32)>>>,
}

#[derive(serde::Serialize)]
pub struct MenuSpaceInfo {
    pub side: String,
    pub delta: u32, // logical px actually gained
}

/// Grow the window horizontally to make room for the context menu. The side
/// is chosen by the pet's position on its monitor — pet on the left half
/// grows rightward, pet on the right half grows leftward (menu opens toward
/// the free space). The top-left corner is kept fixed unless growing
/// leftward (then the window shifts left by the same amount, so the renderer
/// offsets its content to keep the pet in place). If the chosen side lacks
/// room on the monitor, the other side is used instead. Returns the side and
/// width actually gained (clamped to the monitor bounds).
#[tauri::command]
pub fn open_menu_space(
    window: WebviewWindow,
    app: AppHandle,
    state: State<'_, MenuSpaceState>,
    width: u32,
) -> Result<MenuSpaceInfo, String> {
    let factor = window.scale_factor().unwrap_or(1.0);
    let pos = window.outer_position().map_err(|e| e.to_string())?;
    let size = window.outer_size().map_err(|e| e.to_string())?;
    let dw = (width as f64 * factor).round() as i32;

    // Monitor containing the window center (fallback: first).
    let monitors = app.available_monitors().map_err(|e| e.to_string())?;
    let cx = pos.x + size.width as i32 / 2;
    let cy = pos.y + size.height as i32 / 2;
    let mon = monitors
        .iter()
        .find(|m| {
            let p = m.position();
            let s = m.size();
            cx >= p.x && cx < p.x + s.width as i32 && cy >= p.y && cy < p.y + s.height as i32
        })
        .or_else(|| monitors.first())
        .ok_or_else(|| "no monitors found".to_string())?;
    let mp = mon.position();
    let ms = mon.size();
    let margin = (8.0 * factor).round() as i32;

    // Pick the side by the pet's position: on the left half of its monitor
    // the menu opens to the right (toward the free space) and vice versa;
    // flip when the chosen side is cramped and the opposite side has more room.
    let room_left = pos.x - mp.x;
    let room_right = (mp.x + ms.width as i32 - margin) - (pos.x + size.width as i32);
    let pet_center_x = pos.x + size.width as i32 / 2;
    let mon_center_x = mp.x + ms.width as i32 / 2;
    let mut side = if pet_center_x <= mon_center_x {
        "right"
    } else {
        "left"
    };
    let (room_pref, room_alt) = if side == "left" {
        (room_left, room_right)
    } else {
        (room_right, room_left)
    };
    if room_pref < dw && room_alt > room_pref {
        side = if side == "left" { "right" } else { "left" };
    }
    let room = if side == "left" { room_left } else { room_right };
    let delta = dw.clamp(0, room.max(0));

    let mut x = pos.x;
    let mut w = size.width as i32;
    if delta > 0 {
        if side == "left" {
            x -= delta;
        }
        w += delta;
        window
            .set_size(tauri::Size::Physical(tauri::PhysicalSize::new(
                w.max(80) as u32,
                size.height,
            )))
            .map_err(|e| e.to_string())?;
        window
            .set_position(tauri::PhysicalPosition::new(x, pos.y))
            .map_err(|e| e.to_string())?;
    }

    *state.grow.lock().unwrap() = Some((side.to_string(), delta));
    state.active.store(true, Ordering::SeqCst);
    Ok(MenuSpaceInfo {
        side: side.to_string(),
        delta: (delta as f64 / factor).round() as u32,
    })
}

/// Undo the growth applied by `open_menu_space` (restore width, and shift
/// back rightward when the growth was on the left).
#[tauri::command]
pub fn close_menu_space(
    window: WebviewWindow,
    state: State<'_, MenuSpaceState>,
) -> Result<(), String> {
    let taken = state.grow.lock().unwrap().take();
    state.active.store(false, Ordering::SeqCst);
    let Some((side, delta)) = taken else {
        return Ok(());
    };
    if delta <= 0 {
        return Ok(());
    }
    let pos = window.outer_position().map_err(|e| e.to_string())?;
    let size = window.outer_size().map_err(|e| e.to_string())?;
    let mut x = pos.x;
    let w = (size.width as i32 - delta).max(80);
    if side == "left" {
        x += delta;
    }
    window
        .set_size(tauri::Size::Physical(tauri::PhysicalSize::new(
            w as u32,
            size.height,
        )))
        .map_err(|e| e.to_string())?;
    window
        .set_position(tauri::PhysicalPosition::new(x, pos.y))
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
