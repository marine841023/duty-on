<div align="center">

<img src="docs/assets/hero.png" alt="Duty On" width="720"/>

# Duty On · 开工啦

**Your favorite character watches your AI IDE, so you don't have to.**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-blue)]()
[![Built with Tauri](https://img.shields.io/badge/Tauri-2.0-24C8D8?logo=tauri&logoColor=white)](https://v2.tauri.app)
[![Release](https://img.shields.io/github/v/release/marine841023/duty-on)](https://github.com/marine841023/duty-on/releases)

### 🎉 v1.3.3 — Smarter hook state machine!

> **Made for Chinese Trae users** — native Trae CN / TraeCode CN window-title
> detection, multi-root workspace suffix stripping (工作区 / Workspace /
> ワークスペース / 작업 영역), 8 languages with Simplified Chinese first.
>
> **New in v1.3.3:** the pet no longer shows "idle" while the model is still
> generating a long reply — the LLM thinking phase is completely silent (no
> hook events) and was measured at 3m52s in the wild, so its timeout is now
> 10 minutes instead of the 3-minute working timeout.
>
> **v1.3.2:** the pet no longer dozes off while sub-agents or long
> tool runs (builds/tests) are still executing; document-review /
> ask-user / browser-interaction notifications now trigger the alert; a late
> async permission notification can no longer stick the alert forever.
>
> **v1.3.1:** a **system monitor panel** — live CPU / RAM / GPU /
> network / self usage, styled like the task pane (status border colors stay in
> sync). Show/hide the whole panel, collapse it, toggle each metric
> individually. **Hidden by default** — enable it from the context menu.
>
> Since v1.3.0 you can also create your own desktop pet from any
> **GIF / PNG / MP4** — no Live2D model required.
>
> Download v1.3.3 (.zip): [GitHub →](https://github.com/marine841023/duty-on/releases/download/v1.3.3/DutyOn-v1.3.3.zip) · [Gitee →](https://gitee.com/megrezsoft/dutyo/releases/download/v1.3.3/DutyOn-v1.3.3.zip)

**English** · [简体中文](README.zh-CN.md)

</div>

---

Running AI agents in several IDE windows at once? Stop Alt-Tabbing to check
whether they're still working, done, or waiting for your confirmation.
**Duty On** is a tiny transparent Live2D character that floats above your
desktop and shows the live status of every **Trae** / Qoder / Cursor / Codex / OpenCode
session at a glance. Built with **Tauri 2 + Rust** (system WebView; ~70 MB
main process plus the WebView2 runtime's browser processes) — **made for
Chinese Trae users**, with native Trae CN / TraeCode CN
title detection and Simplified Chinese as a first-class language:

- 💤 **Sleeping** — everything is idle (she naps, Zzz…)
- ⚡ **Working** — an AI task is running right now
- 🔔 **Alert** — an agent needs your confirmation **right now**

Built with **Tauri 2 + Rust** (system WebView, no bundled Chromium):
24 fps capped rendering, native click-through.

> **Memory correction:** earlier copy claimed "~80 MB RAM" — that only
> counted the main process and missed the WebView2 runtime's browser-process
> group. Measured totals for 1.x: ~465 MB working set / ~195 MB private
> memory. Want truly lightweight? The **[v2.0 native C++ rewrite](../tree/v2.0-dev)**
> runs as a **single process with ~116 MB measured**, no WebView at all.

## Screenshots

> 📸 Coming soon — see the [screenshot checklist](docs/technical-notes.md)
> (normal mode · mini mode · context menu · the three states · status-bar jump).

## Features

- **🎬 Custom GIF characters** — create your own desktop pet from any **GIF /
  PNG / JPG / WebP / MP4 / WebM** file! Upload separate animations for each
  state (💤 sleeping / ⚡ working / 🔔 alert). Re-upload to replace anytime.
  No Live2D model needed — just pick a GIF and your pet comes alive.
  Large images auto-resized (max 1024px); cache-busting ensures re-uploads
  always show the new animation.
- **Live status at a glance** — one character reflects the aggregate state of
  all connected IDE sessions (alert > working > sleeping)
- **Per-project status bar** — every IDE project listed under the pet with a
  T/Q/C/X/O badge; click a project to focus its IDE window
- **Multi-IDE** — monitors any number of **Trae** / Qoder / Cursor / Codex / OpenCode instances
  concurrently (5 IDEs, each with native hook integration)
- **True click-through** — the window is transparent to the mouse except over
  the character and menus (30 ms cursor polling, Win32/CoreGraphics/X11)
- **Mini mode** — shrinks to a 130×210 corner buddy; toggle from the menu
- **21 built-in motions** — tap the pet, trigger motions from the menu, or let
  the state machine drive idle/work/alert animations
- **Custom Live2D models** — drop any Cubism 4 model into
  `~/.dutyon/live2d/` and it appears in the menu (no rebuild, no restart of
  your IDE)
- **8 languages** — auto-follows the OS locale (简中/繁中/EN/JA/KO/FR/DE/ES)
- **Autostart** — one-toggle login launch per platform

## Install

### Download (recommended)

Grab the **`.zip`** from
GitHub [**Releases**](https://github.com/marine841023/duty-on/releases) or Gitee [**Releases**](https://gitee.com/megrezsoft/dutyo/releases),
extract it, and run the NSIS installer inside (`DutyOn_<ver>_x64-setup.exe`).
Distributed as ZIP to bypass Windows SmartScreen on unsigned exe.
(macOS DMG and Linux AppImage/DEB buildable from source.)

### Build from source

Requirements: [Rust](https://rustup.rs/) (stable), Tauri CLI
(`cargo install tauri-cli --version "^2"`), and the
[platform prerequisites](https://v2.tauri.app/start/prerequisites/).
The frontend is plain static files — no npm, no bundler.

```bash
git clone https://github.com/marine841023/duty-on.git
cd duty-on/src-tauri
cargo tauri build   # bundles into target/release/bundle/
```

### Enable IDE hooks

Right-click the pet → **安装 Hook 集成** (or run `hooks/install-hooks.ps1`
on Windows), then restart your IDE or start a new AI session.

## How it works

```
┌─────────────────────────────────────────────┐
│       DutyOn (Tauri 2 desktop app)          │
│  ┌───────────────────────────────────────┐  │
│  │  Frontend: Live2D pet (pixi-live2d)   │  │
│  │  States: 💤 sleep / ⚡ work / 🔔 alert │  │
│  ├───────────────────────────────────────┤  │
│  │  Rust backend:                        │  │
│  │  · State machine (multi-session)      │  │
│  │  · HTTP server (127.0.0.1:17521)      │  │
│  │  · IDE window scanner                 │  │
│  │  · Click-through polling (30 ms)      │  │
│  └───────────────────────────────────────┘  │
└──────────────────┬──────────────────────────┘
                   │ HTTP POST /hook (localhost)
   ┌───────┬───────┼───────┬───────┬───────┐
┌──┴──┐ ┌──┴──┐ ┌──┴──┐ ┌──┴──┐ ┌──┴────┐
│Trae │ │Qoder│ │Cursor│ │Codex│ │OpenCode│
└─────┘ └─────┘ └─────┘ └─────┘ └───────┘
```

| Hook event | When | Pet state |
|---|---|---|
| `SessionStart` | IDE session created | project online (idle) |
| `UserPromptSubmit` | user sends a message | → working |
| `PreToolUse` / `PostToolUse` | AI tool runs / finishes | → working |
| `Notification` | confirmation needed | → alert |
| `Stop` | AI task completed | → idle |
| `PreToolUse`(AskUserQuestion) *(Qoder)* | Qoder asks the user | → alert |
| `PermissionRequest` *(Qoder)* | Qoder permission prompt | → alert |
| `PermissionRequest` *(Codex)* | Codex CLI permission prompt | → alert |
| `permission.ask` *(OpenCode)* | OpenCode permission prompt | → alert |

Ambiguous `Notification` events default to "task complete" (no alert); the
whitelist lives in [`src-tauri/src/config.rs`](src-tauri/src/config.rs).

## Custom GIF characters (v1.3.0+)

Don't have a Live2D model? No problem! Create a custom pet from any **GIF / PNG /
MP4** file:

1. Right-click the pet → **切换形象** → **+ 新建形象**
2. Enter a name, then upload an animation for each state:
   - 💤 **Sleeping** (idle) — shown when no AI task is running
   - ⚡ **Working** — shown when an AI task is active
   - 🔔 **Alert** — shown when confirmation is needed
3. Click **✎** on any custom character to re-upload animations

Supported formats: GIF (animated), PNG/JPG/WebP (static), MP4/WebM/MOV (video).
Large images are auto-resized to max 1024px to keep things lightweight.
Files are stored in `~/.dutyon/animations/<character_id>/`.

## Custom Live2D models

Drop a Cubism 4 model folder into `~/.dutyon/live2d/<name>/`
(`<name>.moc3` + textures + `model3.json` + motions) — it shows up in the
pet's **切换形象** menu immediately. User models are served through the local
loopback server with proper CORS headers (the Tauri asset protocol can't be
used for XHR-based loaders — see [this note](docs/technical-notes.md)).

## Development

```bash
cd src-tauri
cargo tauri dev    # run with DevTools
cargo test         # 87 unit tests (state machine, hooks merge, server, …)
```

End-to-end regression scripts (pet must be running):
`.userdata/test-flow.ps1`, `.userdata/test-notification.ps1`.

## Tech stack

Tauri 2 · Rust (tokio, axum, serde) · PixiJS v7 · pixi-live2d-display ·
Live2D Cubism Core · Trae / Qoder / Cursor / Codex / OpenCode hooks

## Roadmap

- [ ] CI-built releases for macOS / Linux
- [ ] Per-project alert sounds
- [ ] More IDE integrations (the hook protocol is a plain HTTP POST — PRs welcome)
- [ ] Community model gallery

## Contributing

Issues and PRs are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).

## License & credits

Code: [MIT](LICENSE). Bundled Live2D runtime and sample models are © Live2D
Inc. and used under their respective licenses — see [NOTICE](NOTICE).
