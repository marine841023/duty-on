# Duty On 2.0 · 全平台原生运行时

2.0 核心目标：**彻底干掉浏览器/WebView**，PC 和硬件设备共用同一套 C++ 原生代码，
把内存和 CPU 开销降到地板。

## 架构

```
┌─ PC (Windows) ──────────────────────────────┐
│ dutyon-pet.exe（C++ 单进程）                  │
│  ├ 内嵌 HTTP Server (cpp-httplib) :17521     │
│  │   /hook 接收 · /api/status · /api/metrics │
│  ├ 状态机 + IDE 窗口扫描 + 系统指标采样        │
│  └ GLFW 透明窗口 + OpenGL 3.3                │
│      Live2D Native + ImGui 监控面板          │
│ 实测 ~116MB 单进程，无 WebView2 进程群        │
└─────────────────────────────────────────────┘
                   ▲ 局域网 HTTP 轮询
┌─ 硬件 (ARM Linux) ──────────────────┐
│ dutyon-pet（C++ 同源，ApiClient）     │
│  EGL/GLES framebuffer 直渲          │
│  轮询 PC 的 /api/status + /api/metrics │
└─────────────────────────────────────┘
```

**一套 C++ 代码，两个平台。** 浏览器/WebView 在 2.0 中不存在；
1.x 的 Rust 后端（src-tauri）已移除，全部职责内嵌进 `dutyon-pet.exe`。

## 目录结构

```
device/
├── CMakeLists.txt              # 双平台构建（Windows + ARM Linux）
├── cmake/
│   └── aarch64-toolchain.cmake # ARM64 交叉编译工具链
├── src/
│   ├── main.cpp                # 统一入口（平台分流）
│   ├── config.h                # 编译期配置
│   ├── config/                 # 用户配置（位置记忆等）
│   ├── api/
│   │   └── client.{h,cpp}      # HTTP 客户端（仅 ARM 设备端，轮询 PC API）
│   ├── backend/                # 内嵌后端（原 src-tauri Rust 的 C++ 移植，仅 PC）
│   │   ├── http_server.cpp     #   cpp-httplib 服务器 :17521
│   │   ├── state_manager.cpp   #   多会话状态机
│   │   ├── ide_scanner.cpp     #   IDE 窗口扫描（检测 IDE 关闭）
│   │   ├── sys_monitor.cpp     #   CPU/RAM/GPU/网络指标采样（含 NVML）
│   │   ├── hooks_installer.cpp #   IDE hook 幂等安装（5 IDE）
│   │   ├── autostart.cpp       #   开机自启（注册表）
│   │   └── backend_service.cpp #   后端聚合服务
│   ├── platform/
│   │   ├── window.h            # 平台窗口抽象接口
│   │   ├── win32_window.cpp    # Windows: GLFW 透明窗口 + 穿透/托盘/拖拽
│   │   └── egl_window.cpp      # ARM Linux: EGL framebuffer 直渲
│   ├── render/
│   │   ├── live2d_renderer.cpp # Live2D Cubism Native 封装（moc3/物理/眨眼/动作）
│   │   ├── gif_sprite.cpp      # 自定义 GIF 精灵（WIC 解码多帧）
│   │   └── gles_context.cpp    # ARM: EGL/GLES 上下文
│   ├── state/machine.cpp       # 状态机：API 状态 -> Live2D 动作
│   └── ui/
│       ├── i18n.cpp            # 多语言
│       └── ui_renderer.cpp     # PC: ImGui 监控面板 + 状态栏
└── third_party/
    ├── CubismNativeSdk/        # Live2D Cubism Native SDK（需自行下载）
    └── deps/                   # httplib、GLEW（需自行下载），sha256（随仓库）
```

## 依赖

| 依赖 | PC (Windows) | ARM Linux | 说明 |
|---|---|---|---|
| Cubism Native SDK | 需手动下载 | 需手动下载 | Live2D 官方 SDK，放入 `third_party/CubismNativeSdk/` |
| GLEW 2.2.0 | 需手动下载 | — | Cubism Framework 依赖，放入 `third_party/deps/glew-2.2.0/` |
| cpp-httplib | 需手动下载 | 需手动下载 | 单头文件放入 `third_party/deps/httplib/httplib.h`，设备配网页也使用 |
| GLFW | FetchContent | — | 窗口 + OpenGL 上下文 |
| ImGui + FreeType | FetchContent | — | UI 叠加层（FreeType 保证小字号 CJK 清晰） |
| nlohmann/json | FetchContent | FetchContent | JSON 解析 |
| stb | FetchContent | FetchContent | 图像纹理解码 |
| cpr | — | FetchContent | HTTP 客户端（仅设备端轮询） |
| OpenGL 3.3 | 系统 | — | PC 渲染后端 |
| EGL + GLES3 | — | 系统 | ARM 渲染后端 |

