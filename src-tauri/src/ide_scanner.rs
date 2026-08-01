//! IDE window scanner — detects open Trae IDE windows by title.
//!
//! Per platform:
//!   - Windows: Win32 `EnumWindows` + `GetWindowTextW`
//!   - macOS:   CoreGraphics `CGWindowListCopyWindowInfo` (raw FFI)
//!   - Linux:   X11 `XQueryTree` + `_NET_WM_NAME` via `x11rb` (Wayland has no
//!              global window-list API → connection failure returns empty Vec;
//!              sessions still work via hook events)
//!
//! Window title format: "<active-file> - <project-folder> - Trae CN".
//! The "* - Trae CN" suffix filters non-IDE windows; the project is the
//! second-to-last " - " segment. Used to (a) register idle sessions for newly
//! opened projects and (b) prune sessions whose IDE window closed (Trae fires
//! no hook on window close).

use crate::config;

/// True if the window title belongs to a supported IDE (Trae CN or Qoder).
/// Shared across platforms.
fn is_ide_title(title: &str) -> bool {
    title.ends_with(config::TRAE_TITLE_SUFFIX) || title.ends_with(config::QODER_TITLE_SUFFIX)
}

/// Parse the project folder name from a supported IDE window title. Returns
/// None for non-IDE windows or generic titles. Shared across platforms.
fn parse_project_name(title: &str) -> Option<&str> {
    if !is_ide_title(title) {
        return None;
    }
    let parts: Vec<&str> = title.split(" - ").collect();
    if parts.len() < 2 {
        return None;
    }
    let folder = parts[parts.len() - 2].trim();
    if folder.is_empty()
        || folder == "Trae"
        || folder == "Trae CN"
        || folder == "Qoder"
    {
        return None;
    }
    Some(folder)
}

/// From a list of all on-screen window titles, extract the deduped, sorted
/// Trae project names. Shared across platforms.
fn extract_trae_projects(titles: &[String]) -> Vec<String> {
    let mut names: Vec<String> = titles
        .iter()
        .filter_map(|t| parse_project_name(t).map(|s| s.to_string()))
        .collect();
    names.sort();
    names.dedup();
    names
}

// ============================================================================
// Windows — EnumWindows
// ============================================================================

#[cfg(windows)]
use windows::Win32::Foundation::{BOOL, HWND, LPARAM};
#[cfg(windows)]
use windows::Win32::UI::WindowsAndMessaging::{
    EnumWindows, GetWindowTextW, GetWindowTextLengthW, IsIconic, IsWindowVisible,
    SetForegroundWindow, ShowWindowAsync, SW_RESTORE,
};

#[cfg(windows)]
struct FocusCtx {
    name: String,
    best: *mut Option<HWND>,
    substring: *mut Option<HWND>,
}

/// Focus the Trae IDE window whose project segment matches `name`.
#[cfg(windows)]
pub fn focus_project_window(name: &str) -> bool {
    let mut best: Option<HWND> = None;
    let mut substring_match: Option<HWND> = None;
    let ctx = FocusCtx {
        name: name.to_string(),
        best: &mut best,
        substring: &mut substring_match,
    };
    let lparam = LPARAM(&ctx as *const FocusCtx as isize);
    unsafe {
        let _ = EnumWindows(Some(focus_proc), lparam);
    }

    let target = best.or(substring_match);
    if let Some(hwnd) = target {
        unsafe {
            if IsIconic(hwnd).as_bool() {
                let _ = ShowWindowAsync(hwnd, SW_RESTORE);
            }
            let _ = SetForegroundWindow(hwnd);
        }
        true
    } else {
        false
    }
}

