//! Server-side click-through for the transparent pet window.
//!
//! Tauri's `set_ignore_cursor_events` has no Electron-style `{forward:true}`
//! mode: once the webview is told to ignore the cursor it receives zero mouse
//! events, so a frontend `mousemove`-driven toggle deadlocks the window into
//! permanent click-through. Instead, a Rust polling thread reads the global
//! cursor position via a platform API and toggles `set_ignore_cursor_events`
//! itself.
//!
//! Platform cursor source:
//!   - Windows: Win32 `GetCursorPos`
//!   - macOS:   CoreGraphics `CGEventCreate(NULL)` + `CGEventGetLocation`
//!   - Linux:   X11 `XQueryPointer` via `x11rb` (Wayland has no global cursor
//!              API — connection failure makes the loop bail, leaving the
//!              window always-clickable; see Wayland note below)
//!
//! The frontend reports the "clickable rectangles" (model bounds, status bar,
//! context menu) via the `update_click_regions` IPC, plus a `force_clickable`
//! flag (drag / menu-open) via `set_force_clickable`. The polling thread then
//! decides: cursor outside window → clickable; force_clickable → clickable;
//! cursor inside any reported rect → clickable; otherwise → pass through.
//!
//! The PixiJS ticker (rAF) keeps running while the window is click-through, so
//! the frontend keeps reporting regions even with no mouse input.

use serde::Deserialize;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;
use tauri::WebviewWindow;

#[cfg(windows)]
use windows::Win32::Foundation::POINT;
#[cfg(windows)]
use windows::Win32::UI::WindowsAndMessaging::GetCursorPos;

