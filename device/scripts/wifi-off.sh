#!/bin/bash
# DutyOn Wi-Fi —— 关闭。dutyon-pet 退出（WifiManager::stop）时调用，
# 停掉本程序起的所有 Wi-Fi 守护进程并清地址，把 wlan 交还系统。
# 用法： wifi-off.sh [iface]   默认 iface=wlan0
IFACE="${1:-wlan0}"
CONF=${DUTYON_WIFI_RUN_DIR:-/run/dutyon}
log(){ echo "[wifi-off] $*"; }
source "$(dirname "$0")/wifi-common.sh" || exit 1

mkdir -p "$CONF"
exec 9>"$CONF/wifi.lock"
flock -w 45 9 || { log "timed out waiting for Wi-Fi transition"; exit 1; }
mode_is_current off || exit 0

stop_owned hostapd
stop_owned dnsmasq
stop_clients
ip addr flush dev "$IFACE" 2>/dev/null || true

log "done"
