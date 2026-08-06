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

/// Manually reset all sessions to Idle. Used as a fallback when Trae IDE
/// doesn't fire a Stop hook event (e.g., user aborts during the AI's
/// "thinking" phase — no tool use, so no Stop event). Without this the pet
/// stays stuck in Working until the 3-minute WORKING_TIMEOUT elapses.
#[tauri::command]
pub async fn reset_to_idle(state: State<'_, SharedStateManager>) -> Result<(), String> {
    let mut s = state.lock().await;
    s.reset_all_to_idle();
    Ok(())
}

/// Frontend diagnostic log (via invoke, unlike /log fetch which can be flaky
/// in dev). Writes to the cargo stdout + ~/.dutyon/frontend.log so we can
/// trace menu-timing values (left, screenX, side, delta) that fetch-based
/// __petSendLog drops in some sessions.
#[tauri::command]
pub fn debug_log(msg: String) -> Result<(), String> {
    log::info!("[fe] {}", msg);
    crate::server::append_log_file("info", &msg);
    Ok(())
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

/// Open the user sounds folder in the OS file manager, creating it (plus a
/// multilingual README explaining the {state}.{ext} naming convention) on
/// first use. Sound files are served read-only via GET /api/sounds/:state and
/// played by the external display when the matching state is entered.
#[tauri::command]
pub fn open_sounds_folder() -> Result<(), String> {
    let dir = models::user_sounds_dir();
    std::fs::create_dir_all(&dir)
        .map_err(|e| format!("Failed to create {}: {}", dir.display(), e))?;
    let readme = dir.join("README.txt");
    if !readme.exists() {
        std::fs::write(&readme, models::SOUNDS_README)
            .map_err(|e| format!("Failed to write README: {}", e))?;
    }
    open_in_file_manager(&dir)
}

// ===== External display access =====

/// Read the `external_access` flag from config. When true the HTTP server
/// binds to 0.0.0.0 so other devices on the LAN can read the read-only
/// `/api/*` routes (external display). Defaults to false (loopback only).
#[tauri::command]
pub fn get_external_access() -> Result<bool, String> {
    Ok(user_config::load().external_access.unwrap_or(false))
}

/// Persist the `external_access` flag. Takes effect on the next app restart —
/// a live listener's bind address can't change, so the renderer hints the
/// user to restart after toggling.
#[tauri::command]
pub fn set_external_access(enabled: bool) {
    user_config::update(|cfg| cfg.external_access = Some(enabled));
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

/// Shared snap detection used by `detect_edge_dock` and the drag-time ghost
/// preview: returns the monitor holding most of the window plus the crossed
/// edge that would trigger docking. The window must be PAST the edge — either
/// off-screen (outer edges) or on a neighbouring monitor (internal boundaries
/// count too) — by more than 20% of its width; nothing else snaps.
fn snap_target<'a>(
    monitors: &'a [tauri::Monitor],
    pos: tauri::PhysicalPosition<i32>,
    size: tauri::PhysicalSize<u32>,
) -> Option<(&'a tauri::Monitor, &'static str)> {
    let mon = best_monitor(monitors, pos, size)?;
    let mp = mon.position();
    let ms = mon.size();
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
        (false, false) => return None,
    };
    Some((mon, edge))
}

