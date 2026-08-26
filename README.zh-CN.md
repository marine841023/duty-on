<div align="center">

<img src="docs/assets/hero.png" alt="Duty On" width="720"/>

# 开工啦 (Duty On) - Live2D 桌面精灵监控

**让喜欢的角色替你盯梢——AI 在忙什么，一眼就知道。**

[English](README.md) · **简体中文**

### 🚀 v2.0.3 — 原生 C++ 重写版！

> **为中国 Trae 用户量身打造** — 原生 Trae CN / TraeCode CN 窗口标题识别，
> 自动剥离多根工作区后缀（工作区 / Workspace / ワークスペース / 작업 영역），
> 8 种语言、简体中文优先。
>
> **v2.0：** 整个应用重写为**单个原生 C++ 进程**——无 WebView、无浏览器运行时、
> 无 Rust 后端。一个 `dutyon-pet.exe` 内嵌 HTTP 服务器、状态机、IDE 扫描器和
> 系统指标采样，用 GLFW + OpenGL + Cubism SDK 原生渲染 Live2D / GIF 精灵。
> 与 1.x 可交替安装（共用 `~/.dutyon` 配置），也是移植到低成本 ARM 硬件设备的代码基线。
>
> **v2.0.3 更新：** 宠物完全后台运行——启动后任务栏不再出现图标，仅保留
> 系统托盘。窗口改为隐藏创建、应用工具窗口样式后再显示。
>
> **v2.0.2：** 修复跨显示器缩放——把宠物拖到低分辨率屏（如 1080p@100%）
> 时角色和字体不再缩得过小，≤1440p 的屏与 1.x 尺寸完全一致。
>
> **v2.0.1：** 模型正在生成长回复时宠物不再误显示"空闲"——LLM 思考期
> 完全静默（无任何 hook 事件），实测可长达 3 分 52 秒，其超时兜底从 3 分钟
> 放宽到 10 分钟（与 1.x v1.3.3 同源修复）。
>
> **v1.3.x 系列（WebView 版，支持 macOS/Linux）：** 仍在 `master` 分支维护——
> 最新 [v1.3.3](https://github.com/marine841023/duty-on/releases/tag/v1.3.3)。
>
> 下载 v2.0.3 (.zip)：[GitHub →](https://github.com/marine841023/duty-on/releases/download/v2.0.3/DutyOn-v2.0.3.zip) · [Gitee →](https://gitee.com/megrezsoft/dutyo/releases/download/v2.0.3/DutyOn-v2.0.3.zip)

</div>

---

同时开着好几个 IDE 跑 AI 任务，还要不停 Alt-Tab 检查它们是在干活、干完了、还是卡在等你确认？
「开工啦」是一个透明悬浮的 Live2D 小人儿，**为中国 Trae 用户量身打造**，
把所有 **Trae** / Qoder / Cursor / Codex / OpenCode 会话的实时状态浓缩在一个表情上：

- 💤 **睡觉**：所有 IDE 空闲时，精灵闭眼睡觉，飘出 ZZZ
- ⚡ **忙碌**：有 AI 任务正在执行时，精灵睁眼专注工作
- 🔔 **提醒**：需要你确认操作时，精灵抖动并弹出感叹号

v2.0 是**单个原生 C++ 进程**：内嵌 HTTP 服务器 + 状态机 + IDE 扫描 + 系统指标采样，
精灵用 GLFW + OpenGL + Live2D Cubism SDK（或 GIF 精灵）原生渲染。
无 WebView、无浏览器运行时——同一套代码可构建低成本 ARM Linux 硬件设备版本。

## 功能

- **🎬 自定义 GIF 角色**：用任意 **GIF / PNG / JPG / WebP / MP4 / WebM** 文件创建专属桌宠！
  为每个状态（💤 睡觉 / ⚡ 忙碌 / 🔔 提醒）分别上传动画，随时可重新上传替换。
  无需 Live2D 模型——选个 GIF 就能让桌宠活过来。大图自动缩放（最大 1024px）；
  缓存失效机制确保重新上传后一定显示新动画。
- **Live2D 精灵**：浮在桌面最顶层的透明无边框窗口，支持拖拽移动、位置记忆
- **状态栏**：精灵下方显示所有已连接的 IDE 项目及状态，带 T/Q/C/X/O 徽章区分 Trae/Qoder/Cursor/Codex/OpenCode
- **点击跳转**：点击状态栏中的项目名，自动激活对应的 IDE 窗口
- **多 IDE 支持**：同时监控多个 **Trae** / Qoder / Cursor / Codex / OpenCode 实例，5 种 IDE 各有原生 Hook 集成
- **智能点击穿透**：光标落在模型/菜单上时可点击，其余区域鼠标事件穿透到下层窗口
- **迷你模式**：一键缩小为 130×210 的桌面角落小伙伴（菜单切换，双向还原）
- **高清渲染**：超采样渲染（2x 分辨率缓冲），高 DPI 屏幕下边缘锐利
- **21 个内置动作**：点击精灵随机触发，菜单可点播任意动作，状态机自动联动
- **自定义 Live2D 模型**：把任意 Cubism 4 模型放进 `~/.dutyon/live2d/` 即可在菜单中切换
- **多语言**：简中/繁中/英/日/韩/法/德/西（自动跟随系统语言）
- **开机自启**：菜单一键开关（Windows 注册表）

## 内存占用：实测数字

同一台机器、同样挂着 Live2D 精灵的实测结果：

| 版本 | 进程构成 | 工作集合计 | 私有内存合计 |
|------|----------|-----------|-------------|
| 1.x（WebView 架构） | duty-on.exe（~70MB）+ 6 个 WebView2 浏览器进程（~396MB） | ~465MB | ~195MB |
| **2.0（原生 C++）** | **dutyon-pet.exe 单进程，无任何隐藏进程** | **~116MB** | **~114MB** |

> **勘误：** 1.x 时代的文案曾宣传"内存仅 ~80MB"，那只统计了主进程，
> 没有算上 WebView2 运行时拉起的整组浏览器进程，实际总占用是宣传数字的
> 数倍，是我们当时的疏忽，特此说明。2.0 用 GLFW + OpenGL 原生渲染彻底
> 移除了 WebView2——任务管理器里只有一个进程，占用所见即所得，
> 实际总占用真实下降一半以上。

## 快速开始

### 方式一：下载安装包（推荐）

从 GitHub [Releases](https://github.com/marine841023/duty-on/releases) 或 Gitee [Releases](https://gitee.com/megrezsoft/dutyo/releases) 下载 **`.zip`** 压缩包，
解压后运行里面的 `DutyOn_<版本>_x64-setup.exe` 安装（当前用户安装，含简/繁中/英/日/韩语言选择）。
采用 ZIP 分发以避免 Windows SmartScreen 拦截未签名 exe。
从 1.x 升级？安装器会自动定位原有安装目录，并共用 `~/.dutyon` 里的配置、GIF 形象和模型。

### 方式二：从源码构建

环境要求：CMake 3.16+、Visual Studio 2022（MSVC），以及
[Live2D Cubism Native SDK](https://www.live2d.com/sdk/download/native/)
（放到 `device/third_party/CubismNativeSdk/`，因许可协议不在仓库内分发）。
其余依赖（GLFW、nlohmann/json、stb、Dear ImGui、FreeType）由 CMake 自动拉取。

```bash
git clone -b v2.0-dev https://github.com/marine841023/duty-on.git
cd duty-on/device
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target dutyon-pet
# NSIS 安装包：powershell ../tools/build-package.ps1
```

### 安装 Hook 集成

右键精灵菜单 → **安装 Hook 集成**，
然后**重启 IDE 或开启新的 AI 会话**即可生效。
桥接脚本：`hooks/trae-hook-bridge.ps1`（Windows）、`hooks/trae-hook-bridge.sh`（macOS/Linux）。

## 工作原理

```
┌─────────────────────────────────────────────┐
│  dutyon-pet.exe (单个原生 C++ 进程)          │
│  ┌───────────────────────────────────────┐  │
│  │  原生客户端: GLFW + OpenGL             │  │
│  │  Live2D Cubism SDK / GIF 精灵         │  │
│  │  状态: 💤 睡觉 / ⚡ 忙碌 / 🔔 提醒     │  │
│  │  Dear ImGui 菜单 / 状态栏 /           │  │
│  │  系统监控面板                         │  │
│  ├───────────────────────────────────────┤  │
│  │  内嵌后端:                             │  │
│  │  · 状态机 (多会话追踪 + 超时清理)       │  │
│  │  · HTTP Server (127.0.0.1:17521)      │  │
│  │  · IDE 窗口扫描 + Hook 安装           │  │
│  │  · CPU/内存/GPU/网络 采样             │  │
│  └───────────────────────────────────────┘  │
└──────────────────┬──────────────────────────┘
                   │ HTTP POST /hook (localhost)
   ┌───────┬───────┼───────┬───────┬───────┐
┌──┴──┐ ┌──┴──┐ ┌──┴──┐ ┌──┴──┐ ┌──┴────┐
│Trae │ │Qoder│ │Cursor│ │Codex│ │OpenCode│
└─────┘ └─────┘ └─────┘ └─────┘ └───────┘
```

### Hook 事件映射

| Hook 事件 | 触发时机 | 精灵状态 |
|-----------|---------|---------|
| `SessionStart` | IDE 会话创建 | 项目上线 (idle) |
| `UserPromptSubmit` | 用户发送消息 | → 忙碌 (working) |
| `PreToolUse` / `PostToolUse` | AI 工具执行前/后 | → 忙碌 (working) |
| `Notification` | 需要用户确认 | → 提醒 (alert) |
| `Stop` | AI 完成任务 | → 空闲 (idle) |
| `PreToolUse`(AskUserQuestion) _(Qoder)_ | Qoder 弹出问答 | → 提醒 (alert) |
| `PermissionRequest` _(Qoder)_ | Qoder 请求权限 | → 提醒 (alert) |
| `PermissionRequest` _(Codex)_ | Codex CLI 请求权限 | → 提醒 (alert) |
| `permission.ask` _(OpenCode)_ | OpenCode 请求权限 | → 提醒 (alert) |

整体状态优先级：alert > working > sleeping。模糊 `Notification` 默认按"任务完成"处理
（白名单见 [`device/src/backend/backend_config.h`](device/src/backend/backend_config.h)）。

## 自定义 GIF 角色（v1.3.0+）

没有 Live2D 模型？没问题！用任意 **GIF / PNG / MP4** 文件创建专属桌宠：

1. 右键精灵 → **切换形象** → **+ 新建形象**
2. 输入名字后，为每个状态上传动画：
   - 💤 **睡觉**（空闲）——没有 AI 任务时显示
   - ⚡ **忙碌**——AI 任务执行中显示
   - 🔔 **提醒**——需要确认操作时显示
3. 点击自定义角色上的 **✎** 可随时重新上传动画

支持格式：GIF（动画）、PNG/JPG/WebP（静态图）、MP4/WebM/MOV（视频）。
大图自动缩放至最大 1024px，保持轻量。文件存储在 `~/.dutyon/animations/<角色ID>/`。

## 自定义 Live2D 模型

把模型目录放到 `~/.dutyon/live2d/<名字>/`（含 `<名字>.moc3` + 贴图 + `model3.json` + 动作），
精灵菜单的"切换形象"里立刻就能看到（原生客户端直接从磁盘加载，无需重启）。

## 测试

```bash
cd device
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target dutyon-pet
```

端到端回归脚本（需先启动精灵）：`.userdata/test-notification.ps1`。

## API 接口

本地 HTTP 服务器 (`http://127.0.0.1:17521`)：

| 接口 | 方法 | 说明 |
|------|------|------|
| `/hook` | POST | 接收 Hook 事件 |
| `/status` | GET | 当前状态快照（1.x 兼容格式） |
| `/api/status` | GET | 状态快照（2.0 原生格式） |
| `/api/metrics` | GET | CPU / 内存 / GPU / 网络实时指标 |
| `/api/events` | GET | 事件流（SSE） |
| `/health` | GET | 健康检查 |
| `/unregister` | POST | 注销会话 |
| `/api/hooks` · `/api/hooks/install` | GET/POST | Hook 安装状态 / 触发安装 |
| `/api/autostart` | GET/POST | 开机自启开关 |
| `/api/quit` | POST | 退出程序 |

## 技术栈

- **C++20 单进程架构** — 原生客户端 + 内嵌后端，无 WebView / 浏览器运行时
- **GLFW + OpenGL + Dear ImGui (FreeType)** — 窗口、渲染与 UI
- **Live2D Cubism Native SDK** — Live2D 模型渲染
- **cpp-httplib + nlohmann/json** — 内嵌 HTTP 服务器与事件处理
- **Trae / Qoder / Cursor / Codex / OpenCode Hooks** — 5 种 IDE 的 AI 生命周期事件钩子

## 常见问题

**Q: 精灵不显示 Live2D 模型？**
A: 菜单 → "Hook 状态" / `http://127.0.0.1:17521/health` 排查进程状态；
自定义模型需确认目录结构完整（`model3.json` + `.moc3` + 贴图 + 动作）。

**Q: Hook 安装后没有反应？**
A: 确保重启了 IDE 或开启了新的 AI 会话；菜单 → "Hook 状态"可查看诊断，
事件落盘日志在 `~/.dutyon/hook-received.log`。

**Q: 精灵一直显示睡觉？**
A: 检查 `http://127.0.0.1:17521/health` 是否存活，并确认 Hook 已安装。

**Q: AI 完成了却显示"需要确认"？**
A: 模糊 Notification 默认按完成处理；确认类型白名单见
`device/src/backend/backend_config.h`（`kNotificationConfirmTypes`）。

**Q: 从 1.x 升级后配置还在吗？**
A: 在。两版本共用 `~/.dutyon/config.json` 与形象/模型目录；安装器会自动
装回原目录，可与 1.x 交替安装。

## 许可证

代码：[MIT](LICENSE)。内置 Live2D 运行时与示例模型 © Live2D Inc.，按其各自许可证使用——见 [NOTICE](NOTICE)。
