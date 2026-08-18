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

// ===== System monitor (monitor drawer above the status bar) =====

/// Current monitor-drawer extra height of the pet window (logical px), set by
/// resize_pet_window. Positive = drawer open (window grown upward); negative =
/// project list hidden (window shrunk below the base height). The lib.rs
/// Moved handler uses it to persist the BASE position (as if the drawer were
/// closed) — otherwise quitting with the drawer open would restore the window
/// floating above where the user left it, and re-growing on the next launch
/// would push it further off-screen.
pub static PET_WINDOW_EXTRA_HEIGHT: std::sync::atomic::AtomicI32 =
    std::sync::atomic::AtomicI32::new(0);

/// Fetch the persisted monitor drawer configuration (defaults filled in).
#[tauri::command]
pub fn get_monitor_config() -> user_config::MonitorConfig {
    user_config::load().monitor.unwrap_or_default()
}

/// Persist the full monitor config and sync the sampler's active flag so the
/// sampling thread sleeps while the drawer is closed.
#[tauri::command]
pub fn update_monitor_config(
    config: user_config::MonitorConfig,
) -> Result<(), String> {
    crate::sys_monitor::MONITOR_ACTIVE.store(config.enabled, std::sync::atomic::Ordering::SeqCst);
    user_config::update(|cfg| cfg.monitor = Some(config));
    Ok(())
}

/// Runtime sampling gate, driven by what is actually on screen: the frontend
/// pauses sampling while the drawer is closed/collapsed/mini or every row is
/// hidden, without touching the persisted config. update_monitor_config
/// re-arms sampling from the persisted enabled flag when the config changes.
#[tauri::command]
pub fn set_monitor_sampling(enabled: bool) {
    crate::sys_monitor::MONITOR_ACTIVE.store(enabled, std::sync::atomic::Ordering::SeqCst);
}

/// Grow (positive) or shrink (negative) the pet window by `extra_height`
/// logical px. The monitor drawer grows the window upward; hiding the
/// project list shrinks it below the base height.
///
/// Anchor selection keeps the character (top-aligned canvas) visually
/// stable: at/above base height the BOTTOM edge stays fixed (drawer grows
/// upward, matching the persisted base position). Below base height —
/// shrinking there (project list hidden) or returning from it (re-shown) —
/// the TOP edge stays fixed so the lower UI collapses downward instead of
/// dragging the character down the screen.
#[tauri::command]
pub fn resize_pet_window(window: tauri::Window, extra_height: f64) -> Result<(), String> {
    let mini = user_config::load().mini_mode.unwrap_or(false);
    let (w, base_h) = if mini {
        (
            config::MINI_WINDOW_WIDTH as f64,
            config::MINI_WINDOW_HEIGHT as f64,
        )
    } else {
        (config::WINDOW_WIDTH as f64, config::WINDOW_HEIGHT as f64)
    };
    // Floor at the bare canvas height: hiding the project list leaves only
    // the Live2D character (canvas = 260px), with no lower UI to keep.
    let new_h = (base_h + extra_height).max(config::PET_CANVAS_HEIGHT as f64);
    let extra = new_h - base_h;
    PET_WINDOW_EXTRA_HEIGHT.store(extra.round() as i32, std::sync::atomic::Ordering::SeqCst);
    let factor = window.scale_factor().unwrap_or(1.0);
    let cur_below_base = window
        .outer_size()
        .ok()
        .map(|s| (s.height as f64 / factor) < base_h - 0.5)
        .unwrap_or(false);
    let top_anchored = extra < 0.0 || cur_below_base;
    let anchor = window
        .outer_position()
        .ok()
        .zip(window.outer_size().ok())
        .map(|(pos, size)| {
            let new_h_phys = new_h * factor;
            let y = if top_anchored {
                pos.y
            } else {
                (pos.y as f64 + (size.height as f64 - new_h_phys)).round() as i32
            };
            (pos.x, y)
        });
    window
        .set_size(tauri::Size::Logical(tauri::LogicalSize::new(w, new_h)))
        .map_err(|e| e.to_string())?;
    if let Some((x, y)) = anchor {
        let _ = window.set_position(tauri::Position::Physical(
            tauri::PhysicalPosition::new(x, y),
        ));
    }
    Ok(())
}

// ===== Custom characters =====

const ANIMATION_EXTS: &[&str] = &["gif", "png", "jpg", "jpeg", "webp", "mp4", "webm", "mov"];

