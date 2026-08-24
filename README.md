<div align="center">

<img src="docs/assets/hero.png" alt="Duty On" width="720"/>

# Duty On · 开工啦

**Your favorite character watches your AI IDE, so you don't have to.**

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20ARM%20Linux%20(device)-blue)]()
[![Built with C++](https://img.shields.io/badge/C%2B%2B-native%20%7C%20no%20WebView-00599C?logo=cplusplus&logoColor=white)]()
[![Release](https://img.shields.io/github/v/release/marine841023/duty-on)](https://github.com/marine841023/duty-on/releases)

### 🚀 v2.0.2 — Native C++ rewrite!

> **Made for Chinese Trae users** — native Trae CN / TraeCode CN window-title
> detection, multi-root workspace suffix stripping (工作区 / Workspace /
> ワークスペース / 작업 영역), 8 languages with Simplified Chinese first.
>
> **v2.0:** the whole app is now a **single native C++ process** — no
> WebView, no browser runtime, no Rust backend. One `dutyon-pet.exe`
> embeds the HTTP server, state machine, IDE scanner and metrics sampler,
> and renders Live2D/GIF pets natively with GLFW + OpenGL + Cubism SDK.
> Installs side-by-side with 1.x (shared `~/.dutyon` config), and is the
> codebase we port to low-cost ARM hardware devices.
>
> **New in v2.0.2:** multi-monitor scaling fixed — dragging the pet to a
> lower-resolution monitor no longer shrinks the character and text below
> 1.x size. The resolution-normalization floor is now 1.0, so monitors up
> to 1440p render at exactly the same size as 1.x, and small fonts keep
> crisp pixel rendering on every screen.
>
> **v2.0.1:** the pet no longer shows "idle" while the model is still
> generating a long reply — the LLM thinking phase is completely silent
> (no hook events, measured 3m52s in the wild); its timeout is now 10
> minutes instead of 3 (same fix as v1.3.3 on the 1.x line).
>
> **v1.3.x line (WebView-based, macOS/Linux):** still maintained on the
> `master` branch — latest [v1.3.3](https://github.com/marine841023/duty-on/releases/tag/v1.3.3).
>
> Download v2.0.2 (.zip): [GitHub →](https://github.com/marine841023/duty-on/releases/download/v2.0.2/DutyOn-v2.0.2.zip) · [Gitee →](https://gitee.com/megrezsoft/dutyo/releases/download/v2.0.2/DutyOn-v2.0.2.zip)

**English** · [简体中文](README.zh-CN.md)

</div>

---

Running AI agents in several IDE windows at once? Stop Alt-Tabbing to check
whether they're still working, done, or waiting for your confirmation.
**Duty On** is a tiny transparent Live2D character that floats above your
desktop and shows the live status of every **Trae** / Qoder / Cursor / Codex / OpenCode
session at a glance. **Made for Chinese Trae users**, with native Trae CN /
TraeCode CN title detection and Simplified Chinese as a first-class language:

- 💤 **Sleeping** — everything is idle (she naps, Zzz…)
- ⚡ **Working** — an AI task is running right now
- 🔔 **Alert** — an agent needs your confirmation **right now**

v2.0 is a **single native C++ process**: embedded HTTP server + state
machine + IDE scanner + system-metrics sampler, with the pet rendered
natively via GLFW + OpenGL + Live2D Cubism SDK (or GIF sprites). No WebView,
no browser runtime — and the same codebase builds for low-cost ARM Linux
hardware devices.

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

## Memory footprint, measured

Same machine, same Live2D pet on screen:

| Version | Processes | Working set (sum) | Private memory (sum) |
|---------|-----------|-------------------|----------------------|
| 1.x (WebView) | duty-on.exe (~70 MB) + 6 × WebView2 browser processes (~396 MB) | ~465 MB | ~195 MB |
| **2.0 (native C++)** | **one dutyon-pet.exe, nothing hidden** | **~116 MB** | **~114 MB** |

> **Correction:** 1.x marketing copy claimed "~80 MB RAM". That figure only
> counted the main process and ignored the whole WebView2 browser-process
> group, so real usage was several times higher — our oversight, hereby
> corrected. 2.0 drops WebView2 entirely (GLFW + OpenGL native rendering):
> one process in Task Manager, what you see is what it costs, a real
> reduction of more than half.

## Install

### Download (recommended)

Grab the **`.zip`** from
GitHub [**Releases**](https://github.com/marine841023/duty-on/releases) or Gitee [**Releases**](https://gitee.com/megrezsoft/dutyo/releases),
extract it, and run the NSIS installer inside (`DutyOn_<ver>_x64-setup.exe`).
Distributed as ZIP to bypass Windows SmartScreen on unsigned exe.
Upgrading from 1.x? The installer auto-detects your existing install
location and reuses your `~/.dutyon` config, GIF characters and models.

### Build from source

Requirements: CMake 3.16+, Visual Studio 2022 (MSVC), and the
[Live2D Cubism Native SDK](https://www.live2d.com/sdk/download/native/)
placed at `device/third_party/CubismNativeSdk/` (not redistributed in
this repo for license reasons). All other dependencies (GLFW, nlohmann/json,
stb, Dear ImGui, FreeType) are fetched automatically by CMake.

```bash
git clone -b v2.0-dev https://github.com/marine841023/duty-on.git
cd duty-on/device
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target dutyon-pet
# NSIS installer: powershell ../tools/build-package.ps1
```

### Enable IDE hooks

Right-click the pet → **安装 Hook 集成**, then restart your IDE or start
a new AI session.

## How it works

```
┌─────────────────────────────────────────────┐
│  dutyon-pet.exe (single native C++ process) │
│  ┌───────────────────────────────────────┐  │
│  │  Native client: GLFW + OpenGL         │  │
│  │  Live2D Cubism SDK / GIF sprites      │  │
│  │  States: 💤 sleep / ⚡ work / 🔔 alert │  │
│  │  Dear ImGui menus / status bar /      │  │
│  │  system monitor panel                 │  │
│  ├───────────────────────────────────────┤  │
│  │  Embedded backend:                    │  │
│  │  · State machine (multi-session)      │  │
│  │  · HTTP server (127.0.0.1:17521)      │  │
│  │  · IDE window scanner + hooks install │  │
│  │  · CPU/RAM/GPU/network sampler        │  │
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
whitelist lives in [`device/src/backend/backend_config.h`](device/src/backend/backend_config.h).

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
pet's **切换形象** menu immediately.

## Development

```bash
cd device
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target dutyon-pet
./build/Release/dutyon-pet.exe
```

End-to-end regression script (pet must be running):
`.userdata/test-notification.ps1`.

## Tech stack

C++20 · GLFW · OpenGL · Dear ImGui (FreeType) · Live2D Cubism Native SDK ·
cpp-httplib · nlohmann/json · stb · Trae / Qoder / Cursor / Codex / OpenCode hooks

## Roadmap

- [ ] ARM Linux hardware-device builds (aarch64 cross-toolchain)
- [ ] Per-project alert sounds
- [ ] More IDE integrations (the hook protocol is a plain HTTP POST — PRs welcome)
- [ ] Community model gallery

## Contributing

Issues and PRs are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md).

## License & credits

Code: [MIT](LICENSE). Bundled Live2D runtime and sample models are © Live2D
Inc. and used under their respective licenses — see [NOTICE](NOTICE).