#[cfg(windows)]
unsafe extern "system" fn focus_proc(hwnd: HWND, lparam: LPARAM) -> BOOL {
    let ctx = &*(lparam.0 as *const FocusCtx);
    if !IsWindowVisible(hwnd).as_bool() {
        return BOOL(1);
    }
    let len = GetWindowTextLengthW(hwnd);
    if len <= 0 {
        return BOOL(1);
    }
    let mut buf = vec![0u16; (len as usize) + 1];
    let n = GetWindowTextW(hwnd, &mut buf);
    if n <= 0 {
        return BOOL(1);
    }
    let title = String::from_utf16_lossy(&buf[..n as usize]);
    if !is_ide_title(&title) {
        return BOOL(1);
    }
    let parts: Vec<&str> = title.split(" - ").collect();
    let folder = if parts.len() >= 2 {
        parts[parts.len() - 2].trim()
    } else {
        return BOOL(1);
    };
    if folder.eq_ignore_ascii_case(&ctx.name) {
        *ctx.best = Some(hwnd);
    } else if title.to_lowercase().contains(&ctx.name.to_lowercase())
        && (*ctx.best).is_none()
        && (*ctx.substring).is_none()
    {
        *ctx.substring = Some(hwnd);
    }
    BOOL(1)
}

/// Scan all visible top-level windows and return the project names of every
/// Trae IDE window (deduped, generic titles filtered out).
#[cfg(windows)]
pub fn scan_trae_projects() -> Vec<String> {
    let mut titles: Vec<String> = Vec::new();
    let lparam = LPARAM(&mut titles as *mut Vec<String> as isize);
    unsafe {
        let _ = EnumWindows(Some(enum_proc), lparam);
    }
    extract_trae_projects(&titles)
}

#[cfg(windows)]
unsafe extern "system" fn enum_proc(hwnd: HWND, lparam: LPARAM) -> BOOL {
    let titles = &mut *(lparam.0 as *mut Vec<String>);
    if IsWindowVisible(hwnd).as_bool() {
        let len = GetWindowTextLengthW(hwnd);
        if len > 0 {
            let mut buf = vec![0u16; (len as usize) + 1];
            let n = GetWindowTextW(hwnd, &mut buf);
            if n > 0 {
                let title = String::from_utf16_lossy(&buf[..n as usize]);
                titles.push(title);
            }
        }
    }
    BOOL(1) // continue enumeration
}

// ============================================================================
// macOS — CoreGraphics CGWindowListCopyWindowInfo (raw FFI)
// ============================================================================

#[cfg(target_os = "macos")]
mod cg {
    use std::ffi::CStr;
    use std::os::raw::{c_char, c_int, c_void};

    pub type CFRef = *const c_void;

    extern "C" {
        fn CGWindowListCopyWindowInfo(option: u32, relative_to_window: u32) -> CFRef;
        fn CFArrayGetCount(arr: CFRef) -> isize;
        fn CFArrayGetValueAtIndex(arr: CFRef, idx: isize) -> CFRef;
        fn CFDictionaryGetValue(dict: CFRef, key: CFRef) -> CFRef;
        fn CFRelease(cf: CFRef);
        // kCGWindowName is a global CFString constant exported by CoreGraphics.
        static kCGWindowName: CFRef;
        fn CFStringGetTypeID() -> usize;
        fn CFGetTypeID(cf: CFRef) -> usize;
        fn CFStringGetCStringPtr(s: CFRef, encoding: u32) -> *const c_char;
        fn CFStringGetCString(s: CFRef, buffer: *mut c_char, buffer_size: isize, encoding: u32) -> c_int;
    }

    // kCGWindowListOptionOnScreenOnly == 1 << 0.
    const LIST_ONSCREEN: u32 = 1;
    // kCFStringEncodingUTF8.
    const UTF8: u32 = 0x0800_0100;

    /// Return the titles of all on-screen windows. Empty on failure or when
    /// Screen Recording permission is denied (titles come back blank — graceful).
    pub fn on_screen_window_titles() -> Vec<String> {
        let mut out = Vec::new();
        unsafe {
            let arr = CGWindowListCopyWindowInfo(LIST_ONSCREEN, 0);
            if arr.is_null() {
                return out;
            }
            let count = CFArrayGetCount(arr);
            let str_type = CFStringGetTypeID();
            for i in 0..count {
                let dict = CFArrayGetValueAtIndex(arr, i);
                if dict.is_null() {
                    continue;
                }
                let name_val = CFDictionaryGetValue(dict, kCGWindowName);
                if name_val.is_null() || CFGetTypeID(name_val) != str_type {
                    continue;
                }
                if let Some(s) = cfstring_to_string(name_val) {
                    out.push(s);
                }
            }
            CFRelease(arr);
        }
        out
    }

