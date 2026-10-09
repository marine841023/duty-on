<div align="center">

<img src="docs/assets/hero.png" alt="Duty On" width="720"/>

# 开工啦 (Duty On) · AI 任务桌宠 + 桌面陪伴屏

**让喜欢的角色替你盯梢——AI 在忙什么，一眼就知道。**

[English](README.md) · **简体中文**

### 🚀 v2.0 — 原生 C++ 桌宠 + 硬件陪伴屏

> **为中国 Trae 用户量身打造** — 原生 Trae CN / TraeCode CN 窗口标题识别，
> 自动剥离多根工作区后缀，8 种语言、简体中文优先。
>
> **桌面端：** 整个应用是**单个原生 C++ 进程**——无 WebView、无浏览器运行时。
> 一个 `dutyon-pet.exe` 内嵌 HTTP 服务器、状态机、IDE 扫描器和系统指标采样，
> 用 GLFW + OpenGL + Cubism SDK 原生渲染 Live2D / GIF 精灵，
> 挂着精灵实测内存 ~116MB（1.x WebView 版 ~465MB）。
>
> **硬件设备端（新）：** 同一套 C++ 代码构建的**桌面 AI 任务陪伴屏**——
> 一块小屏立在桌上，Live2D 角色实时演出电脑上所有 AI 会话的状态
> （💤 睡觉 / ⚡ 忙碌 / 🔔 提醒）+ 底部任务清单 + 时钟。
> ARM Linux（DRM/GBM 直渲，无 X11）+ Wi-Fi 配网 + 6 位配对码与电脑配对，
> 未连电脑时自动切换电子相框模式。