/// Max edge length (px) for uploaded animation images. Source images larger
/// than this are downscaled (preserving aspect ratio) so giant GIFs don't
/// bloat ~/.dutyon/animations/ or stall the renderer. Videos are not resized.
/// 1024px leaves headroom for high-DPI screens (the Live2D canvas renders at
/// resolution>=2, so a 240px logical display needs ~480px backing pixels).
const MAX_ANIM_DIM: u32 = 1024;

/// Copy `source` to `dest`, resizing first if it's a still image or animated
/// GIF. Videos (mp4/webm/mov) are copied verbatim — re-encoding requires
/// ffmpeg which we don't bundle. Returns Ok(()) on success.
fn resize_or_copy_animation(source: &std::path::Path, dest: &std::path::Path) -> Result<(), String> {
    let ext = source
        .extension()
        .and_then(|e| e.to_str())
        .map(|s| s.to_ascii_lowercase())
        .unwrap_or_default();

    let is_video = matches!(ext.as_str(), "mp4" | "webm" | "mov");
    if is_video {
        std::fs::copy(source, dest).map_err(|e| format!("Failed to copy video: {}", e))?;
        return Ok(());
    }

    match ext.as_str() {
        "gif" => resize_animated_gif(source, dest),
        "png" | "jpg" | "jpeg" | "webp" => resize_static_image(source, dest),
        _ => {
            // Unknown image type — copy verbatim as a fallback.
            std::fs::copy(source, dest).map_err(|e| format!("Failed to copy file: {}", e))?;
            Ok(())
        }
    }
}

/// Resize an animated GIF to fit within MAX_ANIM_DIM×MAX_ANIM_DIM (preserving
/// aspect ratio and animation: every frame is resized and re-encoded).
fn resize_animated_gif(source: &std::path::Path, dest: &std::path::Path) -> Result<(), String> {
    use image::codecs::gif::{GifDecoder, GifEncoder, Repeat};
    use image::AnimationDecoder;
    use std::fs::File;
    use std::io::BufReader;

    // First check the GIF's dimensions — skip the expensive frame-by-frame
    // re-encode if it's already small enough.
    let probe = image::ImageReader::open(source)
        .map_err(|e| format!("Failed to open GIF: {}", e))?
        .with_guessed_format()
        .map_err(|e| format!("Failed to read GIF: {}", e))?;
    let probe_dims = probe
        .into_dimensions()
        .map_err(|e| format!("Failed to read GIF dimensions: {}", e))?;
    if probe_dims.0 <= MAX_ANIM_DIM && probe_dims.1 <= MAX_ANIM_DIM {
        std::fs::copy(source, dest).map_err(|e| format!("Failed to copy GIF: {}", e))?;
        return Ok(());
    }

    let file = File::open(source).map_err(|e| format!("Failed to open GIF: {}", e))?;
    let decoder =
        GifDecoder::new(BufReader::new(file)).map_err(|e| format!("Failed to decode GIF: {}", e))?;
    let frames = decoder.into_frames();

    let dest_file = File::create(dest).map_err(|e| format!("Failed to create dest GIF: {}", e))?;
    let mut encoder = GifEncoder::new(dest_file);
    // Preserve infinite looping (the re-encode drops the source's Netscape
    // loop extension by default, which makes the GIF play only once).
    encoder
        .set_repeat(Repeat::Infinite)
        .map_err(|e| format!("Failed to set GIF loop: {}", e))?;

    // Resource limit: cap total pixels decoded to prevent "GIF bomb" OOM.
    // MAX_ANIM_DIM * 4 (RGBA bytes) * MAX_FRAMES (500) = 2GB max for 1024px.
    const MAX_FRAMES: usize = 500;
    let mut frame_count = 0;
    for frame_result in frames {
        if frame_count >= MAX_FRAMES {
            return Err(format!("GIF has too many frames (max {})", MAX_FRAMES));
        }
        let frame = frame_result.map_err(|e| format!("Failed to decode GIF frame: {}", e))?;
        frame_count += 1;
        let buf = frame.buffer();
        // Additional per-frame pixel limit: 2048x2048 = 16MB per frame.
        if buf.width() > 2048 || buf.height() > 2048 {
            return Err(format!("GIF frame too large: {}x{}", buf.width(), buf.height()));
        }
        let (w, h) = (buf.width(), buf.height());
        let (nw, nh) = scaled_dims(w, h);
        // Triangle filter gives smooth downscale for sprite-style art.
        let resized = image::imageops::resize(buf, nw, nh, image::imageops::FilterType::Triangle);
        let new_frame =
            image::Frame::from_parts(resized, frame.left(), frame.top(), frame.delay());
        encoder
            .encode_frame(new_frame)
            .map_err(|e| format!("Failed to encode GIF frame: {}", e))?;
    }
    Ok(())
}