/// The exact rect (physical px) `enter_edge_dock` would place the dock bar
/// at: EDGE_DOCK_THICKNESS wide, `content_height` tall (clamped), vertically
/// centered on `cy` and clamped inside the monitor. Shared with the drag-time
/// ghost preview so the snap lands exactly where the ghost promised.
fn dock_bar_rect(
    mp: &tauri::PhysicalPosition<i32>,
    ms: &tauri::PhysicalSize<u32>,
    edge: &str,
    factor: f64,
    cy: i32,
    content_height: u32,
) -> (i32, i32, u32, u32) {
    let w = (config::EDGE_DOCK_THICKNESS as f64 * factor).round() as u32;
    let min_h = (80.0 * factor).round() as u32;
    let max_h = ms.height.saturating_sub((16.0 * factor) as u32).max(min_h);
    let h = ((content_height as f64 * factor).round() as u32).clamp(min_h, max_h);
    let x = if edge == "left" {
        mp.x
    } else {
        mp.x + ms.width as i32 - w as i32
    };
    let margin = (8.0 * factor).round() as i32;
    let y_min = mp.y + margin;
    let y_max = (mp.y + ms.height as i32 - h as i32 - margin).max(y_min);
    let y = (cy - h as i32 / 2).clamp(y_min, y_max);
    (x, y, w, h)
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
    let monitors = app.available_monitors().map_err(|e| e.to_string())?;
    Ok(snap_target(&monitors, pos, size).map(|(_, edge)| edge.to_string()))
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

    // Vertically centered on the drop position, clamped inside the monitor.
    let (x, y, w, h) = dock_bar_rect(mp, ms, &edge, factor, cy, content_height);

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

/// Leave edge-dock mode: restore the pre-snap size and place the character's
/// center right under the cursor — the user grabbed the dock bar, so the pet
/// appears where they're pointing and the drag continues seamlessly. Falls
/// back to the old pull-away offset when the cursor position is unavailable.
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
    let mini = user_config::load().mini_mode.unwrap_or(false);

    // Restore the pre-dock size; fall back to the configured normal/mini size
    // (scaled to the current monitor) if nothing was saved.
    let (w, h) = match prev_size {
        Some(s) => s,
        None => {
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

    // Character center inside the window (logical px): the canvas is flush
    // top and centered horizontally.
    let (center_lx, center_ly) = if mini {
        (config::MINI_WINDOW_WIDTH as f64 / 2.0, config::MINI_PET_CANVAS_HEIGHT as f64 / 2.0)
    } else {
        (config::WINDOW_WIDTH as f64 / 2.0, config::PET_CANVAS_HEIGHT as f64 / 2.0)
    };
    let (mut x, mut y) = if let Some((cx, cy)) = crate::click_through::global_cursor_pos() {
        (
            cx - (center_lx * factor).round() as i32,
            cy - (center_ly * factor).round() as i32,
        )
    } else {
        // No global cursor API: keep the docked spot, pulled one threshold
        // inward so detect_edge_dock doesn't immediately re-snap.
        let pos = window.outer_position().map_err(|e| e.to_string())?;
        let offset = (config::EDGE_SNAP_THRESHOLD as f64 * factor).round() as i32;
        let x = match edge.as_deref() {
            Some("left") => pos.x + offset,
            Some("right") => pos.x - offset,
            _ => pos.x + offset,
        };
        (x, pos.y)
    };

    // Keep the restored window fully visible on its monitor.
    if let Ok(monitors) = window.available_monitors() {
        let size = tauri::PhysicalSize::new(w, h);
        if let Some(mon) =
            best_monitor(&monitors, tauri::PhysicalPosition::new(x, y), size)
        {
            let mp = mon.position();
            let ms = mon.size();
            x = x.clamp(mp.x, (mp.x + ms.width as i32 - w as i32).max(mp.x));
            y = y.clamp(mp.y, (mp.y + ms.height as i32 - h as i32).max(mp.y));
        }
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

/// Set the window's outer position AND size in one call. On Windows this uses
/// Win32 `SetWindowPos` (no SWP_NOMOVE / SWP_NOSIZE) so x/y/cx/cy apply
/// atomically. The two-step `set_size` then `set_position` otherwise flashes
/// when the menu opens leftward: `set_size` first grows the window rightward
/// (right edge jumps +delta), then `set_position` shifts it left — WebView2
/// paints that intermediate state as a visible flicker. With a single
/// `SetWindowPos` the right edge never moves, only the left edge extends.
/// Other platforms fall back to the two-step Tauri API.
#[cfg(windows)]
fn set_window_geometry(
    window: &WebviewWindow,
    x: i32,
    y: i32,
    w: u32,
    h: u32,
) -> Result<(), String> {
    use windows::Win32::Foundation::HWND;
    use windows::Win32::UI::WindowsAndMessaging::{SetWindowPos, SWP_NOACTIVATE, SWP_NOZORDER};
    // Tauri's `hwnd()` returns the windows-0.61 HWND (Tauri's own dep), but our
    // direct dep is windows-0.58 — the two HWND types are distinct and not
    // interchangeable, so SetWindowPos (0.58) won't accept the 0.61 handle.
    // Both are `HWND(*mut c_void)`, so unwrap the raw pointer and re-wrap it in
    // the 0.58 type the linker expects.
    let hwnd_061 = window.hwnd().map_err(|e| e.to_string())?;
    let hwnd = HWND(hwnd_061.0 as *mut core::ffi::c_void);
    unsafe {
        SetWindowPos(
            hwnd,
            HWND::default(),
            x,
            y,
            w as i32,
            h as i32,
            SWP_NOZORDER | SWP_NOACTIVATE,
        )
        .map_err(|e| e.to_string())?;
    }
    Ok(())
}

#[cfg(not(windows))]
fn set_window_geometry(
    window: &WebviewWindow,
    x: i32,
    y: i32,
    w: u32,
    h: u32,
) -> Result<(), String> {
    window
        .set_size(tauri::Size::Physical(tauri::PhysicalSize::new(w, h)))
        .map_err(|e| e.to_string())?;
    window
        .set_position(tauri::PhysicalPosition::new(x, y))
        .map_err(|e| e.to_string())?;
    Ok(())
}

/// Live preview of the upcoming edge snap while dragging: mirrors the
/// detect/enter geometry exactly and parks the hidden "dock-preview" ghost
/// window at the spot the dock bar would occupy. Hides the ghost when the
/// window is dragged back inside the snap threshold. The ghost window is
/// click-through, so the drag continues uninterrupted.
#[tauri::command]
pub fn update_dock_preview(
    window: WebviewWindow,
    app: AppHandle,
    content_height: u32,
) -> Result<(), String> {
    let preview = app
        .get_webview_window("dock-preview")
        .ok_or_else(|| "dock-preview window not found".to_string())?;
    let pos = window.outer_position().map_err(|e| e.to_string())?;
    let size = window.outer_size().map_err(|e| e.to_string())?;
    let factor = window.scale_factor().unwrap_or(1.0);
    let monitors = app.available_monitors().map_err(|e| e.to_string())?;

    let Some((mon, edge)) = snap_target(&monitors, pos, size) else {
        // Back inside the threshold — no snap would happen, drop the ghost.
        let _ = preview.hide();
        return Ok(());
    };
    let cy = pos.y + size.height as i32 / 2;
    let (x, y, w, h) = dock_bar_rect(mon.position(), mon.size(), edge, factor, cy, content_height);
    preview
        .set_size(tauri::Size::Physical(tauri::PhysicalSize::new(w, h)))
        .map_err(|e| e.to_string())?;
    preview
        .set_position(tauri::Position::Physical(tauri::PhysicalPosition::new(
            x, y,
        )))
        .map_err(|e| e.to_string())?;
    if !preview.is_visible().unwrap_or(false) {
        let _ = preview.show();
    }
    Ok(())
}

/// Hide the snap-preview ghost (drag ended or cancelled).
#[tauri::command]
pub fn hide_dock_preview(app: AppHandle) -> Result<(), String> {
    if let Some(preview) = app.get_webview_window("dock-preview") {
        let _ = preview.hide();
    }
    Ok(())
}

/// Pick the side and width for the context-menu growth, based on the pet's
/// position on its monitor (pet on the left half -> grow rightward, right
/// half -> grow leftward; flip when the chosen side is cramped). This is a
/// PURE calculation — it does NOT move the window or touch state — so the
/// renderer can stage its content transform (`translateX`) BEFORE the window
/// actually moves. That ordering matters: WebView2 composites synchronously
/// on the WM_SIZE triggered by the resize, so if the transform is already
/// staged when the window moves, the first frame after the move shows the
/// pet at its correct on-screen position. Staging the transform AFTER the
/// move (the old single-command design) instead painted one frame with the
/// pet shifted left, which read as a visible flicker on left-side menus.
/// Returns the chosen side and the width actually gained (clamped to the
/// monitor bounds), in logical CSS px.
#[tauri::command]
pub fn calculate_menu_space(
    window: WebviewWindow,
    app: AppHandle,
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

    // Pet on the left half of its monitor -> menu opens rightward (toward the
    // free space) and vice versa; flip when the chosen side is cramped and the
    // opposite side has more room.
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

    Ok(MenuSpaceInfo {
        side: side.to_string(),
        delta: (delta as f64 / factor).round() as u32,
    })
}

/// Apply a growth calculated by `calculate_menu_space`: resize/shift the
/// window and record the growth so `close_menu_space` can undo it. The
/// renderer MUST stage its content transform (`translateX(delta)` for
/// left-side growth) BEFORE calling this, so the synchronous WM_SIZE
/// composite shows the pet at its correct position. Re-reads the live window
/// rect so a drag between calculate and apply can't corrupt the geometry.
#[tauri::command]
pub fn apply_menu_space(
    window: WebviewWindow,
    state: State<'_, MenuSpaceState>,
    side: String,
    delta: u32, // logical CSS px, as returned by calculate_menu_space
) -> Result<(), String> {
    let factor = window.scale_factor().unwrap_or(1.0);
    let delta_phys = (delta as f64 * factor).round() as i32;
    if delta_phys > 0 {
        let pos = window.outer_position().map_err(|e| e.to_string())?;
        let size = window.outer_size().map_err(|e| e.to_string())?;
        let mut x = pos.x;
        let w = (size.width as i32 + delta_phys).max(80);
        if side == "left" {
            x -= delta_phys;
        }
        set_window_geometry(&window, x, pos.y, w as u32, size.height)?;
    }
    // Record even when delta_phys == 0 so close_menu_space clears `active`
    // (suppresses position persistence while the menu is open) consistently.
    *state.grow.lock().unwrap() = Some((side, delta_phys));
    state.active.store(true, Ordering::SeqCst);
    Ok(())
}

/// Undo the growth applied by `apply_menu_space` (restore width, and shift
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
    set_window_geometry(&window, x, pos.y, w as u32, size.height)
}

// ===== Separate menu window (zero-flicker approach) =====
//
// The pet window NEVER resizes when the menu opens. Instead, a second
// borderless transparent window ("menu") is shown beside the pet. Because
// the pet window doesn't resize, there's no WebView2 layout lag → zero
// flicker on both left and right sides.

/// Show the menu window at the given logical position and size, then focus it.
/// Called by the main window's renderer after calculating which side of the
/// pet to place the menu on.
#[tauri::command]
pub fn show_menu_window(
    app: AppHandle,
    x: f64,
    y: f64,
    w: f64,
    h: f64,
) -> Result<(), String> {
    let Some(menu_win) = app.get_webview_window("menu") else {
        return Err("menu window not found".into());
    };
    menu_win
        .set_size(tauri::Size::Logical(tauri::LogicalSize::new(w, h)))
        .map_err(|e| e.to_string())?;
    menu_win
        .set_position(tauri::Position::Logical(tauri::LogicalPosition::new(x, y)))
        .map_err(|e| e.to_string())?;
    menu_win.show().map_err(|e| e.to_string())?;
    menu_win.set_focus().map_err(|e| e.to_string())?;
    Ok(())
}

/// Hide the menu window. Called when the user closes the menu (clicks an
/// action, presses Escape, or the window loses focus).
#[tauri::command]
pub fn hide_menu_window(app: AppHandle) -> Result<(), String> {
    let Some(menu_win) = app.get_webview_window("menu") else {
        return Ok(());
    };
    let _ = menu_win.hide();
    Ok(())
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
