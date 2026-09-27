#!/bin/bash
# DutyOn Wi-Fi —— client（入网）模式。
#
# 由 dutyon-pet 在拿到家庭 Wi-Fi 凭据（captive portal 提交或已持久化）时调用：
# 先把凭据写到 /run/dutyon/wpa_supplicant.conf，再 setsid 后台执行本脚本。
# 职责：停 AP 守护进程 -> wpa_supplicant 关联家庭 Wi-Fi -> DHCP 取址（与 PC 同网）。
# 关联/取址是否成功由 dutyon-pet 轮询 wlan0 是否有非 192.168.4.x 的 IP 判定，
# 超时它会回退 AP 重新配网，故本脚本无需返回精确状态。
#
# 用法： wifi-client.sh [iface]   默认 iface=wlan0
IFACE="${1:-wlan0}"
CONF=/run/dutyon

log(){ echo "[wifi-client] $*"; }

rfkill unblock wifi 2>/dev/null || true
if command -v nmcli >/dev/null 2>&1; then nmcli dev set "$IFACE" managed no 2>/dev/null || true; fi

# 停 AP 侧 + 旧 client（幂等）
pkill -x hostapd 2>/dev/null || true
pkill -x dnsmasq 2>/dev/null || true
pkill -f "wpa_supplicant.*$IFACE" 2>/dev/null || true
dhcpcd -k "$IFACE" 2>/dev/null || true
pkill -x udhcpc 2>/dev/null || true
sleep 1

ip link set "$IFACE" down 2>/dev/null || true
sleep 1
ip addr flush dev "$IFACE" 2>/dev/null || true
ip link set "$IFACE" up

# 关联家庭 Wi-Fi（-B 后台守护，掉线自动重连；驱动优先 nl80211 回退 wext）
if [ ! -f "$CONF/wpa_supplicant.conf" ]; then log "missing $CONF/wpa_supplicant.conf"; exit 1; fi
if wpa_supplicant -B -i "$IFACE" -c "$CONF/wpa_supplicant.conf" -D nl80211,wext \
        -P "$CONF/wpa_supplicant.pid"; then
    log "wpa_supplicant started"
else
    log "wpa_supplicant FAILED"
    exit 1
fi

# DHCP 取址：按可用性依次尝试 dhcpcd / udhcpc / dhclient（setup-wifi.sh 装 dhcpcd5）
if command -v dhcpcd >/dev/null 2>&1; then
    dhcpcd -4 -t 25 "$IFACE" && log "dhcpcd ok" || log "dhcpcd FAILED (关联上了但没拿到地址?)"
elif command -v udhcpc >/dev/null 2>&1; then
    udhcpc -i "$IFACE" -q -t 25 -T 1 && log "udhcpc ok" || log "udhcpc FAILED"
elif command -v dhclient >/dev/null 2>&1; then
    dhclient -1 -timeout 25 "$IFACE" && log "dhclient ok" || log "dhclient FAILED"
else
    log "no dhcp client available (装 dhcpcd5)"
fi

ip -4 addr show dev "$IFACE" 2>/dev/null | grep -w inet || log "no IPv4 yet (稍后由 poll 检测)"
log "client done"