/// Resize a static image (png/jpg/webp) to fit within MAX_ANIM_DIM×MAX_ANIM_DIM.
fn resize_static_image(source: &std::path::Path, dest: &std::path::Path) -> Result<(), String> {
    let img = image::open(source).map_err(|e| format!("Failed to open image: {}", e))?;
    let (w, h) = (img.width(), img.height());
    if w <= MAX_ANIM_DIM && h <= MAX_ANIM_DIM {
        img.save(dest)
            .map_err(|e| format!("Failed to save image: {}", e))?;
        return Ok(());
    }
    let (nw, nh) = scaled_dims(w, h);
    let resized = image::imageops::resize(&img, nw, nh, image::imageops::FilterType::Triangle);
    resized
        .save(dest)
        .map_err(|e| format!("Failed to save resized image: {}", e))?;
    Ok(())
}

/// Compute new dimensions that fit within MAX_ANIM_DIM×MAX_ANIM_DIM while
/// preserving aspect ratio. Returns (new_w, new_h).
fn scaled_dims(w: u32, h: u32) -> (u32, u32) {
    if w == 0 || h == 0 {
        return (MAX_ANIM_DIM.min(w), MAX_ANIM_DIM.min(h));
    }
    let max = w.max(h);
    if max <= MAX_ANIM_DIM {
        return (w, h);
    }
    let scale = MAX_ANIM_DIM as f64 / max as f64;
    let nw = (w as f64 * scale).round() as u32;
    let nh = (h as f64 * scale).round() as u32;
    // Guard against zero after rounding.
    (nw.max(1), nh.max(1))
}

/// Validate a custom-character id (defense against path traversal — the id
/// is interpolated into animation filenames). Shared by every character
/// command so the validation rules can't drift apart.
fn validate_char_id(id: &str) -> Result<(), String> {
    if !id.starts_with("char_") || id.contains('/') || id.contains('\\') || id.contains("..") {
        return Err(format!("Invalid character id '{}'", id));
    }
    Ok(())
}

/// Validate a state name used in animation filenames.
fn validate_char_state(state: &str) -> Result<(), String> {
    if !matches!(state, "sleeping" | "working" | "alert") {
        return Err(format!(
            "Invalid state '{}': must be sleeping/working/alert",
            state
        ));
    }
    Ok(())
}

/// Return all characters: built-in Live2D models + user-created animation
/// characters, plus the active character ID.
#[tauri::command]
pub fn get_characters() -> Result<Value, String> {
    let cfg = user_config::load();
    let (models_list, _) = models::get_models();

    let builtins: Vec<Value> = models_list
        .iter()
        .map(|m| {
            // Thumbnail path (if a cached PNG snapshot exists). The frontend
            // converts the absolute path via convertFileSrc (asset protocol).
            let thumb = models::thumbnail_path(&m.name)
                .map(|p| p.to_string_lossy().replace('\\', "/"));
            json!({
                "id": m.url,
                "name": m.name,
                "type": "live2d",
                "url": m.url,
                "userUploaded": m.user_uploaded,
                "thumbnail": thumb,
            })
        })
        .collect();

    let dir = models::animations_dir();
    let dir_str = dir.to_string_lossy().replace('\\', "/");
    let customs: Vec<Value> = cfg
        .custom_characters
        .as_deref()
        .unwrap_or(&[])
        .iter()
        .map(|c| {
            let anim = |f: &Option<String>| f.as_ref().map(|s| format!("{}/{}", dir_str, s));
            // mtime (ms) of each animation file — used by the frontend as a
            // cache-busting query (?v=<mtime>) so re-uploading a GIF (same
            // filename, new content) produces a new URL and the browser
            // re-fetches instead of serving the stale cached image.
            let ver = |f: &Option<String>| -> u64 {
                f.as_ref()
                    .and_then(|s| {
                        std::fs::metadata(dir.join(s))
                            .and_then(|m| m.modified())
                            .ok()
                            .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                            .map(|d| d.as_millis() as u64)
                    })
                    .unwrap_or(0)
            };
            json!({
                "id": c.id,
                "name": c.name,
                "type": "animation",
                "animations": {
                    "sleeping": anim(&c.sleeping),
                    "working": anim(&c.working),
                    "alert": anim(&c.alert),
                },
                "versions": {
                    "sleeping": ver(&c.sleeping),
                    "working": ver(&c.working),
                    "alert": ver(&c.alert),
                }
            })
        })
        .collect();

    let active = cfg
        .active_character_id
        .or_else(|| cfg.model_url.clone())
        .or_else(|| builtins.first().and_then(|m| m["id"].as_str().map(|s| s.to_string())));

    Ok(json!({ "builtin": builtins, "custom": customs, "active": active }))
}

