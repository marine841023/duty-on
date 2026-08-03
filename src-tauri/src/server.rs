//! HTTP server for receiving hook events from IDE bridge scripts.
//!
//! Endpoints:
//!   POST /hook        - Receive a hook event from the bridge script
//!   POST /unregister  - Remove a session (when an IDE closes)
//!   POST /log         - Forward frontend console logs to backend
//!   GET  /status      - Current state snapshot (debugging)
//!   GET  /health      - Health check
//!
//! Connection handling: hyper (axum's HTTP engine) cancels the response
//! future when a client disconnects mid-request and drops the socket, so
//! half-closed sockets don't accumulate as CLOSE_WAIT (the bug that plagued
//! the Node http server). `SO_KEEPALIVE` is set on the listening socket via
//! raw `setsockopt` (axum 0.7's `Serve` does not expose `tcp_keepalive`).

use std::time::Duration;

use crate::config;
use crate::state_manager::{HookEvent, SharedStateManager};
use axum::extract::{Path, State};
use axum::http::{header::CONTENT_TYPE, Method, StatusCode};
use axum::response::{IntoResponse, Json, Response};
use axum::routing::{get, post};
use axum::Router;
use serde_json::json;
use tokio::net::TcpListener;
use tower_http::cors::{AllowOrigin, Any, CorsLayer};

// ── TCP keepalive FFI ──────────────────────────────────────────────────
// axum 0.7's `Serve` lacks a `tcp_keepalive` method (added in 0.8), so we
// set `SO_KEEPALIVE` on the listening socket via raw `setsockopt`. On
// Windows the option propagates to all accepted connections.

#[cfg(windows)]
mod keepalive_ffi {
    use std::os::windows::io::AsRawSocket;
    use std::time::Duration;

    const SOL_SOCKET: i32 = 0xffff;
    const SO_KEEPALIVE: i32 = 0x0008;
    /// `IOC_IN | IOC_VENDOR | 4` (mstcpip.h: `_WSAIOW(IOC_VENDOR, 4)`)
    const SIO_KEEPALIVE_VALS: u32 = 0x98000004;

    #[repr(C)]
    struct TcpKeepalive {
        onoff: u32,
        keepalivetime: u32,
        keepaliveinterval: u32,
    }

    #[link(name = "ws2_32")]
    extern "system" {
        fn setsockopt(s: usize, level: i32, optname: i32, optval: *const u8, optlen: i32) -> i32;
        fn WSAIoctl(
            s: usize,
            code: u32,
            inbuf: *const u8,
            inlen: u32,
            outbuf: *mut u8,
            outlen: u32,
            bytes_returned: *mut u32,
            overlapped: *mut u8,
            completion_routine: *mut u8,
        ) -> i32;
    }

    pub fn set_keepalive(listener: &tokio::net::TcpListener, idle: Duration) {
        let raw = listener.as_raw_socket() as usize;
        let on: i32 = 1;
        let ka = TcpKeepalive {
            onoff: 1,
            keepalivetime: idle.as_millis() as u32,
            keepaliveinterval: 10_000, // 10 s between probes
        };
        let mut bytes_returned: u32 = 0;
        unsafe {
            setsockopt(raw, SOL_SOCKET, SO_KEEPALIVE, &on as *const _ as *const u8, 4);
            WSAIoctl(
                raw,
                SIO_KEEPALIVE_VALS,
                &ka as *const _ as *const u8,
                std::mem::size_of::<TcpKeepalive>() as u32,
                std::ptr::null_mut(),
                0,
                &mut bytes_returned,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            );
        }
    }
}

#[cfg(target_os = "linux")]
mod keepalive_ffi {
    use std::os::unix::io::AsRawFd;
    use std::time::Duration;

    const SOL_SOCKET: i32 = 1;
    const SO_KEEPALIVE: i32 = 9;
    const IPPROTO_TCP: i32 = 6;
    const TCP_KEEPIDLE: i32 = 4;
    const TCP_KEEPINTVL: i32 = 5;
    const TCP_KEEPCNT: i32 = 6;

    extern "C" {
        fn setsockopt(s: i32, level: i32, optname: i32, optval: *const u8, optlen: u32) -> i32;
    }