**最新版本下载**
| 平台 | 下载 |
|---|---|
| 🖥 Windows 桌面版 v2.0.12 | [GitHub Releases](https://github.com/marine841023/duty-on/releases) · [Gitee Releases](https://gitee.com/megrezsoft/duty-on/releases) |
| 📟 设备端源码 | [v2.0-dev 分支](https://gitee.com/megrezsoft/duty-on/tree/v2.0-dev) `device/` 目录（交叉编译，见下文"设备端构建"） |

</div>

---

## 它是什么？

同时开着好几个 IDE 跑 AI 任务，还要不停 Alt-Tab 检查它们是在干活、干完了、还是卡在等你确认？
「开工啦」把所有 **Trae** / Qoder / Cursor / Codex / OpenCode 会话的实时状态浓缩在一个表情上：

- 💤 **睡觉**：所有 IDE 空闲时，精灵闭眼睡觉，飘出 ZZZ
- ⚡ **忙碌**：有 AI 任务正在执行时，精灵睁眼专注工作
- 🔔 **提醒**：需要你确认操作时，精灵抖动并弹出感叹号

**两种形态，一套代码：**

| | 🖥 桌面版（Windows） | 📟 设备版（ARM Linux 硬件屏） |
|---|---|---|
| 形态 | 桌面透明悬浮 Live2D/GIF 精灵 | 独立小屏（竖屏 480×800 / 横屏 800×480） |
| 渲染 | GLFW + OpenGL + ImGui | DRM/GBM 直渲 GLES3（无 X11） |
| 联动 | 本地内嵌后端直连 | Wi-Fi 轮询 PC 端 `/api/status` |
| 角色与配置 | `~/.dutyon/` 共享 | 自动从 PC 同步角色/模型/音频 |
| 附加 | 状态栏、系统监控、迷你模式 | 任务清单 + 时钟 + 电子相框模式 |

## 📟 硬件设备版（重点介绍）

一块低成本 ARM 板（如 Orange Pi Zero 2W，H616 四核 A53）接一块小屏，
就是一台**桌面 AI 任务陪伴屏**：

- **实时状态演出**：Live2D 角色按 PC 上所有 AI 会话的聚合状态切换动作
  （睡觉 / 工作 / 提醒），任务事件带提示音
- **两段式布局**：上段 Live2D 角色演出区，下段任务清单（每行项目名 +
  状态徽章）；多任务时按任务数动态分屏、角色垂直居中
- **时钟 + 状态图标**：主题色时钟/日期，右上角 Wi-Fi / PC 连接图标
  （断链时图标叠红色大叉，一眼可辨）
- **Wi-Fi 配网**：设备开机即建热点（`DutyOn-XXXX`），手机连接后
  **自动弹出配网页**（captive portal）选家中 Wi-Fi，全程无需键盘；
  PC 端可下发"重新配网"指令切换网络
- **配对与同步**：屏幕显示 6 位配对码，PC 端右键宠物 → 设备 → 配对设备
  输入即完成配对；角色、Live2D 模型、状态音频自动从 PC 同步
- **电子相框模式**：未连电脑开机自动进入照片轮播；连上后切回任务屏
- **声音**：HDMI 输出，任务开始/结束/提醒各有提示音，可绑定自定义音频

### 设备端构建（本地交叉编译）

Windows 宿主本地交叉编译，产物直推设备，无需设备端编译：

```bash
# 1. 放置 Arm GNU 13.2 工具链到 tools/cross/（见 device/cmake/aarch64-toolchain.cmake 注释）
# 2. 从设备拉回 sysroot（EGL/GLES/GBM/DRM 头与库）到 device/sysroot/
# 3. 配置 + 构建（产物：device/build-cross/dutyon-pet，单文件 ~14MB）
cmake -G Ninja -S device -B device/build-cross ^
  -DCMAKE_TOOLCHAIN_FILE=device/cmake/aarch64-toolchain.cmake ^
  -DCMAKE_BUILD_TYPE=Release -DCPR_ENABLE_SSL=OFF
cmake --build device/build-cross -j
# 4. 推送部署（scp → 设备 deploy.sh → 重启服务，约 1 分钟）
powershell .userdata/deploy-cross.ps1
```

> 硬件参考：Orange Pi Zero 2W（H616）+ Debian 13 + HDMI 屏。
> Cubism Native SDK 需手动放置到 `device/third_party/CubismNativeSdk/`。

## 🖥 桌面版功能

- **🎬 自定义 GIF 角色**：任意 **GIF / PNG / JPG / WebP / MP4 / WebM** 创建专属桌宠，
  每个状态（💤 / ⚡ / 🔔）分别上传动画，随时替换
- **自定义 Live2D 模型**：任意 Cubism 4 模型放进 `~/.dutyon/live2d/` 即可在菜单切换
- **多 IDE 监控**：同时监控多个 **Trae** / Qoder / Cursor / Codex / OpenCode 实例，
  5 种 IDE 各有原生 Hook 集成
- **状态栏**：精灵下方显示所有 IDE 项目及状态，点击项目名跳转对应 IDE 窗口
- **迷你模式**：一键缩小为 130×210 桌面角落小伙伴
- **智能点击穿透**：光标在模型/菜单上时可交互，其余区域穿透到下层窗口
- **高清渲染**：2x 超采样，高 DPI 屏幕边缘锐利
- **多语言**：简中/繁中/英/日/韩/法/德/西（自动跟随系统语言）
- **开机自启**：菜单一键开关

## 内存占用：实测数字

| 版本 | 进程构成 | 工作集合计 | 私有内存合计 |
|------|----------|-----------|-------------|
| 1.x（WebView 架构） | duty-on.exe（~70MB）+ 6 个 WebView2 浏览器进程（~396MB） | ~465MB | ~195MB |
| **2.0（原生 C++）** | **dutyon-pet.exe 单进程，无任何隐藏进程** | **~116MB** | **~114MB** |

> **勘误：** 1.x 时代的文案曾宣传"内存仅 ~80MB"，那只统计了主进程。
> 2.0 用 GLFW + OpenGL 原生渲染彻底移除 WebView2，占用所见即所得。

## 快速开始（桌面版）

### 方式一：下载安装包（推荐）

从 GitHub [Releases](https://github.com/marine841023/duty-on/releases) 或 Gitee [Releases](https://gitee.com/megrezsoft/duty-on/releases) 下载 **`.zip`** 压缩包，
解压后运行 `DutyOn_<版本>_x64-setup.exe` 安装（含语言选择与开机自启选项）。
从 1.x 升级？共用 `~/.dutyon` 配置、GIF 形象和模型。

### 方式二：从源码构建

```bash
git clone -b v2.0-dev https://github.com/marine841023/duty-on.git
cd duty-on/device
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target dutyon-pet
# NSIS 安装包：powershell ../tools/build-package.ps1
```

需要 [Live2D Cubism Native SDK](https://www.live2d.com/sdk/download/native/)
（放到 `device/third_party/CubismNativeSdk/`，因许可协议不在仓库内分发）。
其余依赖由 CMake 自动拉取。

### 安装 Hook 集成

右键精灵菜单 → **安装 Hook 集成**，然后**重启 IDE 或开启新的 AI 会话**即可生效。

## 工作原理

```
┌─────────────────────────────────────────────┐
│  dutyon-pet.exe (单个原生 C++ 进程)          │
│  ┌─────────────────────────────┬───────────┐│
│  │ 桌面客户端: GLFW + OpenGL    │ 设备端:    ││
│  │ Live2D/GIF · ImGui 菜单     │ DRM/GBM   ││
│  │ 状态: 💤 / ⚡ / 🔔          │ GLES3 直渲││
│  ├─────────────────────────────┴───────────┤│
│  │ 内嵌后端: 状态机 · HTTP Server ·         ││
│  │ IDE 扫描 + Hook 安装 · 指标采样           ││
│  └────────────┬───────────────┬────────────┘│
└───────────────┼───────────────┼─────────────┘
                │ /hook         │ /api/status 轮询 (Wi-Fi)
   ┌────┬────┬──┴──┬────┬────┐ ┌─────────────┐
   │Trae│Qoder│Cursor│Codex│OC │ │ 📟 硬件屏    │
   └────┴────┴──────┴────┴───┘ └─────────────┘
```

### Hook 事件映射

| Hook 事件 | 触发时机 | 精灵状态 |
|-----------|---------|---------|
| `SessionStart` | IDE 会话创建 | 项目上线 (idle) |
| `UserPromptSubmit` | 用户发送消息 | → 忙碌 (working) |
| `PreToolUse` / `PostToolUse` | AI 工具执行前/后 | → 忙碌 (working) |
| `Notification` | 需要用户确认 | → 提醒 (alert) |
| `Stop` | AI 完成任务 | → 空闲 (idle) |

整体状态优先级：alert > working > sleeping。

## 常见问题

**Q: 精灵不显示 Live2D 模型？**
A: 菜单 → "Hook 状态" / `http://127.0.0.1:17521/health` 排查；
自定义模型需确认目录结构完整（`model3.json` + `.moc3` + 贴图 + 动作）。

**Q: Hook 安装后没有反应？**
A: 确保重启了 IDE 或开启了新的 AI 会话；事件日志在 `~/.dutyon/hook-received.log`。

**Q: 从 1.x 升级后配置还在吗？**
A: 在。两版本共用 `~/.dutyon/config.json` 与形象/模型目录。

## 技术栈

- **C++20 单进程架构** — 桌面端 + 设备端共用核心，无 WebView
- **GLFW + OpenGL + Dear ImGui (FreeType)** — 桌面端窗口与渲染
- **DRM/GBM + GLES3** — 设备端无 X11 直渲
- **Live2D Cubism Native SDK** — Live2D 模型渲染（两端共用）
- **cpp-httplib + nlohmann/json + cpr** — HTTP 服务器与设备轮询
- **Trae / Qoder / Cursor / Codex / OpenCode Hooks** — 5 种 IDE 事件钩子

## 许可证

代码：[MIT](LICENSE)。内置 Live2D 运行时与示例模型 © Live2D Inc.，按其各自许可证使用——见 [NOTICE](NOTICE)。
