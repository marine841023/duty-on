#!/bin/sh
# DutyOn 开机首屏 + 并行预热 v2 —— 由 dutyon-splash.service 在 userspace
# ~1.3s 调用（DefaultDependencies=no 超早启动；Before=dutyon.service 只保证
# 开机图先上屏）。本脚本必须秒回，绝不阻塞 dutyon.service。

# 1. fbcon 开机图（BGRX 800x480 raw，已按物理方向预旋转）：
#    fb0 由 sun4i-drm 内核 ~0.9s 就绪；此图填住「上电 → GL 启动帧」之间的
#    黑屏窗口。HDMI console 已静默（console=serial），无人并发写 fb0。
LOGO=/boot/dutyon-bootlogo.raw
if [ -f "$LOGO" ]; then
    dd if="$LOGO" of=/dev/fb0 bs=512k 2>/dev/null && echo "[splash] boot logo drawn"
else
    echo "[splash] WARN: $LOGO missing"
fi

# 2. ondemand 计 iowait 为忙：冷启动页入风暴计为「忙」，防 CPU 躺在
#    480MHz 低频拖慢动态链接与缺页处理（低频是各阶段都慢的放大器）
echo 1 > /sys/devices/system/cpu/cpufreq/ondemand/io_is_busy 2>/dev/null || true

# 3. 后台并行预热（setsid 脱离，读满即退）：
#    Mesa llvmpipe 路径依赖 libgallium(35MB)+libLLVM(123MB)，microSD 实测
#    ~32MB/s——v1 串行预热（Before=dutyon 硬等）实测 14s 反而拖慢整体。
#    dutyon 启动后 EGL 冷初始化需读这 ~160MB 大库，与预热流共享页缓存：
#    预热先到哪 dutyon 就命中到哪。HDMI 声卡（内核 deferred probe ~8.4s）
#    不再是门槛：SoundPlayer 有 aplay 断流重建（respawn），声卡晚就绪自愈。
#    读序 = 使用序：二进制/依赖 → EGL 大件 → 动画（首帧就要）→ Live2D
#    模型（第 3 帧加载）→ 字体（任务面板，角色可见之后才用，排最后）。
DEPS=$(ldd /opt/dutyon/dutyon-pet 2>/dev/null | awk '/=>/{print $3}')
setsid sh -c "cat /opt/dutyon/dutyon-pet $DEPS /usr/lib/aarch64-linux-gnu/libgallium*.so* /usr/lib/aarch64-linux-gnu/libLLVM*.so* /usr/lib/aarch64-linux-gnu/dri/libdril_dri.so /root/.dutyon/animations/* /opt/dutyon/assets/live2d/* /opt/dutyon/assets/font-noto-sc.otf > /dev/null 2>&1" >/dev/null 2>&1 &
echo "[splash] background warmup started"

exit 0
