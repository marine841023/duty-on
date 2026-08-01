//! HTTP Server — axum port of `src/main/server.js`.
//!
//! Endpoints:
//!   POST /hook        - Receive a hook event from the bridge script
//!   POST /unregister  - Remove a session (when an IDE closes)
//!   GET  /status      - Current state snapshot (debugging)
//!   GET  /health      - Health check
//!
//! Connection handling: hyper (axum's HTTP engine) cancels the response
//! future when a client disconnects mid-request and drops the socket, so
//! half-closed sockets don't accumulate as CLOSE_WAIT (the bug that plagued
//! the Node http server). tcp_keepalive below adds idle-connection reaping.

use crate::config;
use crate::state_manager::{HookEvent, SharedStateManager};
use axum::extract::State;
use axum::http::{Method, StatusCode};
use axum::response::{IntoResponse, Json, Response};
use axum::routing::{get, post};
use axum::Router;
use serde_json::json;
use tokio::net::TcpListener;
use tower_http::cors::{AllowOrigin, CorsLayer};

/// Start the HTTP server on the configured loopback port. Runs until the
/// runtime is shut down (app exit).
pub async fn start(state: SharedStateManager) {
    // CORS: allow any origin — the server is loopback-only (127.0.0.1) so this
    // is safe, and it lets the Tauri webview (origin tauri.localhost) POST
    // diagnostic logs to /log without preflight issues.
    let cors = CorsLayer::new()
        .allow_origin(AllowOrigin::any())
        .allow_methods([Method::GET, Method::POST, Method::OPTIONS])
        .allow_headers([axum::http::header::CONTENT_TYPE]);

    let app = Router::new()
        .route("/hook", post(hook))
        .route("/unregister", post(unregister))
        .route("/log", post(frontend_log))
        .route("/status", get(status))
        .route("/health", get(health))
        .layer(cors)
        .with_state(state);

    match TcpListener::bind((config::HOST, config::PORT)).await {
        Ok(listener) => {
            log::info!(
                "[PetServer] Listening on http://{}:{}",
                config::HOST,
                config::PORT
            );
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
    (StatusCode::OK, Json(json!({ "ok": true }))).into_response()
}

async fn unregister(
    State(state): State<SharedStateManager>,
    Json(body): Json<serde_json::Value>,
) -> Response {
    if let Some(session_id) = body.get("session_id").and_then(|v| v.as_str()) {
        let mut sm = state.lock().await;
        sm.remove_session(session_id);
    }
    (StatusCode::OK, Json(json!({ "ok": true }))).into_response()
}

async fn status(State(state): State<SharedStateManager>) -> Response {
    let snapshot = {
        let sm = state.lock().await;
        sm.get_snapshot()
    };
    (StatusCode::OK, Json(snapshot)).into_response()
}

async fn health() -> Response {
    (
        StatusCode::OK,
        Json(json!({ "status": "ok", "port": config::PORT })),
    )
        .into_response()
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
    (StatusCode::OK, Json(json!({ "ok": true }))).into_response()
}
