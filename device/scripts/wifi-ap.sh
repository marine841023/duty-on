#!/bin/bash
# DutyOn Wi-Fi —— AP（配网热点）模式。
#
# 由 dutyon-pet（net/wifi_manager.cpp）在无已保存家庭 Wi-Fi 凭据时调用：
# 先把配置写到 /run/dutyon/{hostapd,dnsmasq}.conf，再 setsid 后台执行本脚本。
# 职责：把 wlan 配成 192.168.4.1 静态网关 + 起 hostapd（热点）+ dnsmasq
#（DHCP 派址 & 把任意域名劫持到 192.168.4.1，触发手机 captive portal 弹配网页）。
#
# 用法： wifi-ap.sh [iface]      默认 iface=wlan0
# 真机若板载 Wi-Fi 不支持 AP（hostapd 报驱动错误），需外接 USB Wi-Fi 或换驱动，
# 调整点集中在本脚本（无需重编 C++）。
IFACE="${1:-wlan0}"
CONF=/run/dutyon
AP_IP=192.168.4.1

log(){ echo "[wifi-ap] $*"; }

# 解除软/硬封锁（有些镜像出厂 rfkill 软关 Wi-Fi）
rfkill unblock wifi 2>/dev/null || true
rfkill unblock all  2>/dev/null || true

# NetworkManager 若存在，别让它管这个 iface（会与 hostapd 抢占）
if command -v nmcli >/dev/null 2>&1; then nmcli dev set "$IFACE" managed no 2>/dev/null || true; fi

# 停 client 侧 + 旧 AP 实例（幂等，可重复调用）
pkill -f "wpa_supplicant.*$IFACE" 2>/dev/null || true
dhcpcd -k "$IFACE" 2>/dev/null || true
pkill -x udhcpc  2>/dev/null || true
pkill -x hostapd 2>/dev/null || true
pkill -x dnsmasq 2>/dev/null || true
sleep 1

# 配静态网关地址（先 down/flush 再 up，确保干净）
ip link set "$IFACE" down 2>/dev/null || true
sleep 1
ip addr flush dev "$IFACE" 2>/dev/null || true
ip link set "$IFACE" up

# 趁 hostapd 启动前同步扫一次周边热点写 scan.txt（单射频：AP 起后就扫不了），
# 供 captive portal 列表点选、免手输 SSID。按信号强->弱去重取前 8；
# ~4s 延迟可接受（QR/portal 不依赖扫描结果，扫不到时 portal 退化为手动输入）。
log "scanning nearby networks..."
iw dev "$IFACE" scan >/dev/null 2>&1
sleep 4
iw dev "$IFACE" scan dump 2>/dev/null \
  | awk '/signal:/{sig=$2} /SSID:/{s=substr($0,index($0,"SSID:")+6); if(s!="" && s!~/^</) printf "%s\t%s\n", sig, s}' \
  | sort -t$'\t' -k1,1 -rn \
  | awk -F'\t' '!seen[$2]++ {print $2}' \
  | head -8 > "$CONF/scan.txt"
log "scan.txt networks: $(wc -l < "$CONF/scan.txt" 2>/dev/null)"

ip addr add "$AP_IP/24" dev "$IFACE" 2>/dev/null || true
log "$IFACE -> $AP_IP/24"

# 起热点（-B 后台，pid 落盘便于 wifi-off.sh 清理）
if [ -f "$CONF/hostapd.conf" ]; then
    if hostapd -B "$CONF/hostapd.conf" -P "$CONF/hostapd.pid"; then
        log "hostapd started"
    else
        log "hostapd FAILED (查 hostapd.conf / 驱动是否支持 AP)"
    fi
else
    log "missing $CONF/hostapd.conf"
fi
sleep 1

# 起 DHCP + DNS（dnsmasq.conf 里 interface+bind-interfaces 只绑 wlan0；
# 必须在静态地址配好之后启动，否则 bind 失败）
if [ -f "$CONF/dnsmasq.conf" ]; then
    if dnsmasq -C "$CONF/dnsmasq.conf" --pid-file="$CONF/dnsmasq.pid"; then
        log "dnsmasq started"
    else
        log "dnsmasq FAILED (53/67 端口被占？systemd-resolved?)"
    fi
else
    log "missing $CONF/dnsmasq.conf"
fi

log "AP done (SSID/pass 见 hostapd.conf；captive portal 由 dutyon-pet 监听 :80)"
