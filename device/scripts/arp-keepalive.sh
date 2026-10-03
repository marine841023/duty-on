#!/bin/bash
# ARP 保活：向广播地址发 2 个 ICMP，让刚关联的手机应答并学到网关 MAC，
# 消除"DHCP 完成但 ARP 未解析"窗口内访问 192.168.4.1 丢包的问题。
# 由 wifi_manager 在配网态每 5s 调用一次（spawnDetached）。
IFACE="${1:-wlan0}"
ping -I "$IFACE" -b -c 2 -W 1 192.168.4.255 >/dev/null 2>&1 || true
exit 0
