//! IDE window scanner for detecting running Trae, Qoder and Cursor IDE
//! instances. Scans window titles across Windows, macOS, and Linux (X11) to
//! identify active project windows and their IDE type.

use crate::config;
use crate::models::IdeKind;

/// A detected IDE window: project folder name + which IDE owns the window.
/// Carries the IDE kind so the pet can badge each project in the status bar.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DetectedProject {
    pub name: String,
    pub ide: IdeKind,
}

/// Windows appends a privilege suffix to the window title of elevated
/// (run-as-administrator) processes. The suffix is locale-dependent
/// ("[Administrator]" on en-US, "[管理员]" on zh-CN, ...), so match the
/// generic ` - Qoder/Trae CN/Cursor [<anything>]` shape instead of
/// enumerating translations. Returns the title with the suffix stripped.
fn strip_privilege_suffix(title: &str) -> &str {
    for suffix in [
        config::QODER_TITLE_SUFFIX,
        config::TRAE_TITLE_SUFFIX,
        config::CURSOR_AGENTS_TITLE_SUFFIX,
        config::CURSOR_TITLE_SUFFIX,
        config::CURSOR_AGENTS_TITLE,
    ] {
        // " - Qoder [管理员]".len() == suffix.len() + 1 + bracket contents
        if let Some(ide_end) = title.rfind(suffix) {
            let rest = &title[ide_end + suffix.len()..];
            let trimmed = rest.trim_start();
            if trimmed.is_empty() {
                return &title[..ide_end + suffix.len()];
            }
            // The remainder must be exactly one bracketed token: "[...]".
            let bytes = trimmed.as_bytes();
            if bytes[0] == b'[' && trimmed.ends_with(']') && !trimmed[1..trimmed.len() - 1].contains('[') {
                return &title[..ide_end + suffix.len()];
            }
        }
    }
    title
}

/// Parse an IDE window title to extract the project name and IDE type.
/// Returns None if the title is not a recognized IDE window title or is a
/// generic title (no project open, e.g. "Trae CN - Trae CN").
///
/// Trae IDE: "<file> - <project> - Trae CN"
/// Qoder:    "<file> - <project> - Qoder"
/// Cursor:   "<file> - <project> - Cursor"
///           "<folder> - Cursor Agents" / "Cursor Agents" (3.x Agents panel —
///           the editor main window may be untitled, so the panel title is
///           the only visible signal; without a workspace it shows up as a
///           project named "Cursor Agents")
/// Elevated windows carry a privilege suffix: "... - Qoder [管理员]".
fn parse_title(title: &str) -> Option<(&str, IdeKind)> {
    let title = strip_privilege_suffix(title);
    // Check the Agents-panel suffix before " - Cursor": " - Cursor Agents"
    // doesn't end with " - Cursor", but the standalone "Cursor Agents"
    // title matches neither suffix — handle it as an exact title.
    if title == config::CURSOR_AGENTS_TITLE {
        return Some((config::CURSOR_AGENTS_TITLE, IdeKind::Cursor));
    }
    let ide = if title.ends_with(config::TRAE_TITLE_SUFFIX) {
        IdeKind::Trae
    } else if title.ends_with(config::QODER_TITLE_SUFFIX) {
        IdeKind::Qoder
    } else if title.ends_with(config::CURSOR_AGENTS_TITLE_SUFFIX) {
        IdeKind::Cursor
    } else if title.ends_with(config::CURSOR_TITLE_SUFFIX) {
        IdeKind::Cursor
    } else {
        return None;
    };
    let parts: Vec<&str> = title.split(" - ").collect();
    if parts.len() < 2 {
        return None;
    }
    let folder = parts[parts.len() - 2].trim();
    if folder.is_empty()
        || folder == "Trae"
        || folder == "Trae CN"
        || folder == "Qoder"
        || folder == "Cursor"
    {
        return None;
    }
    Some((folder, ide))
}

