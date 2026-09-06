#!/bin/bash
# 香橙派部署准备：等 apt 完成 -> 装 unzip/rsync -> 下载解压 Cubism SDK
set -e
echo "== 等待 apt 空闲 =="
for i in $(seq 1 120); do
  if ! pgrep -x apt-get >/dev/null && ! pgrep -x dpkg >/dev/null; then break; fi
  sleep 5
done
pgrep -x apt-get >/dev/null && { echo "apt 仍在运行，放弃"; exit 1; }
echo "apt 空闲"

echo "== 工具检查 =="
which unzip rsync >/dev/null 2>&1 || DEBIAN_FRONTEND=noninteractive apt-get install -y -qq unzip rsync

echo "== 下载 Cubism SDK for Native R5 =="
cd /opt
if [ ! -f /opt/CubismSdkForNative-5-r.5.zip ]; then
  curl -fL --retry 3 -o /opt/CubismSdkForNative-5-r.5.zip \
    https://cubism.live2d.com/sdk-native/bin/CubismSdkForNative-5-r.5.zip
fi
ls -lh /opt/CubismSdkForNative-5-r.5.zip

echo "== 解压 =="
if [ ! -d /opt/CubismSdkForNative-5-r.5 ]; then
  unzip -q /opt/CubismSdkForNative-5-r.5.zip -d /opt
fi

echo "== 布置 third_party =="
mkdir -p /opt/dutyon-src/device/third_party/CubismNativeSdk
cd /opt/CubismSdkForNative-5-r.5
cp -r Core /opt/dutyon-src/device/third_party/CubismNativeSdk/
cp -r Framework /opt/dutyon-src/device/third_party/CubismNativeSdk/
echo "== Core linux 库 =="
ls -l /opt/dutyon-src/device/third_party/CubismNativeSdk/Core/lib/linux/
echo "== SETUP DONE =="