/// Create a new custom character with the given name.
#[tauri::command]
pub fn create_character(name: String) -> Result<Value, String> {
    // Nanosecond timestamp: a millisecond value could collide when two
    // characters are created in the same millisecond (duplicate ids →
    // duplicated config entries and shared animation files).
    let id = format!(
        "char_{}",
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0)
    );
    user_config::update(|cfg| {
        let list = cfg.custom_characters.get_or_insert_with(Vec::new);
        list.push(user_config::CustomCharacter {
            id: id.clone(),
            name: name.clone(),
            ..Default::default()
        });
        cfg.active_character_id = Some(id.clone());
    });
    Ok(json!({ "id": id, "name": name }))
}

/// Delete a custom character and its animation files.
#[tauri::command]
pub fn delete_character(id: String) -> Result<(), String> {
    // Same traversal guard as pick_character_animation — the id builds the
    // filename prefix used for deletion below.
    validate_char_id(&id)?;
    user_config::update(|cfg| {
        if let Some(list) = &mut cfg.custom_characters {
            list.retain(|c| c.id != id);
        }
        if cfg.active_character_id.as_deref() == Some(&id) {
            cfg.active_character_id = None;
        }
    });
    let dir = models::animations_dir();
    if let Ok(entries) = std::fs::read_dir(&dir) {
        for entry in entries.flatten() {
            if let Some(name) = entry.file_name().to_str() {
                if name.starts_with(&format!("{}_", id)) {
                    let _ = std::fs::remove_file(entry.path());
                }
            }
        }
    }
    Ok(())
}

/// Save a Live2D model thumbnail (base64 PNG) captured by the main window
/// after a model loads, so the switch-character menu can show a preview.
/// `name` is the model display name (sanitized to a safe filename).
#[tauri::command]
pub fn save_model_thumbnail(name: String, data: String) -> Result<(), String> {
    let dir = models::thumbnails_dir();
    std::fs::create_dir_all(&dir).map_err(|e| format!("Failed to create thumbnails dir: {}", e))?;
    let safe: String = name
        .chars()
        .map(|c| if c.is_alphanumeric() || c == '-' { c } else { '_' })
        .collect();
    let path = dir.join(format!("{}.png", safe));
    // Strip the "data:image/png;base64," prefix if present.
    let b64 = data
        .strip_prefix("data:image/png;base64,")
        .unwrap_or(&data);
    use base64::Engine;
    let bytes = base64::engine::general_purpose::STANDARD
        .decode(b64)
        .map_err(|e| format!("Failed to decode base64: {}", e))?;
    std::fs::write(&path, bytes).map_err(|e| format!("Failed to write thumbnail: {}", e))?;
    Ok(())
}