/// From a list of all on-screen window titles, extract the IDE projects
/// (deduped by project name — the first window seen wins its IDE kind).
/// Shared across platforms.
fn extract_ide_projects(titles: &[String]) -> Vec<DetectedProject> {
    let mut out: Vec<DetectedProject> = Vec::new();
    for t in titles {
        if let Some((name, ide)) = parse_title(t) {
            if !out.iter().any(|p| p.name == name) {
                out.push(DetectedProject {
                    name: name.to_string(),
                    ide,
                });
            }
        }
    }
    out.sort_by(|a, b| a.name.cmp(&b.name));
    out
}

/// Titles that mention an IDE but didn't parse into a project. Logged (only
/// when the detected set changes) so unfamiliar title formats — new IDE
/// versions, locales, chat-panel titles — show up in frontend.log and can be
/// added to the parser. Truncated to keep the log small.
fn suspect_ide_titles(titles: &[String]) -> Vec<String> {
    titles
        .iter()
        .filter(|t| {
            let low = t.to_lowercase();
            low.contains("qoder") || low.contains("trae") || low.contains("cursor")
        })
        .take(8)
        .map(|t| t.chars().take(200).collect::<String>())
        .collect()
}

// ============================================================================
// Windows — EnumWindows
// ============================================================================

#[cfg(windows)]
use windows::core::PWSTR;
#[cfg(windows)]
use windows::Win32::Foundation::{CloseHandle, BOOL, HWND, LPARAM, WPARAM};
#[cfg(windows)]
use windows::Win32::System::Threading::{
    OpenProcess, QueryFullProcessImageNameW, PROCESS_NAME_WIN32,
    PROCESS_QUERY_LIMITED_INFORMATION,
};
#[cfg(windows)]
use windows::Win32::UI::WindowsAndMessaging::{
    EnumWindows, GetWindowThreadProcessId, IsIconic, IsWindowVisible, SendMessageTimeoutW,
    SetForegroundWindow, ShowWindowAsync, SMTO_ABORTIFHUNG, SW_RESTORE, WM_GETTEXT,
    WM_GETTEXTLENGTH,
};

/// Executable image names of the IDEs we track (case-insensitive, matched
/// against the last path segment of the owning process image).
#[cfg(windows)]
const IDE_PROCESS_EXES: &[&str] = &["Qoder.exe", "Trae CN.exe", "Trae.exe", "Cursor.exe"];

/// Hard cap (ms) for every title request sent to an IDE window.
#[cfg(windows)]
const TITLE_QUERY_TIMEOUT_MS: u32 = 250;

/// Whether the window's owning process is a known IDE. `Some(true)` /
/// `Some(false)` when the process image could be queried; `None` when the
/// query itself failed — callers then fall back to a timeout-guarded title
/// read so an unqueryable process can't make its window invisible.
///
/// This check is pure process querying (no window messages), so it can
/// never hang no matter what window it runs against.
#[cfg(windows)]
fn window_owned_by_ide(hwnd: HWND) -> Option<bool> {
    let mut pid = 0u32;
    unsafe {
        if GetWindowThreadProcessId(hwnd, Some(&mut pid)) == 0 || pid == 0 {
            return None;
        }
        let process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid).ok()?;
        let mut buf = [0u16; 512];
        let mut len = buf.len() as u32;
        let ok = QueryFullProcessImageNameW(
            process,
            PROCESS_NAME_WIN32,
            PWSTR(buf.as_mut_ptr()),
            &mut len,
        )
        .is_ok();
        let _ = CloseHandle(process);
        if !ok {
            return None;
        }
        let path = String::from_utf16_lossy(&buf[..len as usize]);
        let exe = path.rsplit(['\\', '/']).next().unwrap_or(&path);
        Some(
            IDE_PROCESS_EXES
                .iter()
                .any(|name| name.eq_ignore_ascii_case(exe)),
        )
    }
}

