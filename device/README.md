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
    └── deps/                   # httplib.h（随仓库）、glew-2.2.0（需自行下载）
```

## 依赖

| 依赖 | PC (Windows) | ARM Linux | 说明 |
|---|---|---|---|
| Cubism Native SDK | 需手动下载 | 需手动下载 | Live2D 官方 SDK，放入 `third_party/CubismNativeSdk/` |
| GLEW 2.2.0 | 需手动下载 | — | Cubism Framework 依赖，放入 `third_party/deps/glew-2.2.0/` |
| cpp-httplib | 随仓库分发 | — | 内嵌 HTTP 服务器（单头文件） |
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
cmake -B build -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-toolchain.cmake
cmake --build build -j$(nproc)
```

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
- [ ] ARM 真机交叉编译验证
- [ ] DRM/KMS 直接显示（部分 Mali 驱动需 GBM surface）

## 与 1.x 的关系

| | 1.x | 2.0 |
|---|---|---|
| 前端 | WebView2 (Chromium) + PixiJS + HTML | 无浏览器，C++ 原生 |
| 后端 | Tauri (Rust) 独立进程 | C++ 内嵌单进程（无 Rust） |
| UI 框架 | HTML/CSS/JS | ImGui (PC) |
| Live2D | pixi-live2d-display (WebGL) | Cubism Native SDK (OpenGL) |
| 内存 | 全进程合计 ~465MB 工作集 / ~195MB 私有（主进程 ~70MB + WebView2 进程群 ~396MB；旧宣传 "~80MB" 只统计了主进程，特此勘误） | 实测 ~116MB 工作集 / ~114MB 私有（单进程，所见即所得） |
| 模型格式 | .model3.json | **相同格式，原样复用** |
