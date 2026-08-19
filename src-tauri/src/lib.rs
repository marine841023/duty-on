//! DutyOn （开工啦） — 2.0 headless backend.
//!
//! 2.0 architecture: NO browser / WebView anywhere — not on PC, not on ARM
//! devices. This Rust process is a pure backend: HTTP API server (state,
//! events SSE, metrics, sounds) + state manager + IDE scanner + system
//! monitor sampler + a tray icon with Quit. All rendering and UI is done by
//! the native C++ client (device/: GLFW + OpenGL + ImGui on Windows,
//! EGL/GLES2 on ARM Linux), which polls this backend over HTTP.
//!
//! The Tauri commands below are still registered (they're shared with the
//! 1.x line and harmless), but with no WebView frontend nothing invokes
//! them; window-management commands would simply find no window.

mod click_through;
mod commands;
mod config;
mod hooks_installer;
mod ide_scanner;
mod models;
mod server;
mod state_manager;
mod sys_monitor;
mod user_config;

use crate::state_manager::{SharedStateManager, StateManager};
use std::sync::atomic::AtomicBool;
use std::sync::Arc;
use std::time::Duration;
use tauri::{
    menu::{Menu, MenuItem},
    tray::TrayIconBuilder,
    Manager,
};
use tauri_plugin_autostart::MacosLauncher;

/// Set to true while a file picker dialog is open. Kept for the shared
/// commands module (pick_character_animation); unused in headless mode.
pub static IS_PICKING_FILE: AtomicBool = AtomicBool::new(false);

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
        // Prevent a second backend instance (port 17521 would conflict).
        .plugin(tauri_plugin_single_instance::init(|_app, _argv, _cwd| {}))
        .plugin(tauri_plugin_dialog::init())
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
            commands::resize_menu_window,
            commands::get_monitor_config,
            commands::update_monitor_config,
            commands::set_monitor_sampling,
            commands::resize_pet_window,
            commands::update_click_regions,
            commands::set_force_clickable,
            commands::bring_to_front,
            commands::flash_attention,
            commands::quit,
            commands::uninstall_app,
            commands::get_characters,
            commands::create_character,
            commands::delete_character,
            commands::pick_character_animation,
            commands::clear_character_animation,
            commands::switch_character,
            commands::save_model_thumbnail,
            commands::read_live2d_bundle,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}

/// Headless setup: state + services only. No windows are created — the
/// process keeps running thanks to the tray icon and the HTTP server.
fn setup(app: &mut tauri::App) -> Result<(), Box<dyn std::error::Error>> {
    // ----- State manager -----
    let sm: SharedStateManager = Arc::new(tokio::sync::Mutex::new(StateManager::new()));
    app.manage(sm.clone());

    // States shared with the commands module (registered so managed-state
    // lookups never panic if a command is invoked).
    app.manage(commands::EdgeDockState::default());
    app.manage(commands::MenuSpaceState::default());
    app.manage(click_through::ClickThroughState::new());

    // ----- HTTP server (hook events from the Trae IDE bridge; also serves
    // /api/status, /api/events SSE, /api/metrics and /api/sounds to the
    // native C++ client) -----
    let sm_http = sm.clone();
    tauri::async_runtime::spawn(async move {
        server::start(sm_http).await;
    });

    // ----- Periodic cleanup + alert reminder timers -----
    spawn_timers(sm.clone());

    // ----- IDE window scanner (adaptive interval) -----
    spawn_ide_scanner(sm.clone());

    // ----- System monitor sampler (CPU/RAM/GPU/NET/self, 1.5s tick) -----
    // The thread sleeps at zero cost while no client polls /api/metrics.
    let mon_cfg = user_config::load().monitor.unwrap_or_default();
    sys_monitor::MONITOR_ACTIVE.store(mon_cfg.enabled, std::sync::atomic::Ordering::SeqCst);
    sys_monitor::spawn(app.handle().clone());

    // ----- System tray (Quit only) -----
    // The native C++ client has its own tray for pet interactions; this one
    // exists so the headless backend can always be found and stopped.
    let quit_item = MenuItem::with_id(app, "tray_quit", "退出 DutyOn 后端", true, None::<&str>)?;
    let tray_menu = Menu::with_items(app, &[&quit_item])?;

    TrayIconBuilder::with_id("backend-tray")
        .icon(app.default_window_icon().unwrap().clone())
        .menu(&tray_menu)
        .tooltip("DutyOn Backend")
        .on_menu_event(|app, event| {
            if event.id.as_ref() == "tray_quit" {
                app.exit(0);
            }
        })
        .build(app)?;

    log::info!("[setup] headless backend started (no WebView windows)");
    Ok(())
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
