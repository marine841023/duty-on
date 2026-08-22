# Contributing to Duty On · 开工啦

Thanks for your interest! Issues and pull requests are welcome.
感谢关注！欢迎提 Issue 和 PR（中英文都可以）。

## Ways to contribute

- **Bug reports** — use the bug report template; include your OS, app version,
  and steps to reproduce
- **Feature ideas** — open a feature request first so we can align on scope
- **New IDE integrations** — the hook protocol is a plain HTTP POST to
  `127.0.0.1:17521/hook`; see `hooks/hooks-template.json` and
  `device/src/backend/state_manager.cpp` for the event model
- **Translations** — `device/src/ui/i18n.cpp` holds all locales; keep key
  order aligned
- **Live2D models** — we can only bundle models with a redistribution-friendly
  license; user models can always be dropped into `~/.dutyon/live2d/` locally

## Development setup

2.0 is a single-process C++ application (embedded HTTP server + state machine
+ native Live2D client). See `device/README.md` for prerequisites.

```powershell
cd device
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\Release\dutyon-pet.exe
```

No npm/node needed.

## Conventions

- **C++**: state transitions belong in `device/src/backend/state_manager.cpp`;
  platform window/click-through logic in `device/src/platform/`, IDE window
  scanning in `device/src/backend/ide_scanner.cpp`
- **Commits**: concise imperative messages, Chinese or English both fine
  (e.g. `fix: 迷你模式切换后模型缩放错乱` / `feat: add /live2d file route`)
- Don't commit `device/build/`, `device/third_party/` (Cubism SDK license
  forbids redistribution), or anything under `~/.dutyon/` (user data)

## Pull requests

1. Fork & branch from the target branch (`master` for 1.x, `v2.0-dev` for 2.0)
2. `cmake --build` green on your platform
3. Describe the user-visible behavior change and how you verified it
