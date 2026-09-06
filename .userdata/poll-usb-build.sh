#!/bin/sh
# 设备端编译进度：尾部日志 + 标记计数
tail -15 /tmp/build.log
echo "--- markers ---"
grep -c "== CONFIGURE RC=" /tmp/build.log 2>/dev/null
grep -c "== BUILD RC=" /tmp/build.log 2>/dev/null