    pub fn set_keepalive(listener: &tokio::net::TcpListener, idle: Duration) {
        let raw = listener.as_raw_fd();
        let on: i32 = 1;
        let idle_secs = idle.as_secs() as i32;
        let intvl: i32 = 10;
        let cnt: i32 = 3;
        unsafe {
            setsockopt(raw, SOL_SOCKET, SO_KEEPALIVE, &on as *const _ as *const u8, 4);
            setsockopt(raw, IPPROTO_TCP, TCP_KEEPIDLE, &idle_secs as *const _ as *const u8, 4);
            setsockopt(raw, IPPROTO_TCP, TCP_KEEPINTVL, &intvl as *const _ as *const u8, 4);
            setsockopt(raw, IPPROTO_TCP, TCP_KEEPCNT, &cnt as *const _ as *const u8, 4);
        }
    }
}

#[cfg(target_os = "macos")]
mod keepalive_ffi {
    use std::os::unix::io::AsRawFd;
    use std::time::Duration;

    const SOL_SOCKET: i32 = 0xffff;
    const SO_KEEPALIVE: i32 = 0x0008;
    const IPPROTO_TCP: i32 = 6;
    const TCP_KEEPALIVE: i32 = 0x10;

    extern "C" {
        fn setsockopt(s: i32, level: i32, optname: i32, optval: *const u8, optlen: u32) -> i32;
    }

    pub fn set_keepalive(listener: &tokio::net::TcpListener, idle: Duration) {
        let raw = listener.as_raw_fd();
        let on: i32 = 1;
        let idle_secs = idle.as_secs() as i32;
        unsafe {
            setsockopt(raw, SOL_SOCKET, SO_KEEPALIVE, &on as *const _ as *const u8, 4);
            setsockopt(raw, IPPROTO_TCP, TCP_KEEPALIVE, &idle_secs as *const _ as *const u8, 4);
        }
    }
}

#[cfg(not(any(windows, target_os = "linux", target_os = "macos")))]
mod keepalive_ffi {
    use std::time::Duration;
    pub fn set_keepalive(_listener: &tokio::net::TcpListener, _idle: Duration) {}
}

/// Build the axum Router with all routes, CORS, and shared state.
/// Separated from `start` so tests can spin up a server on an ephemeral port.
fn build_router(state: SharedStateManager) -> Router {
    // CORS: restrict to Tauri webview origins only. Bridge scripts use
    // curl/Invoke-RestMethod (non-browser clients ignore CORS), so only the
    // webview's fetch/XHR calls need to pass preflight here. Headers are
    // wide-open because the cubism4/pixi model loaders send library-specific
    // request headers we cannot enumerate.
    let cors = CorsLayer::new()
        .allow_origin(AllowOrigin::list([
            "http://tauri.localhost".parse().unwrap(),
            "tauri://localhost".parse().unwrap(),
            "https://tauri.localhost".parse().unwrap(),
        ]))
        .allow_methods([Method::GET, Method::POST, Method::OPTIONS])
        .allow_headers(Any);

    Router::new()
        .route("/hook", post(hook))
        .route("/unregister", post(unregister))
        .route("/log", post(frontend_log))
        .route("/status", get(status))
        .route("/health", get(health))
        .route("/live2d/*path", get(serve_live2d_file))
        .layer(cors)
        .with_state(state)
}

/// Start the HTTP server on the configured loopback port. Runs until the
/// runtime is shut down (app exit).
pub async fn start(state: SharedStateManager) {
    let app = build_router(state);

    match TcpListener::bind((config::HOST, config::PORT)).await {
        Ok(listener) => {
            log::info!(
                "[PetServer] Listening on http://{}:{}",
                config::HOST,
                config::PORT
            );
            keepalive_ffi::set_keepalive(&listener, Duration::from_secs(60));
            let serve = axum::serve(listener, app).tcp_nodelay(true);
            if let Err(e) = serve.await {
                log::error!("[PetServer] serve error: {}", e);
            }
        }
        Err(e) => {
            if e.kind() == std::io::ErrorKind::AddrInUse {
                log::error!(
                    "[PetServer] Port {} is already in use. Another instance may be running.",
                    config::PORT
                );
            } else {
                log::error!("[PetServer] Bind error: {}", e);
            }
        }
    }
}

/// Build a 200 OK JSON response.
fn ok_json(payload: impl serde::Serialize) -> Response {
    (StatusCode::OK, Json(serde_json::to_value(payload).unwrap_or_default())).into_response()
}

async fn hook(
    State(state): State<SharedStateManager>,
    Json(event): Json<HookEvent>,
) -> Response {
    let project_label = if !event.project_name.is_empty() {
        event.project_name.as_str()
    } else if !event.cwd.is_empty() {
        event.cwd.as_str()
    } else {
        "?"
    };
    log::info!(
        "[PetServer] event: {} | session={} | project={}",
        event.hook_event_name,
        event.session_id,
        project_label
    );
    if event.hook_event_name == "Notification" {
        log::info!(
            "[PetServer] Notification payload: {}",
            serde_json::to_string(&event).unwrap_or_default()
        );
    }
    {
        let mut sm = state.lock().await;
        sm.handle_hook_event(&event);
    }
    ok_json(&json!({ "ok": true }))
}

