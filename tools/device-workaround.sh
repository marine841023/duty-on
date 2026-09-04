#!/bin/bash
# 用 tracefs 的 sys_enter_openat 跟踪点抓 dutyon-pet 的文件访问路径
T=/sys/kernel/tracing
[ -d $T ] || T=/sys/kernel/debug/tracing
[ -d $T ] || { echo "no tracefs"; exit 1; }
cd $T
echo 0 > tracing_on
echo > trace
echo 'common_pid == 0' > /dev/null  # 占位：不用过滤器，全量抓后 grep
echo 1 > events/syscalls/sys_enter_openat/enable
echo 1 > tracing_on
echo "tracing armed"