/// Read a Live2D model3.json and all referenced files (moc3, textures,
/// physics, display info) and return them as base64-encoded data URLs.
/// The frontend creates blob URLs from these and loads the model without
/// needing HTTP or asset-protocol access (both of which have limitations
/// in the Tauri webview).
#[tauri::command]
pub fn read_live2d_bundle(model_path: String) -> Result<Value, String> {
    let path = PathBuf::from(&model_path);
    let dir = path.parent().ok_or("Invalid model path")?;

    // Read and parse model3.json
    let json_str = std::fs::read_to_string(&path)
        .map_err(|e| format!("Failed to read model3.json: {}", e))?;
    let settings: Value = serde_json::from_str(&json_str)
        .map_err(|e| format!("Failed to parse model3.json: {}", e))?;

    // Collect all referenced files from FileReferences
    use base64::Engine;
    let mut files = serde_json::Map::new();
    let base64_engine = base64::engine::general_purpose::STANDARD;

    let read_file_b64 = |rel: &str| -> Result<String, String> {
        let file_path = dir.join(rel);
        let bytes = std::fs::read(&file_path)
            .map_err(|e| format!("Failed to read {}: {}", rel, e))?;
        Ok(base64_engine.encode(&bytes))
    };

    // Helper to add a file to the bundle
    let add_file = |rel: &str, files: &mut serde_json::Map<String, Value>| {
        if let Ok(b64) = read_file_b64(rel) {
            // Determine MIME type from extension
            let mime = if rel.ends_with(".png") {
                "image/png"
            } else if rel.ends_with(".json") {
                "application/json"
            } else {
                "application/octet-stream"
            };
            files.insert(
                rel.to_string(),
                json!({ "data": b64, "mime": mime }),
            );
        }
    };

    // Moc
    if let Some(moc) = settings
        .get("FileReferences")
        .and_then(|f| f.get("Moc"))
        .and_then(|m| m.as_str())
    {
        add_file(moc, &mut files);
    }
    // Textures
    if let Some(textures) = settings
        .get("FileReferences")
        .and_then(|f| f.get("Textures"))
        .and_then(|t| t.as_array())
    {
        for tex in textures {
            if let Some(tex_path) = tex.as_str() {
                add_file(tex_path, &mut files);
            }
        }
    }
    // Physics (optional)
    if let Some(phys) = settings
        .get("FileReferences")
        .and_then(|f| f.get("Physics"))
        .and_then(|p| p.as_str())
    {
        add_file(phys, &mut files);
    }
    // DisplayInfo (optional)
    if let Some(di) = settings
        .get("FileReferences")
        .and_then(|f| f.get("DisplayInfo"))
        .and_then(|d| d.as_str())
    {
        add_file(di, &mut files);
    }
    // Motions (optional) — each group is an array of {File: "...motion3.json"}
    if let Some(motions) = settings
        .get("FileReferences")
        .and_then(|f| f.get("Motions"))
        .and_then(|m| m.as_object())
    {
        for group in motions.values() {
            if let Some(arr) = group.as_array() {
                for motion in arr {
                    if let Some(file) = motion.get("File").and_then(|f| f.as_str()) {
                        add_file(file, &mut files);
                    }
                }
            }
        }
    }

    Ok(json!({
        "settings": settings,
        "files": files,
    }))
}

/// Open a file picker and save the chosen animation for a character's state.
#[tauri::command]
pub async fn pick_character_animation(
    id: String,
    state: String,
    app: AppHandle,
) -> Result<Value, String> {
    use tauri_plugin_dialog::DialogExt;
    use tokio::sync::oneshot;

    // Security: validate id and state BEFORE opening the file picker or
    // touching the filesystem. Prevents path traversal via crafted id/state.
    validate_char_state(&state)?;
    validate_char_id(&id)?;

    // Set flag so menu window blur handler doesn't hide the menu while
    // the native file picker dialog is open (it steals window focus).
    crate::IS_PICKING_FILE.store(true, std::sync::atomic::Ordering::SeqCst);

    let (tx, rx) = oneshot::channel();
    app.dialog()
        .file()
        .add_filter("Animations", ANIMATION_EXTS)
        .pick_file(move |result| {
            let _ = tx.send(result);
        });

    let file_path = rx.await.map_err(|e| {
        crate::IS_PICKING_FILE.store(false, std::sync::atomic::Ordering::SeqCst);
        format!("channel error: {}", e)
    })?;

    // File picker closed — clear the flag.
    crate::IS_PICKING_FILE.store(false, std::sync::atomic::Ordering::SeqCst);

    let Some(file_path) = file_path else {
        return Ok(json!({ "success": false, "reason": "cancelled" }));
    };

    let source = file_path
        .into_path()
        .map_err(|e| format!("Failed to resolve file path: {}", e))?;
    let dir = models::animations_dir();
    std::fs::create_dir_all(&dir)
        .map_err(|e| format!("Failed to create animations dir: {}", e))?;

    let ext = source.extension().and_then(|e| e.to_str()).unwrap_or("gif");
    let dest_filename = format!("{}_{}.{}", id, state, ext);
    let dest = dir.join(&dest_filename);

    // Write to a temp file first, then rename into place — a failed or
    // interrupted re-encode (GIF with too many frames, oversized frame,
    // disk full) must never leave a truncated animation behind, and the
    // previous file must survive until the new one is fully written.
    // The ".tmp" suffix still matches the cleanup prefix below, so a stale
    // temp file from a crashed upload is removed on the next upload.
    let tmp = dir.join(format!("{}.tmp", dest_filename));

    // Re-encoding a large GIF is CPU-heavy (up to seconds for 500 frames)
    // — run it on a blocking thread so the async runtime's workers (state
    // broadcasts, hook events) stay responsive.
    let src = source.clone();
    let tmp_for_task = tmp.clone();
    let resized =
        tokio::task::spawn_blocking(move || resize_or_copy_animation(&src, &tmp_for_task))
            .await
            .map_err(|e| format!("Animation processing task failed: {}", e))?;
    if let Err(e) = resized {
        let _ = std::fs::remove_file(&tmp);
        return Err(e);
    }

    // Atomic replace (same directory → same volume). WebView2 may hold the
    // old file open without FILE_SHARE_DELETE, which makes rename-over fail
    // on Windows — fall back to unlink + rename in that case.
    if std::fs::rename(&tmp, &dest).is_err() {
        let _ = std::fs::remove_file(&dest);
        std::fs::rename(&tmp, &dest)
            .map_err(|e| format!("Failed to move uploaded animation into place: {}", e))?;
    }

    // The new animation is safely in place — NOW remove old files for this
    // character+state with different extensions (and any stale temp files).
    if let Ok(entries) = std::fs::read_dir(&dir) {
        for entry in entries.flatten() {
            if let Some(name) = entry.file_name().to_str() {
                if name.starts_with(&format!("{}_{}.", id, state)) && name != &dest_filename {
                    let _ = std::fs::remove_file(entry.path());
                }
            }
        }
    }

    user_config::update(|cfg| {
        if let Some(list) = &mut cfg.custom_characters {
            if let Some(c) = list.iter_mut().find(|c| c.id == id) {
                match state.as_str() {
                    "sleeping" => c.sleeping = Some(dest_filename.clone()),
                    "working" => c.working = Some(dest_filename.clone()),
                    "alert" => c.alert = Some(dest_filename.clone()),
                    _ => {}
                }
            }
        }
    });

    let dest_path = dest.to_string_lossy().replace('\\', "/");
    Ok(json!({ "success": true, "path": dest_path, "id": id, "state": state }))
}

