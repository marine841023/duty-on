#!/bin/bash
# WSL 侧执行：上传轮询脚本并等待设备端编译完成（每 20s 查一次，最多 20 分钟）
# 设备 SSH 凭据不写进仓库：从环境变量 DUTYON_DEVICE / DUTYON_DEVICE_PASS 读取，
# 可在同目录放 deploy.env（已被 .gitignore 忽略）自动加载，模板见 deploy.env.example
[ -f "$(dirname "$0")/deploy.env" ] && . "$(dirname "$0")/deploy.env"
: "${DUTYON_DEVICE:?请设置 DUTYON_DEVICE=root@<设备IP>，或写入 .userdata/deploy.env}"
: "${DUTYON_DEVICE_PASS:?请设置 DUTYON_DEVICE_PASS=<设备SSH口令>，或写入 .userdata/deploy.env}"
D="$DUTYON_DEVICE"
SP="sshpass -p $DUTYON_DEVICE_PASS"
REPO="${DUTYON_REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
$SP scp -o StrictHostKeyChecking=no $REPO/.userdata/poll-usb-build.sh $D:/tmp/
for i in $(seq 1 60); do
  out=$($SP ssh -o StrictHostKeyChecking=no $D "bash /tmp/poll-usb-build.sh")
  echo "$out" | tail -3
  if echo "$out" | grep -q "BUILD RC=0"; then echo BUILD-OK; exit 0; fi
  if echo "$out" | grep -q "BUILD RC="; then echo BUILD-FAILED; exit 1; fi
  sleep 20
done
echo BUILD-TIMEOUT
exit 2