/// Read a window title via WM_GETTEXTLENGTH/WM_GETTEXT with a hard timeout.
///
/// The classic `GetWindowTextW` sends a synchronous `WM_GETTEXT` to the
/// window's owner thread and blocks indefinitely while that thread is busy
/// — a console window running a foreground CMD command is the textbook
/// case. Because the scanner holds the state-manager lock once per cycle,
/// one such hang stalls the whole hook→state→frontend pipeline (events
/// visibly arrive in bridge.log but the pet only updates when the blocked
/// window finally responds/closes). The timeout turns any unresponsive
/// window into a cheap skip; `None` = no usable title.
#[cfg(windows)]
unsafe fn read_title_timeout(hwnd: HWND) -> Option<String> {
    let mut len: usize = 0;
    let r = SendMessageTimeoutW(
        hwnd,
        WM_GETTEXTLENGTH,
        WPARAM(0),
        LPARAM(0),
        SMTO_ABORTIFHUNG,
        TITLE_QUERY_TIMEOUT_MS,
        Some(&mut len),
    );
    // 0 covers both "timed out" and "empty title" — either way: skip.
    if r.0 <= 0 || len == 0 {
        return None;
    }
    let mut buf = vec![0u16; len + 1];
    let mut copied: usize = 0;
    let r = SendMessageTimeoutW(
        hwnd,
        WM_GETTEXT,
        WPARAM(len + 1),
        LPARAM(buf.as_mut_ptr() as isize),
        SMTO_ABORTIFHUNG,
        TITLE_QUERY_TIMEOUT_MS,
        Some(&mut copied),
    );
    if r.0 <= 0 || copied == 0 {
        return None;
    }
    Some(String::from_utf16_lossy(&buf[..copied]))
}

#[cfg(windows)]
struct FocusCtx {
    name: String,
    best: Option<HWND>,
    substring: Option<HWND>,
}

/// Focus the Trae IDE window whose project segment matches `name`.
#[cfg(windows)]
pub fn focus_project_window(name: &str) -> bool {
    // The context is passed to the EnumWindows callback as a raw pointer
    // (Win32 LPARAM has no generics); the borrow lives for the duration of
    // the EnumWindows call, which is synchronous, so this is sound.
    let mut ctx = FocusCtx {
        name: name.to_string(),
        best: None,
        substring: None,
    };
    let lparam = LPARAM(&mut ctx as *mut FocusCtx as isize);
    unsafe {
        let _ = EnumWindows(Some(focus_proc), lparam);
    }

    let target = ctx.best.or(ctx.substring);
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
    let ctx = &mut *(lparam.0 as *mut FocusCtx);
    if !IsWindowVisible(hwnd).as_bool() {
        return BOOL(1);
    }
    let Some(title) = read_title_timeout(hwnd) else {
        return BOOL(1);
    };
    // parse_title filters non-IDE windows and generic titles; the folder
    // segment drives the exact match, the full title a substring fallback.
    let folder = match parse_title(&title) {
        Some((folder, _ide)) => folder,
        None => return BOOL(1),
    };
    if folder.eq_ignore_ascii_case(&ctx.name) {
        ctx.best = Some(hwnd);
    } else if title.to_lowercase().contains(&ctx.name.to_lowercase())
        && ctx.best.is_none()
        && ctx.substring.is_none()
    {
        ctx.substring = Some(hwnd);
    }
    BOOL(1)
}

/// Scan all visible top-level windows and return the projects of every IDE
/// window (deduped, generic titles filtered out), each tagged with its IDE,
/// plus raw IDE-looking titles for diagnostics.
#[cfg(windows)]
pub fn scan_ide_projects() -> (Vec<DetectedProject>, Vec<String>) {
    let mut titles: Vec<String> = Vec::new();
    let lparam = LPARAM(&mut titles as *mut Vec<String> as isize);
    unsafe {
        let _ = EnumWindows(Some(enum_proc), lparam);
    }
    let suspects = suspect_ide_titles(&titles);
    (extract_ide_projects(&titles), suspects)
}

