# ARM64 (aarch64) 交叉编译工具链配置 —— Windows 宿主 → Orange Pi Zero 2W (H616)
#
# 工具链：Arm GNU 13.2.Rel1（mingw-w64 宿主，glibc 2.38 目标运行时；
# 设备是 Debian 13 / glibc 2.41，向后兼容 ≥2.38 的产物）。
# 放置位置：<仓库根>/tools/cross/arm-gnu-toolchain-13.2.Rel1-mingw-w64-i686-aarch64-none-linux-gnu/
# （下载 zip 解压即得，见 tools/cross/arm-gnu.zip）
#
# sysroot：设备拉回的 EGL/GBM/DRM/GLES 头文件与 .so（device/sysroot/，
#   由 ssh tar 从设备 /usr/include + /usr/lib/aarch64-linux-gnu 导出）。
#   glibc/libstdc++ 不用 sysroot —— 工具链自带，且以 -static-libstdc++
#   -static-libgcc 静态链入，运行期只动态依赖设备系统的 libEGL/libGLESv2/
#   libgbm/libdrm + glibc。
#
# 用法（仓库根目录）：
#   cmake -G Ninja -S device -B device/build-cross ^
#     -DCMAKE_TOOLCHAIN_FILE=device/cmake/aarch64-toolchain.cmake ^
#     -DCMAKE_BUILD_TYPE=Release -DCPR_ENABLE_SSL=OFF
#   cmake --build device/build-cross -j
# 部署：.userdata/deploy-cross.ps1（scp 二进制 → 设备 deploy.sh → 重启服务）

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# 从本文件位置反推仓库根，定位工具链与 sysroot（免硬编码绝对路径）
get_filename_component(_THIS_DIR  "${CMAKE_CURRENT_LIST_FILE}" DIRECTORY)  # <root>/device/cmake
get_filename_component(_DEVICE_DIR "${_THIS_DIR}" DIRECTORY)               # <root>/device
get_filename_component(_ROOT_DIR   "${_DEVICE_DIR}" DIRECTORY)             # <root>

set(CROSS_TOOLS "${_ROOT_DIR}/tools/cross/arm-gnu-toolchain-13.2.Rel1-mingw-w64-i686-aarch64-none-linux-gnu")
set(SYSROOT "${_DEVICE_DIR}/sysroot")

set(CMAKE_C_COMPILER   "${CROSS_TOOLS}/bin/aarch64-none-linux-gnu-gcc.exe")
set(CMAKE_CXX_COMPILER "${CROSS_TOOLS}/bin/aarch64-none-linux-gnu-g++.exe")

# find_path/find_library 只在 sysroot 里找目标机资源；find_program（ninja 等
# 构建工具）仍在宿主找
set(CMAKE_FIND_ROOT_PATH "${SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# H616 = 4x Cortex-A53
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -mcpu=cortex-a53 -mtune=cortex-a53")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=cortex-a53 -mtune=cortex-a53")

# C++ 运行时静态链入（工具链 libstdc++ 面向 glibc 2.38，直接动态部署到
# 设备也可用[2.41 兼容]，但静态免分发、少一个运行期依赖判断）
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libstdc++ -static-libgcc")

# 交叉链接：动态 .so（libEGL/libGLESv2/libgbm/libdrm）的传递依赖解析不走 -L，
# 需要 -rpath-link 显式指向 sysroot 库目录；CMake 默认注入的 build rpath 是
# Windows 盘符路径（含 D:），冒号会被 GNU ld 当路径分隔符 → 传递依赖全找不到，
# 必须禁用 build rpath（二进制运行期走设备系统库路径，无需 rpath）
set(CMAKE_SKIP_BUILD_RPATH TRUE)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-rpath-link,${SYSROOT}/usr/lib/aarch64-linux-gnu")
