#!/bin/bash
# 部署 /opt/dutyon：二进制 + FrameworkShaders + Live2D 模型
set -e
SRC=/opt/dutyon-src
DST=/opt/dutyon
mkdir -p $DST/assets
install -m755 $SRC/device/build/dutyon-pet $DST/dutyon-pet
rm -rf $DST/FrameworkShaders
# 设备端是 GLES：用 StandardES 版 shader（与 Standard 同名文件，#version 100）
cp -r $SRC/device/third_party/CubismNativeSdk/Framework/src/Rendering/OpenGL/Shaders/StandardES $DST/FrameworkShaders
rm -rf $DST/assets/live2d
cp -r $SRC/frontend/assets/live2d $DST/assets/live2d
# 开机引导横幅（未插 USB 时屏底提示；路径见 config.h kPromptBannerPath）
cp $SRC/frontend/assets/device/prompt-usb.png $DST/assets/prompt-usb.png
# 任务列表文字字体（见 config.h kFontPath）
cp $SRC/frontend/assets/device/font-noto-sc.otf $DST/assets/font-noto-sc.otf
ls -la $DST; ls $DST/FrameworkShaders | head -3; ls $DST/assets/live2d | head -5
echo "== DEPLOY DONE =="
