# Duty On 2.0 · 全平台原生运行时

2.0 核心目标：**彻底干掉浏览器/WebView**，PC 和硬件设备共用同一套 C++ 原生代码，
把内存和 CPU 开销降到地板。

## 架构

```
┌─ PC (Windows) ──────────────────────┐
│ duty-on.exe (Rust, 纯后端无窗口)      │
│  └ HTTP/SSE API :17521               │
│    (hooks 接收/状态机/系统监控)        │
│           ▲ localhost                │
│ dutyon-pet.exe (C++ 原生) ◄──────────┤
│  GLFW 透明窗口 + OpenGL 3.3           │
│  Live2D Native + ImGui 监控面板      │
│  ~30MB 内存，无 WebView2 进程群       │
└──────────────────────────────────────┘

┌─ 硬件 (ARM Linux) ──────────────────┐
│ dutyon-device (C++ 同源)             │
│  EGL/GLES2 framebuffer 直渲          │
│  轮询 PC 的 HTTP API（局域网）        │
│  ~60MB 内存（含系统）                 │
└──────────────────────────────────────┘
```

**一套 C++ 代码，两个平台。** 浏览器/WebView 在 2.0 中不存在。

## 目录结构

```
device/
├── CMakeLists.txt              # 双平台构建（Windows + ARM Linux）
├── cmake/
│   └── aarch64-toolchain.cmake # ARM64 交叉编译工具链
├── src/
│   ├── main.cpp                # 统一入口（平台分流）
│   ├── config.h                # 编译期配置
│   ├── api/
│   │   ├── client.h            # HTTP 客户端（状态 + 监控轮询）
│   │   └── client.cpp
│   ├── platform/
│   │   ├── window.h            # 平台窗口抽象接口
│   │   ├── win32_window.cpp    # Windows: GLFW 透明窗口 + 穿透/托盘/拖拽
│   │   └── egl_window.cpp      # ARM Linux: EGL framebuffer 直渲
│   ├── render/
│   │   ├── live2d_renderer.h   # Live2D Cubism Native 封装
│   │   ├── live2d_renderer.cpp # 完整实现（moc3/表情/物理/眨眼/呼吸/动作）
│   │   ├── gles_context.h      # ARM: EGL/GLES2 上下文
│   │   └── gles_context.cpp
│   ├── state/
│   │   ├── machine.h           # 状态机：API 状态 -> Live2D 动作
│   │   └── machine.cpp
│   └── ui/
│       ├── ui_renderer.h       # UI 叠加层抽象
│       └── ui_renderer.cpp     # PC: ImGui 监控面板 + 状态栏
├── models/                     # Live2D 模型（与桌面版同格式）
└── third_party/
    └── CubismNativeSdk/        # Live2D Cubism Native SDK（需自行下载）
```

## 依赖

| 依赖 | PC (Windows) | ARM Linux | 说明 |
|---|---|---|---|
| Cubism Native SDK | 需手动下载 | 需手动下载 | Live2D 官方 SDK，放入 `third_party/CubismNativeSdk/` |
| GLFW | FetchContent | — | 窗口 + OpenGL 上下文 |
| ImGui | FetchContent | — | UI 叠加层（监控面板/状态栏） |
| cpr + nlohmann/json | FetchContent | FetchContent | HTTP 客户端 |
| stb_image | FetchContent | FetchContent | PNG 纹理解码 |
| OpenGL 3.3 | 系统 | — | PC 渲染后端 |
| EGL + GLES3 | — | 系统 | ARM 渲染后端 |

## 构建

### Windows（开发/测试）

```powershell
cd device
mkdir build; cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
.\Release\dutyon-pet.exe
```

### ARM Linux（交叉编译）

```bash
cd device
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-toolchain.cmake ..
make -j$(nproc)
```

## 实现状态

- [x] 平台窗口抽象（`IPlatformWindow`）
- [x] Windows: GLFW 透明无边框窗口 + Win32 穿透/托盘/拖拽/右键菜单
- [x] ARM Linux: EGL/GLES2 framebuffer 直渲
- [x] HTTP 轮询 `/api/status` + `/api/metrics`（与 Rust serde 对齐）
- [x] 状态机（overallState -> 动作组，与桌面版一致）
- [x] Live2D 完整集成（moc3/表情/物理/姿势/眨眼/呼吸/GLES2+GL3 渲染）
- [x] ImGui 监控面板（CPU/RAM/GPU/NET/自占）+ 状态栏
- [x] Rust 后端 `/api/metrics` HTTP 暴露（替代 Tauri event）
- [ ] Cubism Native SDK 放入 third_party（需手动下载，许可限制）
- [ ] PC 端真机构建验证（需 Visual Studio + SDK）
- [ ] ARM 真机交叉编译验证
- [ ] DRM/KMS 直接显示（部分 Mali 驱动需 GBM surface）
- [ ] 自定义模型选择 UI（当前硬编码 nito）
- [ ] 窗口位置持久化

## 与 1.x 的关系

| | 1.x | 2.0 |
|---|---|---|
| 前端 | WebView2 (Chromium) + PixiJS + HTML | 无浏览器，C++ 原生 |
| 后端 | Tauri (Rust) | 同一套 Rust，但纯 API 无窗口 |
| UI 框架 | HTML/CSS/JS | ImGui (PC) / 占位 (ARM) |
| Live2D | pixi-live2d-display (WebGL) | Cubism Native SDK (OpenGL) |
| 内存 | ~80-120MB | 目标 ~30MB (PC) / ~60MB (ARM) |
| 模型格式 | .model3.json | **相同格式，原样复用** |
