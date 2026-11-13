# 设备端界面离屏预览（仅 Windows 开发机）：
#   cmake -S device/tools/ui-preview -B device/build/ui-preview -G "Visual Studio 17 2022" -A x64
#   cmake --build device/build/ui-preview --config Release
#   device/build/ui-preview/Release/ui_preview.exe frontend/assets/device/font-noto-sc.otf <时钟字体> <输出目录> [角色图片]
cmake_minimum_required(VERSION 3.16)
project(dutyon_ui_preview LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DEVICE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(DEPS "${DEVICE_DIR}/third_party/deps")
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/glfw-3.4" glfw EXCLUDE_FROM_ALL)

add_executable(ui_preview
    ui_preview.cpp
    "${DEVICE_DIR}/src/ui/task_panel.cpp"
    "${DEVICE_DIR}/src/ui/i18n.cpp"
    "${DEVICE_DIR}/src/render/text_renderer.cpp")
target_compile_definitions(ui_preview PRIVATE DUTYON_UI_PREVIEW GLEW_STATIC NOMINMAX)
target_include_directories(ui_preview PRIVATE
    "${DEVICE_DIR}/src"
    "${DEPS}/glew-2.2.0/include"
    "${DEPS}/stb-master"
    "${DEPS}/json-3.11.3/include")
target_link_libraries(ui_preview PRIVATE glfw opengl32
    "${DEPS}/glew-2.2.0/lib/Release/x64/glew32s.lib")
if(MSVC)
    target_compile_options(ui_preview PRIVATE /utf-8)
    target_link_options(ui_preview PRIVATE /NODEFAULTLIB:LIBCMT)
endif()
