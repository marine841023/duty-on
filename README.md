# 开工啦 (Duty On) - Live2D 桌面精灵监控

一个浮在桌面顶层的 Live2D 精灵，用于实时监控多个 Trae IDE 实例的 AI 任务状态。
中文名「开工啦」，英文名「Duty On」。

基于 **Tauri 2.0 + Rust** 构建（系统 WebView，无 Chromium），跨 Windows / macOS / Linux，
内存占用约 70-90MB（原 Electron 版约 446MB，已废弃并移除）。

## 功能

- **Live2D 精灵**：浮在桌面最顶层的透明无边框窗口，支持拖拽移动、位置记忆
- **三种状态动画**：
  - 💤 **睡觉**：所有 IDE 空闲时，精灵闭眼睡觉，飘出 ZZZ
  - ⚡ **忙碌**：有 AI 任务正在执行时，精灵睁眼专注工作
  - 🔔 **提醒**：需要你确认操作时，精灵抖动并弹出感叹号
- **状态栏**：精灵下方显示所有已连接的 Trae IDE 项目及其状态
- **点击跳转**：点击状态栏中的项目名，自动激活对应的 Trae IDE 窗口
- **多 IDE 支持**：同时监控多个 Trae IDE 实例，每个实例独立追踪
- **智能点击穿透**：光标落在模型/菜单上时可点击，其余区域鼠标事件穿透到下层窗口
- **多语言**：简体中文 / 繁體中文 / English / 日本語 / 한국어（自动跟随系统语言）
- **开机自启**：菜单一键开关（Windows 注册表 / macOS LaunchAgent / Linux autostart）

## 工作原理

```
┌─────────────────────────────────────────────┐
│       DutyOn (Tauri 2.0 桌面应用)            │
│  ┌───────────────────────────────────────┐  │
│  │  前端: Live2D 精灵 (pixi-live2d)       │  │
│  │  状态: 💤 睡觉 / ⚡ 忙碌 / 🔔 提醒     │  │
│  ├───────────────────────────────────────┤  │
│  │  Rust 后端:                            │  │
│  │  · 状态机 (多会话追踪 + 超时清理)       │  │
│  │  · HTTP Server (127.0.0.1:17521)      │  │
│  │  · IDE 窗口扫描 (检测 IDE 关闭)        │  │
│  │  · 点击穿透轮询 (30ms 光标位置)        │  │
│  └───────────────────────────────────────┘  │
└──────────────────┬──────────────────────────┘
                   │ HTTP POST /hook
                   │ (localhost)
    ┌──────────────┼──────────────┐
    │              │              │
┌───┴───┐    ┌────┴───┐    ┌────┴───┐
│ Trae  │    │ Trae   │    │ Trae   │
│ IDE 1 │    │ IDE 2  │    │ IDE 3  │
│Proj A │    │Proj B  │    │Proj C  │
└───────┘    └────────┘    └────────┘
  Hook        Hook           Hook
  事件         事件           事件
```

### Trae IDE Hook 事件映射

| Hook 事件 | 触发时机 | 精灵状态 |
|-----------|---------|---------|
| `SessionStart` | IDE 会话创建 | 项目上线 (idle) |
| `UserPromptSubmit` | 用户发送消息 | → 忙碌 (working) |
| `PreToolUse` | AI 即将执行工具 | → 忙碌 (working) |
| `PostToolUse` | AI 工具执行完成 | → 仍忙碌 (working) |
| `Notification` | 需要用户确认 | → 提醒 (alert!) |
| `Stop` | AI 完成任务 | → 空闲 (idle) |

整体状态优先级：alert > working > sleeping。`Notification` 事件按白名单分类：
`notification_type=task_complete` 或含完成类关键词 → idle；含 `tool_name` 或确认类关键词 → alert；
模糊事件默认按完成处理（可在 `src-tauri/src/config.rs` 调整）。

## 快速开始

### 1. 环境要求

