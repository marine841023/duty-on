#!/bin/bash
# 部署 USB RNDIS gadget 服务 + usb0 静态 IP/ DHCP 服务
set -e

# ---- gadget 脚本 ----
cat > /opt/dutyon/usb-gadget.sh <<'GADGET'
#!/bin/bash
# DutyOn USB gadget：NCM 以太网网卡模式（Windows 10/11 内置 usbnet 免驱）。
# 弃用历史：RNDIS 在本机驱动无法自动匹配（错误 28）；ECM 枚举成功但
# Windows 不为接口建网卡节点。PID/序列号换新值避开 Windows 设备缓存。
# usb_gadget configfs 目录由 libcomposite 注册，必须先加载模块
modprobe libcomposite
modprobe usb_f_ncm
G=/sys/kernel/config/usb_gadget/dutyon
UDC=$(ls /sys/class/udc/ | head -1)
[ -z "$UDC" ] && { echo "no UDC"; exit 1; }
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config

# 幂等：先解绑并清理旧实例（含旧 RNDIS/ECM 实例的残留）
if [ -d "$G" ]; then
  echo "" > "$G/UDC" 2>/dev/null || true
  rm -f "$G/configs/b.1/rndis.usb0" "$G/configs/b.1/ecm.usb0" "$G/configs/b.1/ncm.usb0" 2>/dev/null || true
  rmdir "$G/configs/b.1/strings/0x409" "$G/configs/b.1" 2>/dev/null || true
  rmdir "$G/functions/rndis.usb0" "$G/functions/ecm.usb0" "$G/functions/ncm.usb0" 2>/dev/null || true
  rmdir "$G/strings/0x409" "$G" 2>/dev/null || true
fi

mkdir -p "$G"
# VID/PID 用 NetChip 0x0525，PID 换新值（0xa4a5）避开 Windows 对旧设备缓存的驱动决策
echo 0x0525 > "$G/idVendor"
echo 0xa4a5 > "$G/idProduct"
echo 0x0200 > "$G/bcdUSB"
mkdir -p "$G/strings/0x409"
echo "DUTYON0001"     > "$G/strings/0x409/serialnumber"
echo "DutyOn"         > "$G/strings/0x409/manufacturer"
echo "DutyOn Display" > "$G/strings/0x409/product"
mkdir -p "$G/functions/ncm.usb0"
mkdir -p "$G/configs/b.1/strings/0x409"
echo "DutyOn NCM" > "$G/configs/b.1/strings/0x409/configuration"
ln -s "$G/functions/ncm.usb0" "$G/configs/b.1/"
sleep 1
echo "$UDC" > "$G/UDC"
echo "gadget bound to $UDC"
GADGET
chmod +x /opt/dutyon/usb-gadget.sh

# ---- systemd 单元 ----
cat > /etc/systemd/system/dutyon-usb.service <<'UNIT'
[Unit]
Description=DutyOn USB RNDIS gadget
Before=systemd-networkd.service dutyon.service
Wants=sys-subsystem-net-devices-usb0.device

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStartPre=/bin/sleep 3
ExecStart=/opt/dutyon/usb-gadget.sh

[Install]
WantedBy=multi-user.target
UNIT

# ---- usb0 网络：静态 192.168.7.1 + 内置 DHCP 池 ----
cat > /etc/systemd/network/10-usb0.network <<'NET'
[Match]
Name=usb*

[Network]
Address=192.168.7.1/24
DHCPServer=yes
ConfigureWithoutCarrier=yes

[DHCPServer]
PoolOffset=100
PoolSize=100
NET

systemctl daemon-reload
systemctl enable dutyon-usb.service
systemctl restart systemd-networkd
systemctl start dutyon-usb.service
sleep 3
echo "--- gadget ---"
cat /sys/kernel/config/usb_gadget/dutyon/UDC 2>/dev/null || echo NOT_BOUND
echo "--- 网卡 ---"
ls /sys/class/net/
ip addr show dev usb0 2>/dev/null | head -5
echo "== USB-SYS DONE =="
