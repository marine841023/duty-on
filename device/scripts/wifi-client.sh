#!/bin/bash
# DutyOn Wi-Fi —— client（入网）模式。
#
# 由 dutyon-pet 在拿到家庭 Wi-Fi 凭据（captive portal 提交或已持久化）时调用：
# 先把凭据写到 /run/dutyon/wpa_supplicant.conf，再 setsid 后台执行本脚本。
# 职责：停 AP 守护进程 -> wpa_supplicant 关联家庭 Wi-Fi -> DHCP 取址（与 PC 同网）。
# 关联/取址是否成功由 dutyon-pet 检查 client 进程、链路及有效 IPv4 判定，
# 超时它会回退 AP 重新配网，故本脚本无需返回精确状态。
#
# 用法： wifi-client.sh [iface]   默认 iface=wlan0
IFACE="${1:-wlan0}"
CONF=${DUTYON_WIFI_RUN_DIR:-/run/dutyon}

log(){ echo "[wifi-client] $*"; }
source "$(dirname "$0")/wifi-common.sh" || exit 1

mkdir -p "$CONF"
exec 9>"$CONF/wifi.lock"
flock -w 20 9 || { log "timed out waiting for Wi-Fi transition"; exit 1; }
mode_is_current client || exit 0

# 等 wlan0 出现（最多 10s）：dutyon.service 已不等网络（提早启动抢占屏幕），
# 本脚本可能跑在 sprdwl_ng 驱动加载完成之前；等到了再继续，避免
# wpa_supplicant 因接口不存在直接失败、拖到 join 超时才重试
for i in $(seq 1 20); do
    ip link show "$IFACE" >/dev/null 2>&1 && break
    [ "$i" = 1 ] && log "waiting for $IFACE (driver loading...)"
    sleep 0.5
done
ip link show "$IFACE" >/dev/null 2>&1 || { log "missing $IFACE"; exit 1; }

rfkill unblock wifi 2>/dev/null || true
if command -v nmcli >/dev/null 2>&1; then nmcli dev set "$IFACE" managed no 2>/dev/null || true; fi

# 停 AP 侧 + 旧 client（幂等）
stop_owned hostapd
stop_owned dnsmasq
stop_clients
sleep 1

ip link set "$IFACE" down 2>/dev/null || true
sleep 1
ip addr flush dev "$IFACE" 2>/dev/null || true
ip link set "$IFACE" up

# 关联家庭 Wi-Fi（-B 后台守护，掉线自动重连；驱动优先 nl80211 回退 wext）
if [ ! -f "$CONF/wpa_supplicant.conf" ]; then log "missing $CONF/wpa_supplicant.conf"; exit 1; fi
if wpa_supplicant -B -i "$IFACE" -c "$CONF/wpa_supplicant.conf" -D nl80211,wext \
        -P "$CONF/wpa_supplicant.pid" 9>&-; then
    log "wpa_supplicant started"
else
    log "wpa_supplicant FAILED"
    exit 1
fi

# DHCP 取址：按可用性依次尝试 dhcpcd / udhcpc / dhclient（setup-wifi.sh 装 dhcpcd5）
if command -v dhcpcd >/dev/null 2>&1; then
    # dhcpcd may exit 0 while its background daemon still waits for carrier.
    # The exit status alone does not prove authentication or a DHCP lease.
    # -b backgrounds immediately: waiting for carrier/lease must not hold the
    # transition lock and delay AP recovery after an authentication failure.
    timeout -k 2 28 dhcpcd -b -4 -L -t 25 "$IFACE" 9>&- && log "dhcpcd launched (lease checked below)" || log "dhcpcd command FAILED"
elif command -v udhcpc >/dev/null 2>&1; then
    timeout -k 2 28 udhcpc -i "$IFACE" -q -t 25 -T 1 9>&- && log "udhcpc ok" || log "udhcpc FAILED"
elif command -v dhclient >/dev/null 2>&1; then
    timeout -k 2 28 dhclient -1 "$IFACE" 9>&- && log "dhclient ok" || log "dhclient FAILED"
else
    log "no dhcp client available (装 dhcpcd5)"
fi

ip -4 addr show dev "$IFACE" 2>/dev/null | grep -w inet || log "no IPv4 yet (稍后由 poll 检测)"
log "client done"
