cmake_minimum_required(VERSION 3.16)
project(dutyon-2.0 VERSION 2.0.0 LANGUAGES CXX)

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

FetchContent_Declare(cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 1.10.5)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master)

if(WIN32)
    # PC 端：GLFW（窗口）+ ImGui（UI 叠加层）
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.91.5
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(cpr json stb glfw imgui)
else()
    # ARM Linux：系统 EGL + GLES3
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
    )
    target_include_directories(imgui_lib PUBLIC
        ${IMGUI_DIR}
        ${IMGUI_DIR}/backends
    )
    target_link_libraries(imgui_lib PUBLIC glfw opengl32)
endif()

# ---------------------------------------------------------------------------
# 主程序：按平台选择源文件
# ---------------------------------------------------------------------------
set(SOURCES
    src/main.cpp
    src/api/client.cpp
    src/render/live2d_renderer.cpp
    src/state/machine.cpp
    src/ui/ui_renderer.cpp
)

if(WIN32)
    list(APPEND SOURCES src/platform/win32_window.cpp)
else()
    list(APPEND SOURCES
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

target_compile_definitions(dutyon-pet PRIVATE ${CSM_TARGET})

# 平台特定链接
if(WIN32)
    target_link_libraries(dutyon-pet
        cubism_framework
        Live2DCubismCore
        glfw
        imgui_lib
        opengl32
        cpr::cpr
        nlohmann_json::nlohmann_json
        shell32       # Shell_NotifyIcon
        user32
        gdi32
    )
    # 子系统：无控制台窗口（调试用 console 版可注释掉）
    # set_target_properties(dutyon-pet PROPERTIES WIN32_EXECUTABLE TRUE)
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