/// Clear a character's animation for a specific state.
#[tauri::command]
pub fn clear_character_animation(id: String, state: String) -> Result<(), String> {
    // Same traversal guards as pick_character_animation — both values are
    // interpolated into the filename prefix used for deletion below.
    validate_char_id(&id)?;
    validate_char_state(&state)?;
    user_config::update(|cfg| {
        if let Some(list) = &mut cfg.custom_characters {
            if let Some(c) = list.iter_mut().find(|c| c.id == id) {
                match state.as_str() {
                    "sleeping" => c.sleeping = None,
                    "working" => c.working = None,
                    "alert" => c.alert = None,
                    _ => {}
                }
            }
        }
    });
    let dir = models::animations_dir();
    if let Ok(entries) = std::fs::read_dir(&dir) {
        for entry in entries.flatten() {
            if let Some(name) = entry.file_name().to_str() {
                if name.starts_with(&format!("{}_{}.", id, state)) {
                    let _ = std::fs::remove_file(entry.path());
                }
            }
        }
    }
    Ok(())
}

/// Switch to a character by ID (built-in model URL or custom character ID).
#[tauri::command]
pub fn switch_character(id: String) {
    user_config::update(|cfg| {
        cfg.active_character_id = Some(id.clone());
        if id.starts_with("char_") {
            cfg.model_url = None;
        } else {
            cfg.model_url = Some(id.clone());
        }
    });
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

/// Shared snap detection used by `detect_edge_dock`, `enter_edge_dock` and the
/// drag-time ghost preview: returns the monitor + edge the window should dock
/// to.
///
/// **Algorithm:** "Most area wins" — first pick the monitor that holds the
/// largest area of the window (`best_monitor`), then check whether the window
/// extends past that monitor's left/right edge by more than 20% of its width.
/// This is the STABLE choice across drag positions:
///
///   - On OUTER edges (no neighbour): a window mostly on screen A that bleeds
///     past A's outer edge snaps to that edge — correct.
///   - On INTERNAL boundaries (A's right touches B's left at x=X): when the
///     window crosses x=X, `best_monitor` flips to the other side ONLY when
///     more than half the window is on that side. Before the flip we snap to
///     the originating edge; after the flip we snap to the new edge — B's
///     "right" result is then normalised to the neighbour's "left" so both
///     sides of the same physical boundary resolve to the same `(monitor,
///     "left")` pair, preventing the bar from jumping between two positions.
///
/// **Hysteresis:** pass `hysteresis_px > 0` when the preview/dock is already
/// active — the threshold is lowered so the bar doesn't flicker on/off when
/// the window wobbles right at the boundary during a drag.
fn snap_target<'a>(
    monitors: &'a [tauri::Monitor],
    pos: tauri::PhysicalPosition<i32>,
    size: tauri::PhysicalSize<u32>,
    hysteresis_px: i64,
) -> Option<(&'a tauri::Monitor, &'static str)> {
    let mon = best_monitor(monitors, pos, size)?;
    let mp = mon.position();
    let ms = mon.size();
    let threshold = (size.width as f64 * 0.2).round() as i64;
    let min_cross = if hysteresis_px > 0 {
        (threshold - hysteresis_px).max(5)
    } else {
        threshold
    };
    let cross_left = (mp.x - pos.x) as i64;
    let cross_right = (pos.x + size.width as i32 - (mp.x + ms.width as i32)) as i64;

    let edge = match (cross_left > min_cross, cross_right > min_cross) {
        (true, true) => {
            if cross_left >= cross_right { "left" } else { "right" }
        }
        (true, false) => "left",
        (false, true) => "right",
        (false, false) => return None,
    };

    // Internal boundary normalisation: if we'd dock to this monitor's RIGHT
    // edge and another monitor starts exactly there (same y), dock to that
    // neighbour's LEFT edge instead — the bar sits on the boundary, and both
    // sides of the boundary resolve to the same (monitor, "left") answer.
    if edge == "right" {
        let boundary = mp.x + ms.width as i32;
        if let Some(neighbour) = monitors.iter().find(|m| {
            let p = m.position();
            p.x == boundary && p.y == mp.y
        }) {
            return Some((neighbour, "left"));
        }
    }
    // Symmetric: if we'd dock to this monitor's LEFT edge and another monitor
    // ends exactly there, this is the same internal boundary — return self
    // as "left" of this monitor (the right-edge-of-neighbour case was already
    // converted above when the window was on the neighbour). No-op here
    // because returning (mon, "left") is already the canonical answer.

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
    let result = snap_target(&monitors, pos, size, 0);
    log::info!(
        "[edgeDock] detect: pos=({},{}) size={}x{} monitors={} => {:?}",
        pos.x, pos.y, size.width, size.height,
        monitors.len(),
        result.map(|(m, e)| {
            let p = m.position();
            let s = m.size();
            format!("(mon [{},{} {}x{}], {})", p.x, p.y, s.width, s.height, e)
        })
    );
    Ok(result.map(|(_, edge)| edge.to_string()))
}

