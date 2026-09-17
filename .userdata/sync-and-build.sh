#!/bin/bash
# 在 WSL 里执行：把本次改动同步到设备源码目录并触发增量编译
# 设备 SSH 凭据不写进仓库：从环境变量 DUTYON_DEVICE / DUTYON_DEVICE_PASS 读取，
# 可在同目录放 deploy.env（已被 .gitignore 忽略）自动加载，模板见 deploy.env.example
set -e
[ -f "$(dirname "$0")/deploy.env" ] && . "$(dirname "$0")/deploy.env"
: "${DUTYON_DEVICE:?请设置 DUTYON_DEVICE=root@<设备IP>，或写入 .userdata/deploy.env}"
: "${DUTYON_DEVICE_PASS:?请设置 DUTYON_DEVICE_PASS=<设备SSH口令>，或写入 .userdata/deploy.env}"
D="$DUTYON_DEVICE"
P=/opt/dutyon-src
B="${DUTYON_REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
SP="sshpass -p $DUTYON_DEVICE_PASS scp -o StrictHostKeyChecking=no"

sshpass -p "$DUTYON_DEVICE_PASS" ssh -o StrictHostKeyChecking=no $D "mkdir -p $P/device/src/audio $P/device/src/net $P/device/src/ui $P/frontend/assets/device/sounds"

$SP $B/device/CMakeLists.txt $D:$P/device/
$SP $B/device/src/config.h $B/device/src/main.cpp $D:$P/device/src/
$SP $B/device/src/api/client.h $B/device/src/api/client.cpp $D:$P/device/src/api/
$SP $B/device/src/audio/sound_player.h $B/device/src/audio/sound_player.cpp $D:$P/device/src/audio/
$SP $B/device/src/net/usb_link.h $B/device/src/net/usb_link.cpp $D:$P/device/src/net/
$SP $B/device/src/render/prompt_banner.h $B/device/src/render/prompt_banner.cpp $B/device/src/render/text_renderer.h $B/device/src/render/text_renderer.cpp $D:$P/device/src/render/
$SP $B/device/src/ui/task_panel.h $B/device/src/ui/task_panel.cpp $D:$P/device/src/ui/
$SP $B/frontend/assets/device/prompt-usb.png $B/frontend/assets/device/font-noto-sc.otf $D:$P/frontend/assets/device/
$SP $B/frontend/assets/device/sounds/*.wav $D:$P/frontend/assets/device/sounds/

$SP $B/.userdata/build-device.sh $D:/tmp/
sshpass -p "$DUTYON_DEVICE_PASS" ssh -o StrictHostKeyChecking=no $D 'bash /tmp/build-device.sh'
echo SYNC-AND-BUILD-DONE