#[cfg(windows)]
unsafe extern "system" fn enum_proc(hwnd: HWND, lparam: LPARAM) -> BOOL {
    let titles = &mut *(lparam.0 as *mut Vec<String>);
    if IsWindowVisible(hwnd).as_bool() {
        // Request titles only from IDE-owned windows (decided by process
        // name — no window messages are sent to anything else). Windows
        // whose owner can't be queried (None) fall back to a timeout-
        // guarded read. This guarantees a busy/hung non-IDE window — e.g.
        // a CMD console running a long command — can never stall the
        // enumeration, the scan loop, or the state pipeline behind it.
        if window_owned_by_ide(hwnd) != Some(false) {
            if let Some(title) = read_title_timeout(hwnd) {
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
pub fn scan_ide_projects() -> (Vec<DetectedProject>, Vec<String>) {
    let titles = cg::on_screen_window_titles();
    if titles.is_empty() {
        log::debug!("[ide] macOS: no window titles (Screen Recording permission may be required)");
    }
    let suspects = suspect_ide_titles(&titles);
    (extract_ide_projects(&titles), suspects)
}

/// macOS can't precisely activate a specific window of another app without the
/// Accessibility API. Scan on-screen titles to decide which IDE owns the
/// project, then activate the matching app (best-effort).
#[cfg(target_os = "macos")]
pub fn focus_project_window(name: &str) -> bool {
    let titles = cg::on_screen_window_titles();
    let owner = titles.iter().find_map(|t| {
        match parse_title(t) {
            Some((p, ide)) if p.eq_ignore_ascii_case(name) => Some(ide),
            _ => None,
        }
    });
    let app = match owner {
        Some(IdeKind::Qoder) => "Qoder",
        Some(IdeKind::Cursor) => "Cursor",
        _ => "Trae CN",
    };
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
    use crate::ide_scanner::parse_title;
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

    /// Scan X11 top-level windows, return ALL non-empty window titles, and
    /// refresh the name→window-id map. Empty on X11 connect failure
    /// (Wayland/headless).
    pub fn scan() -> Vec<String> {
        let (conn, screen_num) = match x11rb::connect(None) {
            Ok(c) => c,
            Err(_) => {
                // Wayland or headless — no window enumeration possible.
                // SAFETY: `unwrap()` on the Mutex is safe — the crate is built
                // with `panic = "abort"` (Cargo.toml [profile.release]), so a
                // panic never unwinds and thus never poisons the lock.
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
                if let Some((proj, _ide)) = parse_title(&title) {
                    new_map.insert(proj.to_string(), win);
                }
                titles.push(title);
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
        // SAFETY: `unwrap()` on the Mutex is safe — the crate is built with
        // `panic = "abort"` (Cargo.toml [profile.release]), so a panic never
        // unwinds and thus never poisons the lock.
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
pub fn scan_ide_projects() -> (Vec<DetectedProject>, Vec<String>) {
    let titles = x11::scan();
    if titles.is_empty() {
        log::debug!("[ide] Linux: no IDE windows (X11 unavailable or no IDE open)");
    }
    let suspects = suspect_ide_titles(&titles);
    (extract_ide_projects(&titles), suspects)
}

#[cfg(target_os = "linux")]
pub fn focus_project_window(name: &str) -> bool {
    x11::focus_window(name)
}

// ============================================================================
// Fallback — any other platform: no detection.
// ============================================================================

#[cfg(not(any(windows, target_os = "macos", target_os = "linux")))]
pub fn scan_ide_projects() -> (Vec<DetectedProject>, Vec<String>) {
    (Vec::new(), Vec::new())
}

#[cfg(not(any(windows, target_os = "macos", target_os = "linux")))]
pub fn focus_project_window(_name: &str) -> bool {
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_title_standard_trae() {
        // Trae IDE window title: "<file> - <project> - Trae CN"
        let result = parse_title("main.rs - MyProject - Trae CN");
        assert_eq!(result, Some(("MyProject", IdeKind::Trae)));
    }

    #[test]
    fn parse_title_standard_qoder() {
        // Qoder window title: "<file> - <project> - Qoder"
        let result = parse_title("app.ts - MyProject - Qoder");
        assert_eq!(result, Some(("MyProject", IdeKind::Qoder)));
    }

    #[test]
    fn parse_title_standard_cursor() {
        // Cursor window title: "<file> - <project> - Cursor"
        let result = parse_title("main.py - MyProject - Cursor");
        assert_eq!(result, Some(("MyProject", IdeKind::Cursor)));
    }

    #[test]
    fn parse_title_cursor_generic_no_project() {
        // Cursor with no project open: "Cursor - Cursor" → folder segment is
        // "Cursor" → filtered out (same shape as "Trae CN - Trae CN")
        let result = parse_title("Cursor - Cursor");
        assert_eq!(result, None);
    }

    #[test]
    fn parse_title_cursor_agents_panel_standalone() {
        // Cursor 3.x standalone Agents panel (editor main window untitled).
        let result = parse_title("Cursor Agents");
        assert_eq!(result, Some(("Cursor Agents", IdeKind::Cursor)));
    }

    #[test]
    fn parse_title_cursor_agents_panel_with_workspace() {
        let result = parse_title("MyProject - Cursor Agents");
        assert_eq!(result, Some(("MyProject", IdeKind::Cursor)));
    }

    #[test]
    fn parse_title_cursor_agents_panel_elevated() {
        // Privilege suffix on the panel title is stripped like any IDE title.
        let result = parse_title("Cursor Agents [管理员]");
        assert_eq!(result, Some(("Cursor Agents", IdeKind::Cursor)));
    }

    #[test]
    fn parse_title_non_ide_title() {
        let result = parse_title("Visual Studio Code");
        assert_eq!(result, None);
    }

    #[test]
    fn parse_title_empty() {
        let result = parse_title("");
        assert_eq!(result, None);
    }

    #[test]
    fn parse_title_elevated_qoder_admin_suffix() {
        // Elevated processes get "[Administrator]" appended on en-US Windows.
        let result = parse_title("settings.json - Qoder [Administrator]");
        assert_eq!(result, Some(("settings.json", IdeKind::Qoder)));
    }

    #[test]
    fn parse_title_elevated_qoder_chinese_suffix() {
        // zh-CN Windows appends "[管理员]" instead.
        let result = parse_title("settings.json - Qoder [管理员]");
        assert_eq!(result, Some(("settings.json", IdeKind::Qoder)));
    }

    #[test]
    fn parse_title_elevated_trae_suffix() {
        let result = parse_title("main.rs - MyProject - Trae CN [管理员]");
        assert_eq!(result, Some(("MyProject", IdeKind::Trae)));
    }

    #[test]
    fn parse_title_suffix_not_bracketed_ignored() {
        // Anything after the IDE name that isn't a single bracketed token is
        // not a privilege suffix — leave the title unparsed.
        let result = parse_title("app.ts - MyProject - Qoder preview");
        assert_eq!(result, None);
    }

    #[test]
    fn parse_title_generic_no_project() {
        // "Trae CN - Trae CN" → folder segment is "Trae CN" → filtered out
        let result = parse_title("Trae CN - Trae CN");
        assert_eq!(result, None);
        // Same for Qoder without a project open
        let result2 = parse_title("Qoder - Qoder");
        assert_eq!(result2, None);
    }

    #[test]
    fn extract_ide_projects_dedupes_same_name() {
        // Two windows with the same project name (one Trae, one Qoder)
        // should produce a single entry — the first one wins.
        let titles = vec![
            "main.rs - MyProject - Trae CN".to_string(),
            "app.ts - MyProject - Qoder".to_string(),
            "index.js - OtherProject - Trae CN".to_string(),
        ];
        let projects = extract_ide_projects(&titles);
        // MyProject deduped to one entry; OtherProject is separate.
        assert_eq!(projects.len(), 2);
        // Sorted alphabetically.
        assert_eq!(projects[0].name, "MyProject");
        assert_eq!(projects[0].ide, IdeKind::Trae); // first window wins
        assert_eq!(projects[1].name, "OtherProject");
    }
}
