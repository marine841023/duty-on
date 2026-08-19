# ARM64 (aarch64) 交叉编译工具链配置
# 用法: cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/aarch64-toolchain.cmake ..

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# 如果目标板有 sysroot，取消下面注释并修改路径
# set(CMAKE_SYSROOT /path/to/aarch64-sysroot)
# set(CMAKE_FIND_ROOT_PATH /path/to/aarch64-sysroot)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 针对 Cortex-A53 优化
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mcpu=cortex-a53 -mtune=cortex-a53")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcpu=cortex-a53 -mtune=cortex-a53")
