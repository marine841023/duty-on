# Technical Notes

## Why not the Tauri asset protocol for user models

User-supplied Live2D models were initially served through the Tauri asset
protocol (`http://asset.localhost/...`), but the cubism4/pixi XHR loaders
send custom request headers, which triggers a CORS preflight; the asset
handler responds without CORS headers, the preflight fails and the loader
reports an opaque "Network error" — while a plain `fetch` probe against the
same URL (a simple request, no preflight) returns 200. Telltale sign:
**fetch works, XHR fails**.

Fix: the local axum server exposes a `GET /live2d/*path` route (rejecting
`..` traversal and empty path segments) and uses `tower_http::cors` to allow
the webview origin with `allow_headers(Any)`. See
`src-tauri/src/server.rs`; preflight/traversal/404 cases are covered by
tests.

## Screenshot checklist (for README)

| File | Content | How |
|------|---------|-----|
| `normal-mode.png` | Full mode: pet + status bar with 2–3 IDE projects | Normal placement, bottom-right, whole window |
| `mini-mode.png` | Mini mode tucked into a corner | Switch to mini via menu, then capture |
| `menu.png` | Context menu with the "切换形象" submenu open | Capture while the menu is open |
| `state-sleeping.png` / `state-working.png` / `state-alert.png` | Close-ups of the three states | Drive states with `.userdata/test-flow.ps1` |
| `demo.gif` | State transitions: IDE event → pet wakes, works, then alerts | Record ~10 s with any GIF recorder |

> Keep an IDE window in the background for context; make sure no private
> code is visible.