/// A clickable rectangle in physical pixels, relative to the window's
/// top-left. The window is undecorated so outer == inner; these map directly
/// to the cursor-local coords computed in the polling loop. Deserialized from
/// the frontend's `update_click_regions` IPC.
#[derive(Clone, Copy, Debug, Deserialize)]
pub struct ClickRegion {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

/// Shared, cloneable state managed by Tauri and read by the polling thread.
/// All fields are `Arc` so a clone is cheap and the whole thing is
/// `Send + Sync + 'static` (required by `app.manage` + `std::thread::spawn`).
#[derive(Clone)]
pub struct ClickThroughState {
    /// Regions that should NOT pass clicks through (model / status bar / menu).
    pub regions: Arc<Mutex<Vec<ClickRegion>>>,
    /// When true, the window stays clickable regardless of cursor position
    /// (used while dragging or while the menu is open).
    pub force_clickable: Arc<AtomicBool>,
    /// Last ignore value we applied — read in the loop to dedupe
    /// `set_ignore_cursor_events` calls (only toggle on change).
    pub current_ignore: Arc<AtomicBool>,
}

impl ClickThroughState {
    pub fn new() -> Self {
        Self {
            regions: Arc::new(Mutex::new(Vec::new())),
            // `force_clickable` starts false; `current_ignore` starts false so
            // the window is fully clickable until the first poll with regions
            // decides otherwise (covers startup before the frontend's first
            // region report).
            force_clickable: Arc::new(AtomicBool::new(false)),
            current_ignore: Arc::new(AtomicBool::new(false)),
        }
    }
}

impl Default for ClickThroughState {
    fn default() -> Self {
        Self::new()
    }
}

/// 30ms poll interval — balances responsiveness (<50ms perceived) with CPU.
const POLL_INTERVAL_MS: u64 = 30;

/// Shared hit-test: given the global cursor (cx, cy) and the window's outer
/// rect (wx, wy, ww, wh) in physical screen pixels, decide whether the window
/// should ignore cursor events. Identical semantics on every platform — only
/// the cursor-source differs.
#[inline]
fn compute_target_ignore(
    state: &ClickThroughState,
    cx: i32,
    cy: i32,
    wx: i32,
    wy: i32,
    ww: i32,
    wh: i32,
) -> bool {
    if cx < wx || cy < wy || cx >= wx + ww || cy >= wy + wh {
        // Cursor outside the window — nothing to pass through, stay clickable
        // (also ensures the moment the cursor re-enters the model the window
        // is already receiving events, no 30ms delay).
        return false;
    }
    if state.force_clickable.load(Ordering::SeqCst) {
        return false;
    }
    let lx = cx - wx;
    let ly = cy - wy;
    let regions = state.regions.lock().unwrap_or_else(|e| e.into_inner());
    let hit = regions
        .iter()
        .any(|r| lx >= r.x && lx < r.x + r.width && ly >= r.y && ly < r.y + r.height);
    !hit
}

/// Apply the ignore decision, toggling `set_ignore_cursor_events` only when the
/// value changes (avoids hitting the platform window API every 30ms).
#[inline]
fn apply_ignore(window: &WebviewWindow, state: &ClickThroughState, target_ignore: bool) {
    let prev = state.current_ignore.load(Ordering::SeqCst);
    if target_ignore != prev {
        let _ = window.set_ignore_cursor_events(target_ignore);
        state.current_ignore.store(target_ignore, Ordering::SeqCst);
    }
}

/// Polling loop — owns all `set_ignore_cursor_events` calls at runtime.
/// Runs for the lifetime of the app. Started from `setup()` in `lib.rs` on a
/// dedicated `std::thread`. Exits silently if window-handle queries error.
#[cfg(windows)]
pub fn run_polling_loop(window: WebviewWindow, state: ClickThroughState) {
    loop {
        std::thread::sleep(Duration::from_millis(POLL_INTERVAL_MS));

        // Global cursor position in physical screen pixels.
        let mut point = POINT::default();
        // windows 0.58: GetCursorPos is unsafe and returns Result<(), Error>.
        if unsafe { GetCursorPos(&mut point) }.is_err() {
            continue;
        }
        let (cx, cy) = (point.x, point.y);

        // Window outer rect in physical screen pixels. The window is undecorated
        // so outer == inner; cursor-local coords map directly to the
        // window-local physical-pixel rects the frontend reports.
        let (wx, wy) = match window.outer_position() {
            Ok(p) => (p.x, p.y),
            Err(_) => continue,
        };
        let (ww, wh) = match window.outer_size() {
            Ok(s) => (s.width as i32, s.height as i32),
            Err(_) => continue,
        };

        let target_ignore = compute_target_ignore(&state, cx, cy, wx, wy, ww, wh);
        apply_ignore(&window, &state, target_ignore);
    }
}

/// macOS polling loop — CoreGraphics CGEvent cursor location.
#[cfg(target_os = "macos")]
pub fn run_polling_loop(window: WebviewWindow, state: ClickThroughState) {
    use core_graphics::event::CGEvent;

    loop {
        std::thread::sleep(Duration::from_millis(POLL_INTERVAL_MS));

        // CGEventCreate(NULL) initializes a fresh event at the current mouse
        // location; CGEventGetLocation then returns that location in global
        // display coords (primary display top-left origin, y-down) — which
        // matches Tauri's outer_position/outer_size physical-pixel space on
        // macOS, so no Y-flip is needed.
        let loc = CGEvent::new(None).ok().map(|e| e.location());
        let (cx, cy) = match loc {
            Some(p) => (p.x as i32, p.y as i32),
            None => continue,
        };

        let (wx, wy) = match window.outer_position() {
            Ok(p) => (p.x, p.y),
            Err(_) => continue,
        };
        let (ww, wh) = match window.outer_size() {
            Ok(s) => (s.width as i32, s.height as i32),
            Err(_) => continue,
        };

        let target_ignore = compute_target_ignore(&state, cx, cy, wx, wy, ww, wh);
        apply_ignore(&window, &state, target_ignore);
    }
}

/// Linux polling loop — X11 XQueryPointer via x11rb.
///
/// Wayland note: Wayland forbids clients from querying the global cursor
/// position. If `x11rb::connect` fails (no X server — i.e. a pure Wayland or
/// headless session) the loop logs once and returns, leaving `current_ignore`
/// at its initial `false` so the window stays always-clickable. This is the
/// graceful-degradation path; sessions still work via hook events.
#[cfg(target_os = "linux")]
pub fn run_polling_loop(window: WebviewWindow, state: ClickThroughState) {
    use x11rb::protocol::xproto::ConnectionExt as _;

    let (conn, screen_num) = match x11rb::connect(None) {
        Ok(c) => c,
        Err(_) => {
            log::warn!(
                "[clickThrough] X11 connect failed; click-through disabled (likely Wayland/headless). Window stays always-clickable."
            );
            return;
        }
    };
    let root = {
        let setup = conn.setup();
        setup.roots[screen_num].root
    };

    loop {
        std::thread::sleep(Duration::from_millis(POLL_INTERVAL_MS));

        // XQueryPointer root_x/root_y are in X screen coords — the same space
        // Tauri's outer_position uses on X11.
        let reply = match conn.query_pointer(root) {
            Ok(cookie) => match cookie.reply() {
                Ok(r) => r,
                Err(_) => continue,
            },
            Err(_) => continue,
        };
        let (cx, cy) = (reply.root_x as i32, reply.root_y as i32);

        let (wx, wy) = match window.outer_position() {
            Ok(p) => (p.x, p.y),
            Err(_) => continue,
        };
        let (ww, wh) = match window.outer_size() {
            Ok(s) => (s.width as i32, s.height as i32),
            Err(_) => continue,
        };

        let target_ignore = compute_target_ignore(&state, cx, cy, wx, wy, ww, wh);
        apply_ignore(&window, &state, target_ignore);
    }
}

/// Fallback for any other platform: no click-through (window always clickable).
#[cfg(not(any(windows, target_os = "macos", target_os = "linux")))]
pub fn run_polling_loop(_window: WebviewWindow, _state: ClickThroughState) {}