Windows 端零 libcurl/cpr 依赖（少分发 cpr.dll + libcurl.dll）。

## 构建

### Windows（开发/测试）

```powershell
cd device
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
.\build\Release\dutyon-pet.exe
```

### ARM Linux（交叉编译）

```bash
cd device
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
cmake --build build -j$(nproc)
```

### Windows 使用与打包

右键系统托盘图标可显示/隐藏精灵、移至所在屏幕工作区正中、退出；左键图标恢复显示。
角色右键仍打开完整设置菜单。启动会自动刷新 Hook 配置；Codex 信任、事件投递和已安装
是不同状态，排查见 [Hook Review](../docs/hook-review.md)。

在仓库根目录执行 `powershell -File tools/build-package.ps1`，产物位于 `tools/dist/`。
安装包及升级包包含 `hooks/`，运行时无需依赖源码目录。
独立后端与桥接测试命令见 [贡献说明](../CONTRIBUTING.md)。

### 设备热点与部署

没有有效家庭 Wi-Fi 凭据，或连续两次入网失败后，设备进入 `DutyOn-XXXX` 配网热点。
每次入网尝试最多等待约 65 秒，包含接口准备和 DHCP；保存过凭据的设备开机不会立即出现热点。
热点密码首次随机生成后保存在设备身份文件中，断网、重启均沿用屏幕显示的密码。
连接后若手机没有弹出配网页，可直接访问 `http://192.168.4.1/`。

部署时必须同步二进制和以下脚本到 `/opt/dutyon/`，使用 LF 换行并赋予执行权限：
`wifi-common.sh`、`wifi-ap.sh`、`wifi-client.sh`、`wifi-off.sh`、`arp-keepalive.sh`。
其中 `wifi-common.sh` 是其他 Wi-Fi 脚本的运行依赖，不能遗漏。
现有自动同步与部署脚本已包含这些文件。

异常输入和退出流程测试见 [设备端审查记录](../docs/device-review.md)。
回归测试建议在电脑交叉编译后上传运行，避免低内存设备同时运行图形服务和编译器。

## 实现状态

- [x] 平台窗口抽象（`IPlatformWindow`）
- [x] Windows: GLFW 透明无边框窗口 + Win32 穿透/托盘/拖拽/右键菜单
- [x] Windows: 内嵌 HTTP 服务器（/health /api/status /api/metrics /hook /unregister）
- [x] 状态机（多会话追踪 + 超时清理 + overallState -> 动作组）
- [x] IDE 窗口扫描 + hook 幂等安装（Trae/Qoder/Cursor/Codex/OpenCode）
- [x] 系统指标采样（CPU/RAM/GPU-NVML/网络）+ ImGui 监控面板
- [x] Live2D 完整集成（moc3/表情/物理/姿势/眨眼/呼吸/GLES2+GL3 渲染）
- [x] 自定义 GIF 精灵（WIC 多帧解码 + 透明索引）
- [x] 窗口位置持久化（记忆上次启动位置）
- [x] 开机自启（注册表）
- [x] ARM64 交叉编译与设备部署流程（验证范围见设备端审查记录）
- [x] DRM/GBM + EGL/GLES 直接显示

## 与 1.x 的关系

| | 1.x | 2.0 |
|---|---|---|
| 前端 | WebView2 (Chromium) + PixiJS + HTML | 无浏览器，C++ 原生 |
| 后端 | Tauri (Rust) 独立进程 | C++ 内嵌单进程（无 Rust） |
| UI 框架 | HTML/CSS/JS | ImGui (PC) |
| Live2D | pixi-live2d-display (WebGL) | Cubism Native SDK (OpenGL) |
| 内存 | 全进程合计 ~465MB 工作集 / ~195MB 私有（主进程 ~70MB + WebView2 进程群 ~396MB；旧宣传 "~80MB" 只统计了主进程，特此勘误） | 实测 ~116MB 工作集 / ~114MB 私有（单进程，所见即所得） |
| 模型格式 | .model3.json | **相同格式，原样复用** |
