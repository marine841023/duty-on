#!/bin/bash
# 设备端编译（增量）：先 configure 同步，再后台 -j2 编译，日志写 /tmp/build.log
: > /tmp/build.log
cd /opt/dutyon-src/device
cmake -B build -DCMAKE_BUILD_TYPE=Release >> /tmp/build.log 2>&1
echo "== CONFIGURE RC=$? ==" >> /tmp/build.log
nohup bash -c 'cd /opt/dutyon-src/device && cmake --build build -j2 >> /tmp/build.log 2>&1; echo "== BUILD RC=$? ==" >> /tmp/build.log' >/dev/null 2>&1 &
echo BUILD-STARTED
