# Duty On Device · 硬件端 Native 运行时

2.0 版本核心目标：**彻底去掉浏览器**，在嵌入式 Linux 开发板上直接运行 Live2D 桌宠，
将 CPU 占用降到个位数、整机内存压到 100MB 以内。

## 架构对比

| | 1.x 桌面版 | 2.0 硬件版 |
|---|---|---|
| 运行时 | Tauri + 系统 WebView | 原生可执行文件（无浏览器） |
| Live2D 渲染 | WebGL（Chromium/Skia） | OpenGL ES 3 + Cubism Native SDK |
| UI 框架 | PixiJS + HTML/CSS | 无（直接 framebuffer/DRM） |
| 状态获取 | HTTP 轮询 `/api/status` | HTTP 轮询 `/api/status`（相同 API） |
| 模型资源 | `~/.dutyon/live2d/` | SD 卡 `/opt/dutyon/models/`（相同格式） |
| CPU 占用 | ~15-25%（WebView） | 目标 <10% |
| 内存占用 | ~80-120MB | 目标 <60MB |

## 目录结构

```
device/
├── CMakeLists.txt          # 构建脚本（交叉编译 ARM64）
├── src/
│   ├── main.cpp            # 入口：初始化 + 主循环
│   ├── config.h            # 编译期/运行期配置
│   ├── api/
│   │   ├── client.h        # HTTP 客户端（轮询桌面端状态）
│   │   └── client.cpp
│   ├── render/
│   │   ├── gles_context.h  # EGL/GLES2 上下文初始化
│   │   ├── gles_context.cpp
│   │   ├── live2d_renderer.h   # Cubism Native 封装
│   │   └── live2d_renderer.cpp
│   └── state/
│       ├── machine.h       # 状态机：API 状态 -> Live2D 动作/表情
│       └── machine.cpp
├── models/                 # Live2D 模型文件（.model3.json 等，从桌面版复制）
└── third_party/
    └── CubismNativeSdk/    # Live2D Cubism Native SDK（需自行下载放入）
```

## 依赖

- **Live2D Cubism Native SDK**（proprietary，需从 [Live2D 官网](https://www.live2d.com/download/cubism-sdk/download-native/) 下载，
  解压后把 `Core/` 和 `Framework/` 放入 `third_party/CubismNativeSdk/`）
- **stb_image.h**（纹理 PNG 解码，CMake FetchContent 自动拉取）
- OpenGL ES 3.0+ / EGL（Mali-G31 支持）
- cpr + nlohmann/json（HTTP 客户端，FetchContent 自动拉取）
- CMake 3.16+，交叉编译工具链 `aarch64-linux-gnu-gcc`

## 实现状态

- [x] EGL/GLES2 上下文（无 X11/Wayland 直渲）
- [x] HTTP 轮询 `/api/status`（字段与桌面端 Snapshot serde 结构对齐）
- [x] 状态机（overallState -> 动作组，与桌面版 DEFAULT_STATE_MOTIONS 一致）
- [x] Live2D 完整集成（moc3/表情/物理/姿势/眨眼/呼吸/动作预载/GLES2 渲染）
- [ ] Cubism Native SDK 放入 third_party（需手动下载，许可限制）
- [ ] 真机交叉编译验证
- [ ] DRM/KMS 直接显示（当前用 EGL_DEFAULT_DISPLAY，部分驱动需 GBM surface）

## 目标硬件

- Orange Pi Zero 2W（全志 H618，Mali-G31 MP2，OpenGL ES 3.2）
- 任何支持 OpenGL ES 3.0 的 ARM64 Linux 板

## 构建

```bash
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-toolchain.cmake ..
make -j$(nproc)
```