- [Rust](https://rustup.rs/)（stable）
- Tauri CLI：`cargo install tauri-cli --version "^2"`
- 各平台 prerequisites 见 [Tauri 官方文档](https://v2.tauri.app/start/prerequisites/)
  （Windows 需 WebView2；Linux 需 `libwebkit2gtk-4.1-0`、`libssl3`）

前端是纯静态文件（`frontend/`），无需 npm/构建步骤。

### 2. 启动精灵（开发模式）

```bash
cd src-tauri
cargo tauri dev
```

精灵会出现在屏幕右下角，初始状态为"睡觉"。开发模式下会自动打开 DevTools。

### 3. 打包安装包

```bash
cd src-tauri
cargo tauri build
```

产物位于 `src-tauri/target/release/bundle/`：
- Windows：NSIS 安装包（当前用户安装，含简/繁中/英/日/韩语言选择）
- macOS：DMG
- Linux：AppImage / DEB

发布方式：**只通过安装包分发**（项目不包含在线更新功能）。

### 4. 安装 Hook 集成

有两种方式安装 Hook：

**方式一**：在精灵的菜单中点击"安装 Hook 集成"

**方式二**：运行独立安装脚本（Windows）

```powershell
.\hooks\install-hooks.ps1
```

安装后，**重启 Trae IDE 或开启新的 AI 会话**，Hook 即可生效。
Hook 桥接脚本：`hooks/trae-hook-bridge.ps1`（Windows）、`hooks/trae-hook-bridge.sh`（macOS/Linux）。

## 测试

### 单元测试（Rust）

状态机核心逻辑（事件→状态映射、多会话优先级、超时清理、提醒重复）与 Hook 配置合并逻辑
由 Rust 单元测试覆盖（11 个用例）：

```bash
cd src-tauri
cargo test
```

### 端到端回归脚本

需先启动精灵（`cargo tauri dev` 或已安装的版本），再运行：

```powershell
# 5 步状态流转回归：sleeping → working → alert → sleeping → 清理
.\.userdata\test-flow.ps1

# Notification 白名单分类：task_complete→idle、模糊→idle、tool_name→alert
.\.userdata\test-notification.ps1
```

### 手动测试

在精灵菜单中可以选择"预览提醒效果"来查看提醒动画，"播放动作"子菜单可触发任意动作。

也可以手动发送测试事件：

```powershell
# 测试忙碌状态
$body = '{"session_id":"test-1","hook_event_name":"UserPromptSubmit","project_path":"C:\test-project","project_name":"test-project"}'
Invoke-RestMethod -Uri 'http://127.0.0.1:17521/hook' -Method Post -Body $body -ContentType 'application/json'

# 测试提醒状态（带 tool_name 视为需确认 → alert）
$body = '{"session_id":"test-1","hook_event_name":"Notification","project_path":"C:\test-project","project_name":"test-project","tool_name":"RunCommand"}'
Invoke-RestMethod -Uri 'http://127.0.0.1:17521/hook' -Method Post -Body $body -ContentType 'application/json'

# 测试任务完成通知（notification_type=task_complete → idle，不触发提醒）
$body = '{"session_id":"test-1","hook_event_name":"Notification","project_path":"C:\test-project","project_name":"test-project","notification_type":"task_complete"}'
Invoke-RestMethod -Uri 'http://127.0.0.1:17521/hook' -Method Post -Body $body -ContentType 'application/json'

# 测试空闲状态
$body = '{"session_id":"test-1","hook_event_name":"Stop","project_path":"C:\test-project","project_name":"test-project"}'
Invoke-RestMethod -Uri 'http://127.0.0.1:17521/hook' -Method Post -Body $body -ContentType 'application/json'
```

## 项目结构

```
traeSprite/
├── src-tauri/                      # Rust 后端 (Tauri 2.0)
│   ├── src/
│   │   ├── main.rs                 # 二进制入口 → lib::run()
│   │   ├── lib.rs                  # 应用装配：窗口/插件/后台线程/IPC 注册
│   │   ├── config.rs               # 集中常量 (端口/超时/关键词白名单/正则)
│   │   ├── state_manager.rs        # 状态机：多会话追踪 + 超时清理 (8 个单测)
│   │   ├── server.rs               # axum HTTP 服务器 (127.0.0.1:17521)
│   │   ├── commands.rs             # 21 个 Tauri IPC 命令
│   │   ├── click_through.rs        # 三平台光标轮询 → 点击穿透切换
│   │   ├── ide_scanner.rs          # 三平台 IDE 窗口枚举 (检测 IDE 关闭)
│   │   ├── hooks_installer.rs      # Hook 配置幂等合并 (3 个单测)
│   │   ├── models.rs               # 编译期生成的模型目录
│   │   └── user_config.rs          # ~/.dutyon/config.json 持久化
│   ├── build.rs                    # 扫描 frontend/assets/live2d 生成 models.gen.rs
│   ├── capabilities/               # Tauri 权限声明
│   ├── icons/
│   ├── Cargo.toml
│   └── tauri.conf.json
├── frontend/                       # 纯静态前端 (无构建步骤)
│   ├── index.html                  # 页面骨架 + 库加载器 (本地优先, CDN 兜底)
│   ├── renderer.js                 # Live2D 渲染 + 状态动画 + 菜单 + 拖拽/穿透上报
│   ├── tauri-bridge.js             # window.petAPI → Tauri invoke 映射
│   ├── i18n.js                     # 5 语言翻译
│   ├── styles.css
│   ├── lib/                        # pixi.js / Cubism Core / pixi-live2d-display
│   └── assets/live2d/              # 5 个模型 + 21 个动作
├── hooks/                          # IDE Hook 桥接脚本与安装器
│   ├── trae-hook-bridge.ps1        # Windows 桥接 (被 Trae IDE 调用)
│   ├── trae-hook-bridge.sh         # macOS/Linux 桥接
│   ├── hooks-template.json         # hooks.json 配置模板
│   └── install-hooks.ps1           # 独立安装脚本
└── .userdata/                      # 端到端测试脚本与事件 fixture
    ├── test-flow.ps1               # 5 步状态流转回归脚本
    ├── test-notification.ps1       # Notification 白名单分类测试
    └── ev-*.json                   # 测试用 Hook 事件样本
```

## 自定义 Live2D 模型

将 Live2D 模型放入 `frontend/assets/live2d/` 即可，无需改代码：

1. 目录结构为 `<name>/<name>.moc3` + 贴图 + （可选）`<name>.model3.json`
2. `build.rs` 在编译时扫描该目录，自动生成模型目录（`models.gen.rs`）
3. 重新编译后，新模型会出现在精灵菜单的"切换形象"列表中
4. 动作文件放在 `frontend/assets/live2d/motion/`，按 `Tap`/`Flick*`/`Idle` 等组自动归类

## API 接口

精灵运行一个本地 HTTP 服务器 (`http://127.0.0.1:17521`)：

| 接口 | 方法 | 说明 |
|------|------|------|
| `/hook` | POST | 接收 Hook 事件 |
| `/status` | GET | 获取当前状态快照 |
| `/health` | GET | 健康检查 |
| `/unregister` | POST | 注销会话 |
| `/log` | POST | 前端诊断日志转发 |

## 技术栈

- **Tauri 2.0** - 桌面应用框架（系统 WebView2 / WKWebView / WebKitGTK）
- **Rust** - 后端：tokio + axum（HTTP）、serde、regex、sys-locale；
  平台层：windows crate / core-graphics / x11rb
- **PixiJS v7** - 2D 渲染引擎
- **pixi-live2d-display** - Live2D 模型加载与渲染
- **Live2D Cubism Core SDK** - Live2D 运行时核心
- **Trae IDE Hooks** - AI 生命周期事件钩子

## 常见问题

**Q: 精灵不显示 Live2D 模型？**
A: 检查 `frontend/assets/live2d/` 下的模型文件是否完整；开发模式下查看自动打开的
DevTools 控制台，诊断日志也会 POST 到 `/log`（Rust 侧 `RUST_LOG=info` 可见）。

**Q: Hook 安装后没有反应？**
A: 确保重启了 Trae IDE 或开启了新的 AI 会话。检查 `~/.trae-cn/hooks.json` 是否包含
`.dutyon` 配置（菜单 → "Hook 状态"可查看诊断信息）。

**Q: 精灵一直显示睡觉？**
A: 确认精灵正在运行（检查 `http://127.0.0.1:17521/health`），并确认 Hook 已正确安装。

**Q: AI 任务完成了，精灵却显示"需要确认"？**
A: Notification 事件若没有可识别的完成/确认特征（`notification_type`、`tool_name`、
关键词），会被当作模糊事件处理。默认按"任务完成"处理（不提醒）；若你希望模糊事件也
触发提醒，可在 `src-tauri/src/config.rs` 中将 `ALERT_ON_AMBIGUOUS_NOTIFICATION` 设为
`true`。完成类与确认类的白名单同样在该文件中调整。

**Q: Wayland (Linux) 下点击穿透不工作？**
A: Wayland 没有全局光标/窗口 API，此时精灵降级为"始终可点击"模式（X11 下正常）。

**Q: 多个 IDE 同时工作，状态栏显示不全？**
A: 状态栏支持滚动，最多显示约 5 个项目。超出部分可滚动查看。
