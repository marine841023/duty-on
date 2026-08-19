cmake_minimum_required(VERSION 3.16)
project(dutyon-device VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Cubism Native SDK path (user must download and place in third_party/)
set(CUBISM_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/CubismNativeSdk")

find_package(PkgConfig REQUIRED)
pkg_check_modules(EGL REQUIRED egl)
pkg_check_modules(GLES3 REQUIRED glesv3)

# cpr (HTTP client) + nlohmann/json
include(FetchContent)
FetchContent_Declare(cpr
    GIT_REPOSITORY https://github.com/libcpr/cpr.git
    GIT_TAG 1.10.5)
FetchContent_Declare(json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3)
FetchContent_MakeAvailable(cpr json)

add_executable(dutyon-device
    src/main.cpp
    src/api/client.cpp
    src/render/gles_context.cpp
    src/render/live2d_renderer.cpp
    src/state/machine.cpp
)

target_include_directories(dutyon-device PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CUBISM_SDK_PATH}/Core/include
    ${CUBISM_SDK_PATH}/Framework/src
    ${EGL_INCLUDE_DIRS}
    ${GLES3_INCLUDE_DIRS}
)

target_link_directories(dutyon-device PRIVATE
    ${CUBISM_SDK_PATH}/Core/lib/linux/aarch64
    ${EGL_LIBRARY_DIRS}
    ${GLES3_LIBRARY_DIRS}
)

target_link_libraries(dutyon-device
    Live2DCubismCore
    ${EGL_LIBRARIES}
    ${GLES3_LIBRARIES}
    cpr::cpr
    nlohmann_json::nlohmann_json
    pthread
    dl
)

target_compile_definitions(dutyon-device PRIVATE
    CSM_TARGET_LINUX_GL
)
