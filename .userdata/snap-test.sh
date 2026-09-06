#!/bin/bash
# 抓帧验证：临时带 DUTYON_SNAPSHOT 运行，截取第 90 帧起的 BMP
systemctl stop dutyon.service
cd /opt/dutyon
rm -f /tmp/snap.*.bmp
DUTYON_SNAPSHOT=/tmp/snap timeout 15 ./dutyon-pet > /tmp/pet-snap.log 2>&1
ls -la /tmp/snap.*.bmp 2>/dev/null
systemctl start dutyon.service
echo "== SNAP DONE =="
