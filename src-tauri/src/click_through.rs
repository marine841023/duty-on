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

/// One-shot read of the global cursor position in physical screen pixels
/// (used by exit_edge_dock to put the pet under the pointer). None when the
/// platform has no global cursor API.
#[cfg(windows)]
pub fn global_cursor_pos() -> Option<(i32, i32)> {
    let mut point = POINT::default();
    // windows 0.58: GetCursorPos is unsafe and returns Result<(), Error>.
    if unsafe { GetCursorPos(&mut point) }.is_err() {
        return None;
    }
    Some((point.x, point.y))
}

#[cfg(not(windows))]
pub fn global_cursor_pos() -> Option<(i32, i32)> {
    None
}

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

/// Core polling logic shared across all platforms.
/// Gets the window position/size, computes the ignore target, and applies it.
/// Only the cursor-source differs per platform; this handles the rest.
fn poll_once(
    window: &WebviewWindow,
    state: &ClickThroughState,
    cursor_x: i32,
    cursor_y: i32,
) {
    if let Ok(pos) = window.outer_position() {
        if let Ok(size) = window.outer_size() {
            let target = compute_target_ignore(
                state,
                cursor_x,
                cursor_y,
                pos.x,
                pos.y,
                size.width as i32,
                size.height as i32,
            );
            apply_ignore(window, state, target);
        }
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

        poll_once(&window, &state, cx, cy);
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

        poll_once(&window, &state, cx, cy);
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

        poll_once(&window, &state, cx, cy);
    }
}

/// Fallback for any other platform: no click-through (window always clickable).
#[cfg(not(any(windows, target_os = "macos", target_os = "linux")))]
pub fn run_polling_loop(_window: WebviewWindow, _state: ClickThroughState) {}

#[cfg(test)]
mod tests {
    //! Unit tests for `compute_target_ignore`.
    //!
    //! Semantics recap (matches the module-level doc: "cursor inside any
    //! reported rect → clickable; otherwise → pass through"):
    //!   - returns `false` (NOT ignored / window stays clickable) when the
    //!     cursor is outside the window, when `force_clickable` is set, or when
    //!     the cursor is inside a reported `ClickRegion`;
    //!   - returns `true` (ignored / clicks pass THROUGH the window) when the
    //!     cursor is inside the window but not inside any reported region
    //!     (including the zero-region case — the whole window area passes
    //!     through until the frontend reports model/menu rects).
    //!
    //! The helper below builds an isolated `ClickThroughState` per case so the
    //! `Arc<Mutex<…>>` / `Arc<AtomicBool>` fields are never shared between tests.

    use super::*;

    /// Build a state with the given regions (in local/window coordinates) and
    /// `force_clickable` flag. Each call yields an independent state.
    fn make_state(regions: &[ClickRegion], force: bool) -> ClickThroughState {
        let state = ClickThroughState::new();
        {
            let mut g = state.regions.lock().unwrap();
            g.extend_from_slice(regions);
        }
        state.force_clickable.store(force, Ordering::SeqCst);
        state
    }

    // Window rect used across tests: origin (100, 100), size 200x200, so it
    // covers screen x∈[100,300) and y∈[100,300).
    const WX: i32 = 100;
    const WY: i32 = 100;
    const WW: i32 = 200;
    const WH: i32 = 200;

    #[test]
    fn cursor_outside_window_stays_clickable() {
        let state = make_state(&[], false);
        // above the window
        assert!(!compute_target_ignore(&state, 150, 50, WX, WY, WW, WH));
        // left of the window
        assert!(!compute_target_ignore(&state, 50, 150, WX, WY, WW, WH));
        // bottom-right outer edge is exclusive → counts as outside
        assert!(!compute_target_ignore(&state, 300, 300, WX, WY, WW, WH));
        // far outside
        assert!(!compute_target_ignore(&state, 0, 0, WX, WY, WW, WH));
    }

    #[test]
    fn inside_window_no_regions_passes_through() {
        // Zero reported regions → nothing is clickable, so the whole window
        // area passes clicks through (ignore == true).
        let state = make_state(&[], false);
        assert!(compute_target_ignore(&state, 150, 150, WX, WY, WW, WH));
        // top-left corner of the window (inclusive lower bound) is inside
        assert!(compute_target_ignore(&state, 100, 100, WX, WY, WW, WH));
    }

    #[test]
    fn inside_window_hitting_region_stays_clickable() {
        // Region in local coords: x∈[10,60), y∈[10,60) → screen [110,160).
        let region = ClickRegion { x: 10, y: 10, width: 50, height: 50 };
        let state = make_state(&[region], false);
        // center of the region (local 35,35 → screen 135,135)
        assert!(!compute_target_ignore(&state, 135, 135, WX, WY, WW, WH));
        // top-left corner of the region (local 10,10 → screen 110,110, inclusive)
        assert!(!compute_target_ignore(&state, 110, 110, WX, WY, WW, WH));
    }

    #[test]
    fn inside_window_missing_region_passes_through() {
        let region = ClickRegion { x: 10, y: 10, width: 50, height: 50 };
        let state = make_state(&[region], false);
        // local (80,80) → screen (180,180): clearly outside the region
        assert!(compute_target_ignore(&state, 180, 180, WX, WY, WW, WH));
        // just past the right edge (local 60,35 → screen 160,135): exclusive
        assert!(compute_target_ignore(&state, 160, 135, WX, WY, WW, WH));
        // just past the bottom edge (local 35,60 → screen 135,160): exclusive
        assert!(compute_target_ignore(&state, 135, 160, WX, WY, WW, WH));
    }

    #[test]
    fn force_clickable_keeps_window_clickable() {
        // No regions → would normally pass through, but force wins → clickable.
        let state = make_state(&[], true);
        assert!(!compute_target_ignore(&state, 150, 150, WX, WY, WW, WH));

        // Force also overrides the "inside window but missing a region" case.
        let region = ClickRegion { x: 10, y: 10, width: 50, height: 50 };
        let state2 = make_state(&[region], true);
        assert!(!compute_target_ignore(&state2, 180, 180, WX, WY, WW, WH));
    }

    #[test]
    fn multiple_regions_each_can_be_hit() {
        let regions = vec![
            ClickRegion { x: 0, y: 0, width: 40, height: 40 },     // top-left
            ClickRegion { x: 160, y: 160, width: 40, height: 40 }, // bottom-right
        ];
        let state = make_state(&regions, false);
        // hit first region (local 20,20 → screen 120,120)
        assert!(!compute_target_ignore(&state, 120, 120, WX, WY, WW, WH));
        // hit second region (local 180,180 → screen 280,280)
        assert!(!compute_target_ignore(&state, 280, 280, WX, WY, WW, WH));
        // hit neither (local 100,100 → screen 200,200)
        assert!(compute_target_ignore(&state, 200, 200, WX, WY, WW, WH));
    }

    #[test]
    fn current_ignore_field_is_not_read_by_compute() {
        // `compute_target_ignore` must depend only on cursor/regions/force, NOT
        // on the dedupe `current_ignore` latch — flipping it must not change the
        // result for an otherwise pass-through position.
        let state = make_state(&[], false);
        state.current_ignore.store(true, Ordering::SeqCst);
        assert!(compute_target_ignore(&state, 150, 150, WX, WY, WW, WH));
        state.current_ignore.store(false, Ordering::SeqCst);
        assert!(compute_target_ignore(&state, 150, 150, WX, WY, WW, WH));
    }
}