/// Snap the window into a compact bar at the given left/right edge. The bar
/// is EDGE_DOCK_THICKNESS wide and as tall as its content (the renderer
/// passes `content_height` in logical px, clamped to sane bounds), vertically
/// centered on where the window was dropped. Remembers the previous rect and
/// edge for `exit_edge_dock`.
///
/// **Multi-monitor consistency:** The frontend calls `detect_edge_dock` first
/// and passes the returned edge here. But `detect_edge_dock` and this function
/// each called `best_monitor` independently — when the window straddles a
/// boundary near the 50/50 overlap point, a 1 px shift between the two IPC
/// calls can flip `best_monitor` to the neighbouring screen. Using the stale
/// edge with the new monitor placed the bar on the WRONG edge of the WRONG
/// monitor (e.g. the far right of monitor 2 instead of the boundary), making
/// the window appear to "disappear". Fix: re-run `snap_target` here so the
/// monitor and edge are always resolved together; fall back to the
/// frontend-provided edge only if the window has drifted out of threshold.
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

    let monitors = app.available_monitors().map_err(|e| e.to_string())?;
    let cy = pos.y + size.height as i32 / 2;

    // Resolve monitor + edge TOGETHER via snap_target so they can never
    // disagree. If snap_target returns None (the window drifted between
    // detect and enter, or sits right at a multi-monitor boundary where the
    // majority flipped to the neighbouring screen), search ALL monitors for
    // the largest edge crossing and dock there. Blindly pairing best_monitor
    // with the frontend-provided edge can place the bar on the WRONG
    // monitor's OUTER edge — e.g. when the window is mostly on the left
    // screen but the frontend said "left" (meaning the right screen's left
    // edge), the bar would jump to the left screen's far-left outer edge,
    // making the window appear to "disappear". If no edge crossing is found
    // on any monitor, skip the dock entirely (better no snap than a wrong one).
    let (mon, resolved_edge) = match snap_target(&monitors, pos, size, 0) {
        Some((m, e)) => (m, e.to_string()),
        None => {
            // Fallback: detect/enter drifted by a few px and we're just below
            // threshold. Use the same best_monitor logic but with a minimal
            // threshold (5px) so we don't snap to a completely wrong edge.
            // The old "max cross over all monitors" search is what caused the
            // bar to jump between two screens at internal boundaries; using
            // best_monitor keeps the choice consistent with snap_target.
            let Some(m) = best_monitor(&monitors, pos, size) else {
                log::info!(
                    "[edgeDock] fallback: no monitors at ({},{}) — skipping (fe edge='{}')",
                    pos.x, pos.y, edge
                );
                return Ok(());
            };
            let mp = m.position();
            let ms = m.size();
            let cl = (mp.x - pos.x) as i64;
            let cr = (pos.x + size.width as i32 - (mp.x + ms.width as i32)) as i64;
            let fe = match (cl > 5, cr > 5) {
                (true, true) => if cl >= cr { "left" } else { "right" },
                (true, false) => "left",
                (false, true) => "right",
                (false, false) => {
                    log::info!(
                        "[edgeDock] fallback: no edge crossing at ({},{}) on mon ({},{}) — skipping (fe edge='{}')",
                        pos.x, pos.y, mp.x, mp.y, edge
                    );
                    return Ok(());
                }
            };
            log::info!(
                "[edgeDock] fallback: win at ({},{}) {}w => mon ({},{}) {}x{} {} (fe edge='{}')",
                pos.x, pos.y, size.width, mp.x, mp.y, ms.width, ms.height, fe, edge
            );
            (m, fe.to_string())
        }
    };
    let mp = mon.position();
    let ms = mon.size();
    // Use the TARGET monitor's DPI factor, not the window's current one —
    // the window may still be on a different-DPI screen when this runs.
    let factor = mon.scale_factor();

    // Vertically centered on the drop position, clamped inside the monitor.
    let (x, y, w, h) = dock_bar_rect(mp, ms, &resolved_edge, factor, cy, content_height);
    log::info!(
        "[edgeDock] enter: mon=({},{}) {}x{} edge={} => bar=({},{}) {}x{} (fe edge='{}')",
        mp.x, mp.y, ms.width, ms.height, resolved_edge,
        x, y, w, h, edge
    );

    // Set active BEFORE changing window geometry so the Moved/Resized event
    // handlers (which also run during set_size/set_position) see the docked
    // state and don't try to re-save or re-clamp the window mid-transition.
    // The old code set active AFTER set_size/set_position, which left a race
    // window where the ScaleFactorChanged clamp could restore the window to
    // full size before the dock completed.
    *state.docked.lock().unwrap() =
        Some(((pos.x, pos.y, size.width, size.height), resolved_edge.clone()));
    state.active.store(true, Ordering::SeqCst);

    window
        .set_size(tauri::Size::Physical(tauri::PhysicalSize::new(w, h)))
        .map_err(|e| e.to_string())?;
    window
        .set_position(tauri::Position::Physical(tauri::PhysicalPosition::new(x, y)))
        .map_err(|e| e.to_string())?;
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
    let monitors = app.available_monitors().map_err(|e| e.to_string())?;

    // Hysteresis: once the preview is visible, require LESS crossing to keep
    // it showing (20px physical of "grace") so the bar doesn't flicker on/off
    // at the exact threshold pixel as the mouse wiggles during a drag.
    let already_visible = preview.is_visible().unwrap_or(false);
    let hyst = if already_visible { 20i64 } else { 0 };
    let Some((mon, edge)) = snap_target(&monitors, pos, size, hyst) else {
        // Back inside the threshold — no snap would happen, drop the ghost.
        let _ = preview.hide();
        return Ok(());
    };
    let cy = pos.y + size.height as i32 / 2;
    // Use the TARGET monitor's DPI for consistent bar size across screens.
    let factor = mon.scale_factor();
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
    let mut y = y;
    // If the menu would extend past the bottom of the screen, shift it up
    // so the full height is visible.
    if let Ok(Some(monitor)) = menu_win.current_monitor() {
        let scale = monitor.scale_factor();
        let screen_h = monitor.size().height as f64 / scale;
        if y + h + 8.0 > screen_h {
            y = (screen_h - h - 8.0).max(0.0);
        }
    }
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

/// Resize the menu window without repositioning or focusing it. Called by the
/// menu window itself to fit its height to the current view's content after
/// switching submenus or loading dynamic items (e.g. the character grid with
/// 5+ cards needs more height than the main menu).
#[tauri::command]
pub fn resize_menu_window(app: AppHandle, w: f64, h: f64) -> Result<(), String> {
    let Some(menu_win) = app.get_webview_window("menu") else {
        return Ok(());
    };
    menu_win
        .set_size(tauri::Size::Logical(tauri::LogicalSize::new(w, h)))
        .map_err(|e| e.to_string())?;
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