    unsafe fn cfstring_to_string(s: CFRef) -> Option<String> {
        // Fast path: direct C-string pointer.
        let ptr = CFStringGetCStringPtr(s, UTF8);
        if !ptr.is_null() {
            return CStr::from_ptr(ptr).to_str().ok().map(|s| s.to_string());
        }
        // Slow path: copy into a stack buffer.
        let mut buf = [0 as c_char; 1024];
        if CFStringGetCString(s, buf.as_mut_ptr(), buf.len() as isize, UTF8) != 0 {
            return CStr::from_ptr(buf.as_ptr())
                .to_str()
                .ok()
                .map(|s| s.to_string());
        }
        None
    }
}

/// macOS: requires Screen Recording permission for window titles. Without it
/// (or on failure) returns empty — sessions still work via hook events.
#[cfg(target_os = "macos")]
pub fn scan_trae_projects() -> Vec<String> {
    let titles = cg::on_screen_window_titles();
    if titles.is_empty() {
        log::debug!("[ide] macOS: no window titles (Screen Recording permission may be required)");
    }
    extract_trae_projects(&titles)
}

/// macOS can't precisely activate a specific window of another app without the
/// Accessibility API. Scan on-screen titles to decide whether the project lives
/// in a Qoder or Trae window, then activate the matching app (best-effort).
#[cfg(target_os = "macos")]
pub fn focus_project_window(name: &str) -> bool {
    let titles = cg::on_screen_window_titles();
    let is_qoder = titles.iter().any(|t| {
        t.ends_with(config::QODER_TITLE_SUFFIX)
            && parse_project_name(t)
                .map(|p| p.eq_ignore_ascii_case(name))
                .unwrap_or(false)
    });
    let app = if is_qoder { "Qoder" } else { "Trae CN" };
    match std::process::Command::new("open").arg("-a").arg(app).spawn() {
        Ok(_) => true,
        Err(e) => {
            log::warn!("[ide] macOS focus (open -a {}) failed: {}", app, e);
            false
        }
    }
}

// ============================================================================
// Linux — X11 XQueryTree + _NET_WM_NAME via x11rb (Wayland → empty)
// ============================================================================

#[cfg(target_os = "linux")]
mod x11 {
    use crate::config;
    use crate::ide_scanner::{is_ide_title, parse_project_name};
    use std::collections::HashMap;
    use std::sync::{Mutex, OnceLock};
    use x11rb::protocol::xproto::{self, ConnectionExt as _, EventMask};

    /// name → window-id cache populated by scan(), read by focus_window().
    /// Keyed by the parsed project name so focus(name) can look up directly.
    static WIN_MAP: OnceLock<Mutex<HashMap<String, u32>>> = OnceLock::new();

