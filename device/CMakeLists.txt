cmake_minimum_required(VERSION 3.16)
project(dutyon-device VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
        "Download it from live2d.com and place Core/ + Framework/ there. "
        "Configure will continue but link will fail without the SDK.")
endif()

# ---------------------------------------------------------------------------
# 系统依赖：EGL + OpenGL ES 3
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES3 REQUIRED glesv3)

# ---------------------------------------------------------------------------
# 第三方库：cpr(HTTP) + nlohmann/json + stb(纹理解码)
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
FetchContent_MakeAvailable(cpr json stb)

# ---------------------------------------------------------------------------
# Cubism Framework 源码（随项目编译，匹配当前编译器/架构）
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CUBISM_FRAMEWORK_SOURCES
    "${CUBISM_FRAMEWORK_PATH}/src/*.cpp")

add_library(cubism_framework STATIC ${CUBISM_FRAMEWORK_SOURCES})
target_include_directories(cubism_framework PUBLIC
    "${CUBISM_FRAMEWORK_PATH}/src"
    "${CUBISM_CORE_PATH}/include"
)
target_compile_definitions(cubism_framework PUBLIC CSM_TARGET_LINUX_GL)
target_link_directories(cubism_framework PUBLIC
    "${CUBISM_CORE_PATH}/lib/linux/aarch64")

# ---------------------------------------------------------------------------
# 主程序
# ---------------------------------------------------------------------------
add_executable(dutyon-device
    src/main.cpp
    src/api/client.cpp
    src/render/gles_context.cpp
    src/render/live2d_renderer.cpp
    src/state/machine.cpp
)

target_include_directories(dutyon-device PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CUBISM_CORE_PATH}/include
    ${CUBISM_FRAMEWORK_PATH}/src
    ${stb_SOURCE_DIR}          # stb_image.h
    ${EGL_INCLUDE_DIRS}
    ${GLES3_INCLUDE_DIRS}
)

target_link_directories(dutyon-device PRIVATE
    ${CUBISM_CORE_PATH}/lib/linux/aarch64
    ${EGL_LIBRARY_DIRS}
    ${GLES3_LIBRARY_DIRS}
)

target_link_libraries(dutyon-device
    cubism_framework
    Live2DCubismCore
    ${EGL_LIBRARIES}
    ${GLES3_LIBRARIES}
    cpr::cpr
    nlohmann_json::nlohmann_json
    pthread
    dl
)

target_compile_definitions(dutyon-device PRIVATE CSM_TARGET_LINUX_GL)