async fn unregister(
    State(state): State<SharedStateManager>,
    Json(body): Json<serde_json::Value>,
) -> Response {
    if let Some(session_id) = body.get("session_id").and_then(|v| v.as_str()) {
        let mut sm = state.lock().await;
        sm.remove_session(session_id);
    }
    ok_json(&json!({ "ok": true }))
}

async fn status(State(state): State<SharedStateManager>) -> Response {
    let snapshot = {
        let sm = state.lock().await;
        sm.get_snapshot()
    };
    ok_json(&snapshot)
}

async fn health() -> Response {
    ok_json(&json!({ "status": "ok", "port": config::PORT }))
}

/// GET /live2d/*path — serve user-provided Live2D model files from
/// ~/.dutyon/live2d/ through the global CORS layer. The Tauri asset protocol
/// cannot serve these to the webview's XHR-based loaders (cubism4/pixi): its
/// responses carry no CORS headers, so preflight fails with an opaque
/// "Network error" even though plain fetch probes return 200.
async fn serve_live2d_file(Path(rel): Path<String>) -> Response {
    // Reject traversal and empty segments (double slashes); the path is then
    // guaranteed to stay inside the live2d root.
    if rel.is_empty() || rel.split('/').any(|seg| seg.is_empty() || seg == "..") {
        return (StatusCode::BAD_REQUEST, "invalid path").into_response();
    }
    let Some(home) = dirs::home_dir() else {
        return (StatusCode::INTERNAL_SERVER_ERROR, "home dir unavailable").into_response();
    };
    let file_path = home.join(".dutyon").join("live2d").join(&rel);
    match tokio::fs::read(&file_path).await {
        Ok(bytes) => {
            let mime = match file_path
                .extension()
                .and_then(|e| e.to_str())
                .map(|e| e.to_ascii_lowercase())
                .as_deref()
            {
                Some("json") => "application/json",
                Some("png") => "image/png",
                Some("jpg") | Some("jpeg") => "image/jpeg",
                _ => "application/octet-stream",
            };
            (StatusCode::OK, [(CONTENT_TYPE, mime)], bytes).into_response()
        }
        Err(_) => (StatusCode::NOT_FOUND, "not found").into_response(),
    }
}

/// Append one line to `~/.dutyon/frontend.log`. Release builds have no
/// console/stderr, so both the webview's /log endpoint and backend
/// diagnostics (e.g. the IDE window scanner) persist here to stay debuggable
/// on user machines. Truncates the file when it grows past 512 KiB.
pub fn append_log_file(level: &str, msg: &str) {
    if let Some(home) = dirs::home_dir() {
        let path = home.join(".dutyon").join("frontend.log");
        let secs = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_secs())
            .unwrap_or(0);
        let line = format!("[{}][{}] {}\n", secs, level, msg);
        if let Ok(meta) = std::fs::metadata(&path) {
            if meta.len() > 512 * 1024 {
                let _ = std::fs::remove_file(&path);
            }
        }
        let _ = std::fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(&path)
            .and_then(|mut f| std::io::Write::write_all(&mut f, line.as_bytes()));
    }
}