    fn map() -> &'static Mutex<HashMap<String, u32>> {
        WIN_MAP.get_or_init(|| Mutex::new(HashMap::new()))
    }

    /// Read a window's title via `_NET_WM_NAME` (UTF-8), falling back to
    /// `WM_NAME`. Returns None if the window has no name or on X11 error.
    fn window_title<C: x11rb::connection::Connection>(
        conn: &C,
        win: u32,
        net_wm_name: Option<u32>,
        wm_name: Option<u32>,
    ) -> Option<String> {
        let try_get = |atom: u32| -> Option<String> {
            let r = conn
                .get_property(false, win, atom, xproto::ATOM_ANY, 0, 1024)
                .ok()?
                .reply()
                .ok()?;
            if r.value.is_empty() {
                return None;
            }
            Some(String::from_utf8_lossy(&r.value).to_string())
        };
        if let Some(atom) = net_wm_name {
            if let Some(s) = try_get(atom) {
                return Some(s);
            }
        }
        if let Some(atom) = wm_name {
            if let Some(s) = try_get(atom) {
                return Some(s);
            }
        }
        None
    }

    /// Scan X11 top-level windows, return raw Trae window titles, and refresh
    /// the name→window-id map. Empty on X11 connect failure (Wayland/headless).
    pub fn scan() -> Vec<String> {
        let (conn, screen_num) = match x11rb::connect(None) {
            Ok(c) => c,
            Err(_) => {
                // Wayland or headless — no window enumeration possible.
                let mut m = map().lock().unwrap();
                m.clear();
                return Vec::new();
            }
        };
        let root = conn.setup().roots[screen_num].root;

        let tree = match conn.query_tree(root) {
            Ok(c) => match c.reply() {
                Ok(r) => r,
                Err(_) => return Vec::new(),
            },
            Err(_) => return Vec::new(),
        };

        let net_wm_name = conn
            .intern_atom(false, b"_NET_WM_NAME")
            .ok()
            .and_then(|c| c.reply().ok())
            .map(|r| r.atom);
        let wm_name = conn
            .intern_atom(false, b"WM_NAME")
            .ok()
            .and_then(|c| c.reply().ok())
            .map(|r| r.atom);

        let mut titles: Vec<String> = Vec::new();
        let mut new_map: HashMap<String, u32> = HashMap::new();
        for &win in &tree.children {
            if let Some(title) = window_title(&conn, win, net_wm_name, wm_name) {
                if is_ide_title(&title) {
                    if let Some(proj) = parse_project_name(&title) {
                        new_map.insert(proj.to_string(), win);
                    }
                    titles.push(title);
                }
            }
        }

        if let Ok(mut m) = map().lock() {
            *m = new_map;
        }
        titles
    }

    /// Activate a window by project name via the EWMH _NET_ACTIVE_WINDOW
    /// ClientMessage on the root window. Best-effort: returns false on any X11
    /// error (compositor may ignore the request).
    pub fn focus_window(name: &str) -> bool {
        let win = match map().lock().unwrap().get(name).copied() {
            Some(w) => w,
            None => return false,
        };

        let (conn, screen_num) = match x11rb::connect(None) {
            Ok(c) => c,
            Err(_) => return false,
        };
        let root = conn.setup().roots[screen_num].root;

        let net_active_window = match conn
            .intern_atom(false, b"_NET_ACTIVE_WINDOW")
            .ok()
            .and_then(|c| c.reply().ok())
        {
            Some(r) => r.atom,
            None => return false,
        };

        // _NET_ACTIVE_WINDOW ClientMessage: data.l = [source, timestamp,
        // currently_active_window, 0, 0]. source 1 = application.
        let event = xproto::ClientMessageEvent {
            response_type: xproto::CLIENT_MESSAGE_EVENT,
            format: 32,
            sequence: 0,
            window: win,
            type_: net_active_window,
            data: xproto::ClientMessageData::from([1u32, 0u32, win, 0u32, 0u32]),
        };

        let mask = EventMask::SUBSTRUCTURE_NOTIFY | EventMask::SUBSTRUCTURE_REDIRECT;
        let res = conn.send_event(false, root, mask, &event).and_then(|_| conn.flush());
        if let Err(e) = res {
            log::warn!("[ide] Linux _NET_ACTIVE_WINDOW send failed: {}", e);
            false
        } else {
            true
        }
    }
}

#[cfg(target_os = "linux")]
pub fn scan_trae_projects() -> Vec<String> {
    let titles = x11::scan();
    if titles.is_empty() {
        log::debug!("[ide] Linux: no Trae windows (X11 unavailable or no IDE open)");
    }
    extract_trae_projects(&titles)
}

#[cfg(target_os = "linux")]
pub fn focus_project_window(name: &str) -> bool {
    x11::focus_window(name)
}

// ============================================================================
// Fallback — any other platform: no detection.
// ============================================================================

#[cfg(not(any(windows, target_os = "macos", target_os = "linux")))]
pub fn scan_trae_projects() -> Vec<String> {
    Vec::new()
}

#[cfg(not(any(windows, target_os = "macos", target_os = "linux")))]
pub fn focus_project_window(_name: &str) -> bool {
    false
}
