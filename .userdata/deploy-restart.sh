#!/bin/bash
# 设备端：部署最新二进制 + 重启服务 + 打印最近日志
bash /tmp/deploy.sh >/dev/null 2>&1
systemctl restart dutyon.service
sleep 3
journalctl -u dutyon.service -n 25 --no-pager
echo "== DEPLOY-RESTART DONE =="
