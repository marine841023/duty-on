cmake_minimum_required(VERSION 3.16)
project(dutyon-2.0 VERSION 2.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 源码含中文注释/字符串（UTF-8 无 BOM），MSVC 默认按系统代码页（GBK）解析
# 会报 C2001/C1075；强制 /utf-8。
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ---------------------------------------------------------------------------
# 平台检测
# ---------------------------------------------------------------------------
if(WIN32)
    set(PLATFORM_NAME "Windows")
    set(CSM_TARGET "CSM_TARGET_WIN_GL")
    set(CUBISM_CORE_LIB_DIR "windows/x86_64/142")
else()
    set(PLATFORM_NAME "Linux-ARM")
    set(CSM_TARGET "CSM_TARGET_LINUX_GL")
    set(CUBISM_CORE_LIB_DIR "linux/aarch64")
endif()

message(STATUS "Building for: ${PLATFORM_NAME}")

# ---------------------------------------------------------------------------
# Cubism Native SDK（需手动下载放入 third_party/CubismNativeSdk/）
#   下载: https://www.live2d.com/download/cubism-sdk/download-native/
#   目录: third_party/CubismNativeSdk/{Core,Framework}
# ---------------------------------------------------------------------------
set(CUBISM_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/CubismNativeSdk")
set(CUBISM_CORE_PATH "${CUBISM_SDK_PATH}/Core")
set(CUBISM_FRAMEWORK_PATH "${CUBISM_SDK_PATH}/Framework")

if(NOT EXISTS "${CUBISM_CORE_PATH}/include")
    message(WARNING
        "Cubism Native SDK not found at ${CUBISM_SDK_PATH}. "
        "Download it from live2d.com and place Core/ + Framework/ there.")
endif()

# ---------------------------------------------------------------------------
# 第三方依赖（FetchContent 自动拉取）
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master)

if(WIN32)
    # PC 端：GLFW（窗口）+ ImGui（UI 叠加层）+ FreeType（字体 hinting 光栅化）
    # stb_truetype 无 hinting：8~11px 小字号 CJK 笔画抗锯齿后糊成灰色
    # （低分辨率屏 ui_scale<1 时尤为明显），FreeType 走 TrueType 原生
    # hinting，笔画对齐像素网格，同字号清晰得多。
    # httplib（cpp-httplib 单头）：内嵌 HTTP 服务器（原 duty-on.exe 的
    # /hook 接收 + 硬件显示只读 API，单进程化）。网络受限，随仓库分发
    # 单头文件（third_party/deps/httplib/httplib.h），不走 FetchContent。
    # Windows 无 cpr：HTTP 客户端轮询（ApiClient）仅设备端使用，PC 端
    # 内嵌后端直连，零 libcurl/cpr 依赖（少分发 cpr.dll + libcurl.dll）。
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.5
        GIT_SHALLOW TRUE)
    FetchContent_Declare(freetype
        GIT_REPOSITORY https://github.com/freetype/freetype.git
        GIT_TAG VER-2-13-3
        GIT_SHALLOW TRUE)
    # 只要 TTF/TTC 光栅化：关掉 zlib/bzip2/png/harfbuzz/brotli 子依赖
    set(FT_DISABLE_ZLIB ON)
    set(FT_DISABLE_BZIP2 ON)
    set(FT_DISABLE_PNG ON)
    set(FT_DISABLE_HARFBUZZ ON)
    set(FT_DISABLE_BROTLI ON)
    # glfw 保持 DLL（此前由 cpr 的 CMakeLists 隐式置 ON，现显式声明）：
    # 预处理器已定义 GLFW_DLL，换静态库会 dllimport 链接失败。
    # json/stb 为 header-only，不受该开关影响。
    set(BUILD_SHARED_LIBS ON)
    FetchContent_MakeAvailable(json stb glfw imgui)
    # freetype 单独引入并强制静态 —— 运行期免分发 freetype.dll
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(freetype)
else()
    # ARM Linux：系统 EGL + GLES3 + cpr（设备端轮询 PC 的 /api/status）
    FetchContent_Declare(cpr
        GIT_REPOSITORY https://github.com/libcpr/cpr.git
        GIT_TAG 1.10.5)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(EGL REQUIRED egl)
    pkg_check_modules(GLES3 REQUIRED glesv3)
    FetchContent_MakeAvailable(cpr json stb)
endif()

# ---------------------------------------------------------------------------
# Cubism Framework 源码（随项目编译，匹配当前编译器/架构）
# 只保留 OpenGL 渲染后端，排除 D3D9/D3D11/Vulkan（我们没有这些 SDK）
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CUBISM_FRAMEWORK_SOURCES
    "${CUBISM_FRAMEWORK_PATH}/src/*.cpp")
list(FILTER CUBISM_FRAMEWORK_SOURCES EXCLUDE REGEX "/Rendering/(D3D9|D3D11|Vulkan)/")

add_library(cubism_framework STATIC ${CUBISM_FRAMEWORK_SOURCES})
target_include_directories(cubism_framework PUBLIC
    "${CUBISM_FRAMEWORK_PATH}/src"
    "${CUBISM_CORE_PATH}/include"
)
target_compile_definitions(cubism_framework PUBLIC ${CSM_TARGET})
target_link_directories(cubism_framework PUBLIC
    "${CUBISM_CORE_PATH}/lib/${CUBISM_CORE_LIB_DIR}")

