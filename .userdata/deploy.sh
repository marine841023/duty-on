#!/bin/bash
# 部署 /opt/dutyon：二进制 + FrameworkShaders + Live2D 模型
set -e
SRC=/opt/dutyon-src
DST=/opt/dutyon
mkdir -p $DST/assets
install -m755 $SRC/device/build/dutyon-pet $DST/dutyon-pet
# Wi-Fi 配网脚本（wifi_manager 运行期按 /opt/dutyon/wifi-*.sh 调用；剥 CRLF 防坏解释器）
if [ -d $SRC/device/scripts ]; then
  for s in wifi-ap.sh wifi-client.sh wifi-off.sh; do
    [ -f $SRC/device/scripts/$s ] && { sed 's/\r$//' $SRC/device/scripts/$s > $DST/$s; chmod 755 $DST/$s; }
  done
fi
mkdir -p /run/dutyon
rm -rf $DST/FrameworkShaders
# 设备端是 GLES：用 StandardES 版 shader（与 Standard 同名文件，#version 100）
cp -r $SRC/device/third_party/CubismNativeSdk/Framework/src/Rendering/OpenGL/Shaders/StandardES $DST/FrameworkShaders
rm -rf $DST/assets/live2d
cp -r $SRC/frontend/assets/live2d $DST/assets/live2d
# 任务列表文字字体（见 config.h kFontPath）
cp $SRC/frontend/assets/device/font-noto-sc.otf $DST/assets/font-noto-sc.otf
# 事件提示音（见 config.h kSoundDir；缺失时 sound_player 自动回退正弦波合成）
rm -rf $DST/assets/sounds
cp -r $SRC/frontend/assets/device/sounds $DST/assets/sounds
ls -la $DST; ls $DST/FrameworkShaders | head -3; ls $DST/assets/live2d | head -5
ls -la $DST/assets/sounds
echo "== DEPLOY DONE =="
