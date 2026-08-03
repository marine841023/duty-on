# Contributing to Duty On · 开工啦

Thanks for your interest! Issues and pull requests are welcome.
感谢关注！欢迎提 Issue 和 PR（中英文都可以）。

## Ways to contribute

- **Bug reports** — use the bug report template; include your OS, app version,
  and the relevant lines from `~/.dutyon/frontend.log` (if UI-related)
- **Feature ideas** — open a feature request first so we can align on scope
- **New IDE integrations** — the hook protocol is a plain HTTP POST to
  `127.0.0.1:17521/hook`; see `hooks/hooks-template.json` and
  `src-tauri/src/state_manager.rs` for the event model
- **Translations** — `frontend/i18n.js` holds all 8 locales; keep key order aligned
- **Live2D models** — we can only bundle models with a redistribution-friendly
  license; user models can always be dropped into `~/.dutyon/live2d/` locally

## Development setup

```bash
# Rust stable + Tauri CLI + platform prerequisites (https://v2.tauri.app)
cd src-tauri
cargo tauri dev     # run with DevTools
cargo test          # must stay green (52 tests)
```

No npm/node needed — the frontend is plain static files.

## Conventions

- **Rust**: keep new logic covered by unit tests; state transitions belong in
  `state_manager.rs`, platform APIs behind `click_through.rs` / `ide_scanner.rs`
- **Frontend**: vanilla JS only, no build step; log via `window.__petSendLog`
  so release builds stay diagnosable
- **Commits**: concise imperative messages, Chinese or English both fine
  (e.g. `fix: 迷你模式切换后模型缩放错乱` / `feat: add /live2d file route`)
- Don't commit `src-tauri/target/`, `node_modules/`, or anything under
  `~/.dutyon/` (user data)

## Pull requests

1. Fork & branch from `master`
2. `cargo test` green + `node --check` on edited JS files
3. Describe the user-visible behavior change and how you verified it
