#!/bin/bash
# DutyOn Wi-Fi —— 关闭。dutyon-pet 退出（WifiManager::stop）时调用，
# 停掉本程序起的所有 Wi-Fi 守护进程并清地址，把 wlan 交还系统。
# 用法： wifi-off.sh [iface]   默认 iface=wlan0
IFACE="${1:-wlan0}"
log(){ echo "[wifi-off] $*"; }

pkill -x hostapd 2>/dev/null || true
pkill -x dnsmasq 2>/dev/null || true
pkill -f "wpa_supplicant.*$IFACE" 2>/dev/null || true
dhcpcd -k "$IFACE" 2>/dev/null || true
pkill -x udhcpc 2>/dev/null || true
ip addr flush dev "$IFACE" 2>/dev/null || true

log "done"
