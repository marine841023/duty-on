<div align="center">

<img src="docs/assets/hero.png" alt="Duty On" width="720"/>

# Duty On · 开工啦

**Your favorite character watches your AI IDE, so you don't have to.**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-blue)]()
[![Built with Tauri](https://img.shields.io/badge/Tauri-2.0-24C8D8?logo=tauri&logoColor=white)](https://v2.tauri.app)
[![Release](https://img.shields.io/github/v/release/marine841023/duty-on)](https://github.com/marine841023/duty-on/releases)

### 🎉 v1.1.6 — External display + refined state machine!

> **New:** External display screen — show the pet's live status on a separate
> device (Raspberry Pi, tablet, phone) with real-time SSE + per-state sounds.
> State machine now distinguishes Thinking / Tool-Use / Complete phases.
> Codex CLI and OpenCode are now supported alongside Trae / Qoder / Cursor.
> [Download v1.1.6 →](https://github.com/marine841023/duty-on/releases/tag/v1.1.6)

**English** · [简体中文](README.zh-CN.md)

</div>

---

Running AI agents in several IDE windows at once? Stop Alt-Tabbing to check
whether they're still working, done, or waiting for your confirmation.
**Duty On** is a tiny transparent Live2D character that floats above your
desktop and shows the live status of every Trae / Qoder / Cursor / Codex / OpenCode
session at a glance:

- 💤 **Sleeping** — everything is idle (she naps, Zzz…)
- ⚡ **Working** — an AI task is running right now
- 🔔 **Alert** — an agent needs your confirmation **right now**

Built with **Tauri 2 + Rust** (system WebView, no bundled Chromium):
~70–90 MB RAM, 24 fps capped rendering, native click-through.

## Screenshots

> 📸 Coming soon — see the [screenshot checklist](docs/technical-notes.md)
> (normal mode · mini mode · context menu · the three states · status-bar jump).

## Features

- **Live status at a glance** — one character reflects the aggregate state of
  all connected IDE sessions (alert > working > sleeping)
- **Per-project status bar** — every IDE project listed under the pet with a
  T/Q/C/X/O badge; click a project to focus its IDE window
- **Multi-IDE** — monitors any number of Trae / Qoder / Cursor / Codex / OpenCode instances
  concurrently
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

Grab the installer from
[**Releases**](https://github.com/marine841023/duty-on/releases)
(Windows NSIS; macOS DMG and Linux AppImage/DEB buildable from source).

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
        ┌──────────┼──────────┐
     ┌──┴───┐   ┌──┴───┐   ┌──┴───┐
     │ Trae │   │Qoder │   │Cursor│
     │ IDE 1│   │ IDE 2│   │ IDE 3│
     └──────┘   └──────┘   └──────┘
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

Ambiguous `Notification` events default to "task complete" (no alert); the
whitelist lives in [`src-tauri/src/config.rs`](src-tauri/src/config.rs).

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
cargo test         # 52 unit tests (state machine, hooks merge, server, …)
```

End-to-end regression scripts (pet must be running):
`.userdata/test-flow.ps1`, `.userdata/test-notification.ps1`.

## Tech stack

Tauri 2 · Rust (tokio, axum, serde) · PixiJS v7 · pixi-live2d-display ·
Live2D Cubism Core · Trae/Qoder/Cursor IDE hooks

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