# Windows 下 Cubism Framework 需要链接桌面 OpenGL + GLEW（官方 glew-2.2.0
# win32 release 包：include/GL/glew.h + lib/Release/x64/glew32s.lib 静态库）
if(WIN32)
    set(GLEW_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/deps/glew-2.2.0")
    target_include_directories(cubism_framework PUBLIC "${GLEW_ROOT}/include")
    target_compile_definitions(cubism_framework PUBLIC GLEW_STATIC)
    target_link_libraries(cubism_framework PUBLIC
        opengl32
        "${GLEW_ROOT}/lib/Release/x64/glew32s.lib"
    )
endif()

# ---------------------------------------------------------------------------
# ImGui 静态库（仅 PC 端需要）
# ---------------------------------------------------------------------------
if(WIN32)
    set(IMGUI_DIR "${imgui_SOURCE_DIR}")
    add_library(imgui_lib STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
        ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp
    )
    target_include_directories(imgui_lib PUBLIC
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
        ${IMGUI_DIR}/misc/freetype
    )
    # PUBLIC：ui_renderer.cpp 等直接 include imgui.h 的编译单元必须与
    # imgui_lib 使用同一 ImFontConfig 布局（IMGUI_ENABLE_FREETYPE 会
    # 增补 RasterizerFlags 等字段，两侧不一致即 UB）
    target_compile_definitions(imgui_lib PUBLIC IMGUI_ENABLE_FREETYPE)
    target_link_libraries(imgui_lib PUBLIC glfw opengl32 freetype)
endif()

# ---------------------------------------------------------------------------
# 主程序：按平台选择源文件
# ---------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/config/user_config.cpp
    src/render/gif_sprite.cpp
    src/render/live2d_renderer.cpp
    src/state/machine.cpp
    src/ui/i18n.cpp
    src/ui/ui_renderer.cpp
)

if(WIN32)
    # 环境变量门控的诊断工具（DUTYON_FT_PROBE / DUTYON_MEM），从 main.cpp 抽出
    # 内嵌后端（原 src-tauri Rust 后端的 C++ 移植：状态机/IDE 扫描/指标
    # 采样/hook 安装器/HTTP 服务器/服务聚合）
    list(APPEND SOURCES
        src/platform/win32_window.cpp
        src/debug_diag.cpp
        src/backend/state_manager.cpp
        src/backend/ide_scanner.cpp
        src/backend/sys_monitor.cpp
        src/backend/hooks_installer.cpp
        src/backend/autostart.cpp
        src/backend/http_server.cpp
        src/backend/backend_service.cpp)
else()
    # ApiClient（HTTP 轮询 PC 端 API）仅设备端使用；PC 端内嵌后端直连
    list(APPEND SOURCES
        src/api/client.cpp
        src/platform/egl_window.cpp
        src/render/gles_context.cpp)
endif()

add_executable(dutyon-pet ${SOURCES})

target_include_directories(dutyon-pet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CUBISM_CORE_PATH}/include
    ${CUBISM_FRAMEWORK_PATH}/src
    ${stb_SOURCE_DIR}
)

if(WIN32)
    # cpp-httplib 单头文件（third_party/deps/httplib/httplib.h 随仓库分发）
    target_include_directories(dutyon-pet PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/deps/httplib)
endif()

target_compile_definitions(dutyon-pet PRIVATE ${CSM_TARGET})

# 诊断：生成 link map（含行号），崩溃偏移可直查源码位置
if(MSVC)
    target_link_options(dutyon-pet PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/dutyon-pet.map" "/MAPINFO:LINES")
endif()

# 平台特定链接
if(WIN32)
    target_link_libraries(dutyon-pet
        cubism_framework
        Live2DCubismCore_MD
        glfw
        imgui_lib
        opengl32
        nlohmann_json::nlohmann_json
        ws2_32        # cpp-httplib 内嵌 HTTP 服务器（Winsock）
        shell32       # Shell_NotifyIcon
        user32
        gdi32
        psapi         # EnumProcessModules（崩溃诊断）
    )
    # 子系统 WINDOWS：启动不弹控制台黑窗。入口仍是 main()，需显式指定
    # mainCRTStartup（否则链接器找 WinMain 报错）。调试要看 printf 日志时
    # 从控制台/重定向启动仍可拿到输出（stdout 句柄有效时 printf 正常写）
    set_target_properties(dutyon-pet PROPERTIES WIN32_EXECUTABLE TRUE)
    target_link_options(dutyon-pet PRIVATE "/ENTRY:mainCRTStartup")

    # Cubism SDK 5 的 OpenGL shader 在运行时从文件加载（FrameworkShaders/*.vert|frag，
    # 相对路径由 live2d_renderer.cpp 的 FrameworkLoadFile 解析到 exe 同目录）。
    # 构建后把 Standard 版 shader 复制到输出目录的 FrameworkShaders/ 下。
    add_custom_command(TARGET dutyon-pet POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:dutyon-pet>/FrameworkShaders"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CUBISM_FRAMEWORK_PATH}/src/Rendering/OpenGL/Shaders/Standard"
            "$<TARGET_FILE_DIR:dutyon-pet>/FrameworkShaders"
    )
else()
    target_include_directories(dutyon-pet PRIVATE
        ${EGL_INCLUDE_DIRS}
        ${GLES3_INCLUDE_DIRS}
    )
    target_link_directories(dutyon-pet PRIVATE
        ${EGL_LIBRARY_DIRS}
        ${GLES3_LIBRARY_DIRS}
    )
    target_link_libraries(dutyon-pet
        cubism_framework
        Live2DCubismCore
        ${EGL_LIBRARIES}
        ${GLES3_LIBRARIES}
        cpr::cpr
        nlohmann_json::nlohmann_json
        pthread
        dl
    )
endif()