/// Frontend diagnostic log endpoint. The webview POSTs JS errors and load
/// diagnostics here (via fetch, independent of __TAURI__) so we can see them
/// in the terminal — essential when the webview console is inaccessible.
async fn frontend_log(Json(body): Json<serde_json::Value>) -> Response {
    let level = body.get("level").and_then(|v| v.as_str()).unwrap_or("info");
    let msg = body.get("msg").and_then(|v| v.as_str()).unwrap_or("");
    match level {
        "error" => log::error!("[frontend] {}", msg),
        "warn" => log::warn!("[frontend] {}", msg),
        _ => log::info!("[frontend] {}", msg),
    }
    append_log_file(level, msg);
    ok_json(&json!({ "ok": true }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::state_manager::StateManager;
    use std::sync::Arc;
    use tokio::io::{AsyncReadExt, AsyncWriteExt};
    use tokio::net::TcpStream;

    fn make_state() -> SharedStateManager {
        Arc::new(tokio::sync::Mutex::new(StateManager::new()))
    }

    /// Start a test server on an ephemeral port; returns the port number.
    async fn start_test_server() -> u16 {
        let app = build_router(make_state());
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let port = listener.local_addr().unwrap().port();
        tokio::spawn(async move {
            let _ = axum::serve(listener, app).await;
        });
        // Yield once so the spawned server task gets to call accept().
        tokio::time::sleep(std::time::Duration::from_millis(50)).await;
        port
    }

    /// Parse raw HTTP response bytes into (status_code, body_string).
    fn parse_http_response(response: &[u8]) -> (u16, String) {
        let response_str = String::from_utf8_lossy(response);
        let status_line = response_str.lines().next().unwrap_or("");
        let status_code: u16 = status_line
            .split_whitespace()
            .nth(1)
            .and_then(|s| s.parse().ok())
            .unwrap_or(0);
        // Body starts after the first blank line (\r\n\r\n).
        let body = response_str
            .split("\r\n\r\n")
            .nth(1)
            .unwrap_or("")
            .to_string();
        (status_code, body)
    }

    /// Send a raw HTTP GET request and return (status_code, body).
    async fn http_get(port: u16, path: &str) -> (u16, String) {
        let mut stream = TcpStream::connect(format!("127.0.0.1:{}", port))
            .await
            .unwrap();
        let request = format!(
            "GET {} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n",
            path
        );
        stream.write_all(request.as_bytes()).await.unwrap();
        let mut response = Vec::new();
        stream.read_to_end(&mut response).await.unwrap();
        parse_http_response(&response)
    }

    /// Send a raw HTTP POST request with a JSON body and return (status_code, body).
    async fn http_post_json(port: u16, path: &str, body: &str) -> (u16, String) {
        let mut stream = TcpStream::connect(format!("127.0.0.1:{}", port))
            .await
            .unwrap();
        let request = format!(
            "POST {} HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            path,
            body.len(),
            body
        );
        stream.write_all(request.as_bytes()).await.unwrap();
        let mut response = Vec::new();
        stream.read_to_end(&mut response).await.unwrap();
        parse_http_response(&response)
    }

    #[tokio::test]
    async fn health_returns_200() {
        let port = start_test_server().await;
        let (status, body) = http_get(port, "/health").await;
        assert_eq!(status, 200);
        assert!(body.contains("ok"), "health body should contain 'ok': {}", body);
    }

    #[tokio::test]
    async fn status_returns_200_and_json() {
        let port = start_test_server().await;
        let (status, body) = http_get(port, "/status").await;
        assert_eq!(status, 200);
        assert!(
            body.contains("overallState") || body.contains("overall_state"),
            "status body should contain overall state: {}", body
        );
    }

    #[tokio::test]
    async fn hook_accepts_event_and_returns_200() {
        let port = start_test_server().await;
        let json = r#"{"session_id":"test-1","hook_event_name":"SessionStart","project_name":"test-project"}"#;
        let (status, body) = http_post_json(port, "/hook", json).await;
        assert_eq!(status, 200);
        assert!(body.contains("ok"), "hook response should contain 'ok': {}", body);
    }

    #[tokio::test]
    async fn live2d_rejects_path_traversal() {
        let port = start_test_server().await;
        let (status, _) = http_get(port, "/live2d/..%2F..%2Fsecret.txt").await;
        assert_eq!(status, 400);
    }

    #[tokio::test]
    async fn live2d_missing_file_returns_404() {
        let port = start_test_server().await;
        let (status, _) = http_get(port, "/live2d/no-such-model-zz9/model3.json").await;
        assert_eq!(status, 404);
    }

    /// The cubism4/pixi XHR loaders send library-specific headers, so their
    /// CORS preflight must succeed against the /live2d route (this is exactly
    /// what the Tauri asset protocol could not do, causing "Network error").
    #[tokio::test]
    async fn live2d_preflight_allows_custom_headers() {
        let port = start_test_server().await;
        let mut stream = TcpStream::connect(format!("127.0.0.1:{}", port))
            .await
            .unwrap();
        let request = "OPTIONS /live2d/x/model3.json HTTP/1.1\r\nHost: 127.0.0.1\r\nOrigin: http://tauri.localhost\r\nAccess-Control-Request-Method: GET\r\nAccess-Control-Request-Headers: x-requested-with\r\nConnection: close\r\n\r\n";
        stream.write_all(request.as_bytes()).await.unwrap();
        let mut response = Vec::new();
        stream.read_to_end(&mut response).await.unwrap();
        let (status, _) = parse_http_response(&response);
        assert_eq!(status, 200);
    }
}
