//! DutyOn （开工啦） — Live2D desktop pet for monitoring Trae IDE AI task status.
//! Tauri 2.0 rewrite of the Electron app. See `.trae/documents/tauri-rewrite-plan.md`.

mod click_through;
mod commands;
mod config;
mod hooks_installer;
mod ide_scanner;
mod models;
mod server;
mod state_manager;
mod user_config;

use crate::state_manager::{current_millis, SharedStateManager, StateManager};
use crate::user_config::WindowPosition;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::Duration;
use tauri::{
    Emitter, Manager, PhysicalPosition, WebviewUrl, WebviewWindowBuilder,
    menu::{Menu, MenuItem, PredefinedMenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
};
use tauri_plugin_autostart::MacosLauncher;
use tokio::sync::broadcast::error::RecvError;

/// Entry point used by `main.rs` and the mobile/desktop lib targets.
#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info"))
        .format_timestamp_secs()
        .init();

    tauri::Builder::default()
        .plugin(tauri_plugin_autostart::init(
            MacosLauncher::LaunchAgent,
            None,
        ))
        .plugin(tauri_plugin_single_instance::init(|app, _argv, _cwd| {
            // A second launch attempted — focus the existing window instead.
            if let Some(w) = app.get_webview_window("main") {
                let _ = w.show();
                let _ = w.set_focus();
            }
        }))
        .setup(setup)
        .invoke_handler(tauri::generate_handler![
            commands::install_hooks,
            commands::is_hooks_installed,
            commands::get_models,
            commands::switch_model,
            commands::open_live2d_folder,
            commands::open_sounds_folder,
            commands::get_external_access,
            commands::set_external_access,
            commands::get_state_motions,
            commands::set_state_motions,
            commands::get_appearance,
            commands::set_flip_horizontal,
            commands::set_mini_mode,
            commands::detect_edge_dock,
            commands::enter_edge_dock,
            commands::exit_edge_dock,
            commands::update_dock_preview,
            commands::hide_dock_preview,
            commands::get_language,
            commands::set_language,
            commands::get_auto_launch,
            commands::set_auto_launch,
            commands::get_state,
            commands::reset_to_idle,
            commands::debug_log,
            commands::test_alert,
            commands::drag_window,
            commands::calculate_menu_space,
            commands::apply_menu_space,
            commands::close_menu_space,
            commands::show_menu_window,
            commands::hide_menu_window,
            commands::update_click_regions,
            commands::set_force_clickable,
            commands::bring_to_front,
            commands::flash_attention,
            commands::quit,
            commands::uninstall_app,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

fn setup(app: &mut tauri::App) -> Result<(), Box<dyn std::error::Error>> {
    // ----- State manager -----
    let sm: SharedStateManager = Arc::new(tokio::sync::Mutex::new(StateManager::new()));
    app.manage(sm.clone());

    // Pre-snap geometry for edge-dock restore. Kept as a clone so the
    // WindowEvent::Moved handler below can skip persisting docked positions.
    let edge_dock = commands::EdgeDockState::default();
    app.manage(edge_dock.clone());

    // Temporary horizontal window growth while the context menu is open.
    let menu_space = commands::MenuSpaceState::default();
    app.manage(menu_space.clone());

    // ----- Window: position + show -----
    let window = app
        .get_webview_window("main")
        .ok_or("main window not found")?;
    position_window(app, &window)?;
    let _ = window.set_always_on_top(true);
    let _ = window.show();

    // Ghost preview window for the upcoming edge snap, shown while dragging
    // close to a screen edge (update_dock_preview). Transparent, click-through
    // and hidden by default — it must never intercept the drag.
    let preview = WebviewWindowBuilder::new(app, "dock-preview", WebviewUrl::App("preview.html".into()))
        .decorations(false)
        .transparent(true)
        .shadow(false)
        .resizable(false)
        .always_on_top(true)
        .skip_taskbar(true)
        .visible(false)
        .inner_size(config::EDGE_DOCK_THICKNESS as f64, 200.0)
        .build()?;
    let _ = preview.set_ignore_cursor_events(true);

    // ----- Menu window (separate, zero-flicker) -----
    // A second borderless transparent window that hosts the context menu. The
    // pet window NEVER resizes when the menu opens — instead this window is
    // shown beside the pet. Because the pet window's geometry is unchanged,
    // WebView2's one-frame layout lag (the root cause of left-side menu
    // flicker) never triggers. The window loads the same index.html with an
    // injected flag (window.__MENU_MODE__ = true) so renderer.js can skip
    // PixiJS/Live2D init and run in menu-only mode.
    let menu_win = WebviewWindowBuilder::new(
        app,
        "menu",
        WebviewUrl::App("index.html".into()),
    )
    .title("Menu")
    .decorations(false)
    .transparent(true)
    .shadow(false)
    .resizable(false)
    .always_on_top(true)
    .skip_taskbar(true)
    .visible(false)
    .inner_size(230.0, 500.0)
    .initialization_script("window.__MENU_MODE__ = true;")
    .build()?;

    // Hide the menu window when it loses focus (user clicks elsewhere,
    // Alt-Tab, etc.). Emit an event so the main window can reset its menu
    // state.
    let menu_win_for_blur = menu_win.clone();
    let app_for_blur = app.handle().clone();
    menu_win.on_window_event(move |event| {
        if let tauri::WindowEvent::Focused(false) = event {
            let _ = menu_win_for_blur.hide();
            let _ = app_for_blur.emit("menu-closed", ());
        }
    });

    // Log window state for debugging "completely transparent / can't click"
    let vis = window.is_visible().unwrap_or(false);
    let pos = window.outer_position().map(|p| (p.x, p.y)).unwrap_or((-1, -1));
    let (sw, sh) = window.outer_size().map(|s| (s.width, s.height)).unwrap_or((0, 0));
    let aot = window.is_always_on_top().unwrap_or(false);
    let decor = window.is_decorated().unwrap_or(false);
    log::info!(
        "[window] visible={} pos=({},{}) size={}x{} alwaysOnTop={} decorated={}",
        vis, pos.0, pos.1, sw, sh, aot, decor
    );

    // Log all detected monitors so multi-monitor dock bugs are diagnosable.
    if let Ok(monitors) = app.available_monitors() {
        log::info!("[monitors] detected {} display(s):", monitors.len());
        for (i, m) in monitors.iter().enumerate() {
            let p = m.position();
            let s = m.size();
            let sf = m.scale_factor();
            log::info!(
                "  #{} pos=({},{}) size={}x{} scale={}",
                i, p.x, p.y, s.width, s.height, sf
            );
        }
    }

    // Open devtools in debug builds so we can inspect the webview console —
    // essential for diagnosing frontend load failures (JS errors, CSP blocks,
    // asset protocol issues that leave the transparent window blank).
    #[cfg(debug_assertions)]
    window.open_devtools();

    // Persist window position on move (throttled to every 500ms). Docked
    // (edge-snapped) positions are skipped so a restart doesn't open the
    // full-size window glued to the screen edge; the shifted geometry while
    // the context menu has grown the window is skipped for the same reason.
    let last_save = Arc::new(AtomicU64::new(0));
    let last_save_clone = last_save.clone();
    let win_scale = window.clone();
    window.on_window_event(move |event| match event {
        tauri::WindowEvent::Moved(pos) => {
            if edge_dock.active.load(Ordering::SeqCst)
                || menu_space.active.load(Ordering::SeqCst)
            {
                return;
            }
            let now = current_millis();
            if now.saturating_sub(last_save_clone.load(Ordering::SeqCst)) >= 500 {
                last_save_clone.store(now, Ordering::SeqCst);
                user_config::update(|c| {
                    c.window_position = Some(WindowPosition { x: pos.x, y: pos.y });
                });
            }
        }
        // The DPI scale changes at runtime — remote-desktop connect/disconnect,
        // moving between monitors with different DPI, or a display-settings
        // change. The OS keeps the window's PHYSICAL size, so its logical size
        // (the unit the CSS layout works in) drifts and content gets clipped.
        // Re-pin the window to its intended logical size, drop the temporary
        // menu growth (recorded in stale physical px), and tell the renderer
        // to reset its menu state too.
        tauri::WindowEvent::ScaleFactorChanged {
            scale_factor,
            new_inner_size,
            ..
        } => {
            *menu_space.grow.lock().unwrap() = None;
            menu_space.active.store(false, Ordering::SeqCst);

            let mini = user_config::load().mini_mode.unwrap_or(false);
            let (lw, lh) = if mini {
                (config::MINI_WINDOW_WIDTH, config::MINI_WINDOW_HEIGHT)
            } else {
                (config::WINDOW_WIDTH, config::WINDOW_HEIGHT)
            };
            let (phys_w, phys_h) = if edge_dock.active.load(Ordering::SeqCst) {
                // Stay docked: re-pin the bar thickness, keep its height.
                let t = (config::EDGE_DOCK_THICKNESS as f64 * scale_factor).round() as u32;
                (t, new_inner_size.height)
            } else {
                (
                    (lw as f64 * scale_factor).round() as u32,
                    (lh as f64 * scale_factor).round() as u32,
                )
            };
            let _ = win_scale.set_size(tauri::Size::Physical(tauri::PhysicalSize::new(
                phys_w, phys_h,
            )));

            // Keep the window visible: clamp into its current monitor.
            if let (Ok(pos), Ok(Some(m))) =
                (win_scale.outer_position(), win_scale.current_monitor())
            {
                let mp = m.position();
                let ms = m.size();
                let x = pos
                    .x
                    .clamp(mp.x, (mp.x + ms.width as i32 - phys_w as i32).max(mp.x));
                let y = pos
                    .y
                    .clamp(mp.y, (mp.y + ms.height as i32 - phys_h as i32).max(mp.y));
                if x != pos.x || y != pos.y {
                    let _ = win_scale.set_position(PhysicalPosition::new(x, y));
                }
            }

            let _ = win_scale.emit("display-changed", ());
        }
        _ => {}
    });

    // ----- Click-through polling thread -----
    // Tauri's set_ignore_cursor_events has no {forward:true} mode, so the
    // frontend can't drive click-through via mousemove (it stops receiving
    // events once ignore=true). A Rust thread polls Win32 GetCursorPos and
    // toggles ignore based on regions the frontend reports via the PixiJS
    // ticker (which keeps running while click-through). See click_through.rs.
    let ct_state = click_through::ClickThroughState::new();
    app.manage(ct_state.clone());
    let window_ct = window.clone();
    std::thread::spawn(move || click_through::run_polling_loop(window_ct, ct_state));

    // ----- Forward state updates / alerts to the renderer -----
    spawn_listeners(app.handle().clone(), sm.clone());

    // ----- HTTP server (hook events from the Trae IDE bridge) -----
    let sm_http = sm.clone();
    tauri::async_runtime::spawn(async move {
        server::start(sm_http).await;
    });

    // ----- Periodic cleanup + alert reminder timers -----
    spawn_timers(sm.clone());

    // ----- IDE window scanner (adaptive interval) -----
    spawn_ide_scanner(sm.clone());

    // ----- System tray -----
    // The pet window is skipTaskbar + frameless + transparent, so users may
    // lose track of it on a busy desktop. A tray icon provides a visible
    // anchor with quick actions (show / hide / quit). Left-click toggles
    // visibility.
    let show_item = MenuItem::with_id(app, "tray_show", "显示宠物", true, None::<&str>)?;
    let hide_item = MenuItem::with_id(app, "tray_hide", "隐藏宠物", true, None::<&str>)?;
    let sep_item = PredefinedMenuItem::separator(app)?;
    let quit_item = MenuItem::with_id(app, "tray_quit", "退出 DutyOn", true, None::<&str>)?;
    let tray_menu = Menu::with_items(app, &[&show_item, &hide_item, &sep_item, &quit_item])?;

    TrayIconBuilder::with_id("main-tray")
        .icon(app.default_window_icon().unwrap().clone())
        .menu(&tray_menu)
        .tooltip("DutyOn")
        .on_menu_event(|app, event| match event.id.as_ref() {
            "tray_show" => {
                if let Some(w) = app.get_webview_window("main") {
                    let _ = w.show();
                    let _ = w.set_focus();
                }
            }
            "tray_hide" => {
                if let Some(w) = app.get_webview_window("main") {
                    let _ = w.hide();
                }
            }
            "tray_quit" => {
                app.exit(0);
            }
            _ => {}
        })
        .on_tray_icon_event(|tray, event| {
            if let TrayIconEvent::Click {
                button: MouseButton::Left,
                button_state: MouseButtonState::Up,
                ..
            } = event
            {
                let app = tray.app_handle();
                if let Some(w) = app.get_webview_window("main") {
                    if w.is_visible().unwrap_or(false) {
                        let _ = w.hide();
                    } else {
                        let _ = w.show();
                        let _ = w.set_focus();
                    }
                }
            }
        })
        .build(app)?;

    Ok(())
}

/// Restore the last window position if still on a visible screen, else default
/// to the bottom-right corner of the primary monitor.
fn position_window(app: &tauri::App, window: &tauri::WebviewWindow) -> Result<(), Box<dyn std::error::Error>> {
    let cfg = user_config::load();

    // Restore the mini-mode window size before positioning so the default
    // bottom-right placement and the on-screen check use the real geometry.
    let mini = cfg.mini_mode.unwrap_or(false);
    let (win_w, win_h) = if mini {
        (config::MINI_WINDOW_WIDTH, config::MINI_WINDOW_HEIGHT)
    } else {
        (config::WINDOW_WIDTH, config::WINDOW_HEIGHT)
    };
    if mini {
        let _ = window.set_size(tauri::Size::Logical(tauri::LogicalSize::new(
            win_w as f64,
            win_h as f64,
        )));
    }

    let (screen_w, screen_h) = app
        .primary_monitor()?
        .map(|m| (m.size().width as i32, m.size().height as i32))
        .unwrap_or((1920, 1080));

    let mut x = screen_w - win_w - config::WINDOW_MARGIN;
    let mut y = screen_h - win_h - config::WINDOW_MARGIN;
    if let Some(pos) = &cfg.window_position {
        if is_position_on_screen(app, pos.x, pos.y, win_w, win_h) {
            x = pos.x;
            y = pos.y;
        }
    }
    window.set_position(PhysicalPosition::new(x, y))?;
    Ok(())
}

/// True if the center of a window at (x,y) size (w,h) lies on any monitor.
fn is_position_on_screen(app: &tauri::App, x: i32, y: i32, w: i32, h: i32) -> bool {
    let cx = x + w / 2;
    let cy = y + h / 2;
    if let Ok(monitors) = app.available_monitors() {
        for m in monitors {
            let p = m.position();
            let s = m.size();
            if cx >= p.x
                && cx <= p.x + s.width as i32
                && cy >= p.y
                && cy <= p.y + s.height as i32
            {
                return true;
            }
        }
    }
    false
}

/// Spawn tasks that subscribe to state-manager broadcasts and emit them as
/// Tauri events (`state-update`, `alert`) to the renderer. Subscription
/// happens inside the async task (the subscribe call needs the async mutex).
fn spawn_listeners(app_handle: tauri::AppHandle, sm: SharedStateManager) {
    let app_for_update = app_handle.clone();
    let sm_update = sm.clone();
    tauri::async_runtime::spawn(async move {
        let mut rx = {
            let s = sm_update.lock().await;
            s.subscribe_update()
        };
        loop {
            match rx.recv().await {
                Ok(snap) => {
                    let _ = app_for_update.emit("state-update", snap);
                }
                Err(RecvError::Lagged(_)) => continue,
                Err(RecvError::Closed) => break,
            }
        }
    });

    let sm_alert = sm;
    tauri::async_runtime::spawn(async move {
        let mut rx = {
            let s = sm_alert.lock().await;
            s.subscribe_alert()
        };
        loop {
            match rx.recv().await {
                Ok(snap) => {
                    let _ = app_handle.emit("alert", snap);
                }
                Err(RecvError::Lagged(_)) => continue,
                Err(RecvError::Closed) => break,
            }
        }
    });
}

/// Spawn the cleanup (30s) and alert-reminder (60s) interval timers.
fn spawn_timers(sm: SharedStateManager) {
    let sm_cleanup = sm.clone();
    tauri::async_runtime::spawn(async move {
        let mut interval = tokio::time::interval(Duration::from_millis(config::CLEANUP_INTERVAL_MS));
        loop {
            interval.tick().await;
            let mut s = sm_cleanup.lock().await;
            s.cleanup_stale_sessions();
        }
    });

    let sm_alert = sm;
    tauri::async_runtime::spawn(async move {
        let mut interval = tokio::time::interval(Duration::from_millis(config::ALERT_REMINDER));
        loop {
            interval.tick().await;
            let mut s = sm_alert.lock().await;
            s.check_and_remind_alert();
        }
    });
}

/// Spawn the IDE window scanner. Adaptive: 4s when sessions exist (fast close
/// detection), 15s when idle (no IDE open, save CPU). Uses setTimeout-style
/// recursion so a slow scan can't overlap the next tick. Whenever the
/// detected set changes, the new set plus any IDE-looking-but-unparsed window
/// titles are written to frontend.log — the key diagnostic for "IDE visible
/// but the pet lost it" reports.
fn spawn_ide_scanner(sm: SharedStateManager) {
    tauri::async_runtime::spawn(async move {
        let mut last_names: Vec<String> = Vec::new();
        loop {
            // Run both the window scan and the CLI liveness probe in one
            // blocking task (each touches the OS and would stall the async
            // runtime if called on the async thread).
            let ((names, suspects), liveness) =
                tokio::task::spawn_blocking(|| {
                    (
                        ide_scanner::scan_ide_projects(),
                        ide_scanner::scan_cli_processes(),
                    )
                })
                .await
                .unwrap_or_else(|_| {
                    ((Vec::new(), Vec::new()), ide_scanner::CliLiveness::default())
                });
            let name_list: Vec<String> = names
                .iter()
                .map(|d| format!("{}({})", d.name, d.ide.as_str()))
                .collect();
            if name_list != last_names {
                crate::server::append_log_file(
                    "info",
                    &format!(
                        "[scanner] detected={:?} suspects={:?}",
                        name_list, suspects
                    ),
                );
                last_names = name_list;
            }
            let next_delay = {
                let mut s = sm.lock().await;
                s.sync_detected_windows(&names);
                s.sync_cli_liveness(&liveness);
                if s.session_count() > 0 {
                    config::SCAN_INTERVAL_ACTIVE_MS
                } else {
                    config::SCAN_INTERVAL_IDLE_MS
                }
            };
            tokio::time::sleep(Duration::from_millis(next_delay)).await;
        }
    });
}
