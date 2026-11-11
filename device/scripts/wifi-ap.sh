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
CONF=${DUTYON_WIFI_RUN_DIR:-/run/dutyon}
AP_IP=192.168.4.1

log(){ echo "[wifi-ap] $*"; }
source "$(dirname "$0")/wifi-common.sh" || exit 1

# AP / client / off 共用锁。AP 自检不排队，避免慢扫描/取址期间反复起脚本。
# 所有后台守护进程启动时必须关闭 fd 9，否则它们会永久持有锁。
mkdir -p "$CONF"
exec 9>"$CONF/wifi.lock"
flock -n 9 || exit 0
mode_is_current ap || exit 0

# 仅复用本程序配置的 hostapd；即使 pid 文件丢失，也不能杀掉已有 AP。
# Unisoc WCN 在同一 boot 内重起 hostapd 可能报 Beacon set failed -12。
if ! daemon_alive hostapd; then
    for pid in $(pgrep -x hostapd); do
        if tr '\0' '\n' 2>/dev/null < "/proc/$pid/cmdline" | grep -Fxq "$CONF/hostapd.conf"; then
            echo "$pid" > "$CONF/hostapd.pid"
            break
        fi
    done
fi
reuse_ap=0
daemon_alive hostapd && reuse_ap=1

# 解除软/硬封锁（有些镜像出厂 rfkill 软关 Wi-Fi）
rfkill unblock wifi 2>/dev/null || true

# 等 wlan0 出现（最多 10s）：dutyon.service 已不等网络（提早启动抢占屏幕），
# 本脚本可能跑在 sprdwl_ng 驱动加载完成之前。
for i in $(seq 1 20); do
    ip link show "$IFACE" >/dev/null 2>&1 && break
    [ "$i" = 1 ] && log "waiting for $IFACE (driver loading...)"
    sleep 0.5
done
ip link show "$IFACE" >/dev/null 2>&1 || { log "missing $IFACE"; exit 1; }

# NetworkManager 若存在，别让它管这个 iface（会与 hostapd 抢占）
if command -v nmcli >/dev/null 2>&1; then nmcli dev set "$IFACE" managed no 2>/dev/null || true; fi

# 停 client 侧 + 旧 AP 实例（幂等，可重复调用）。旧 pid 文件一并删掉：
# C++ 侧自愈检查（hostapdAlive）读到已死旧 pid 会误重跑。
# 只停本接口的客户端，TERM 后限时升级 KILL；残留 DHCP 会抢走 AP 地址。
stop_clients

ensure_gateway() {
    # 保留运行中的 AP，不 down/flush 接口；只移除 client 遗留地址与默认路由。
    ip -4 -o addr show dev "$IFACE" | awk -v keep="$AP_IP/24" '$4 != keep {print $4}' \
      | while read -r a; do ip addr del "$a" dev "$IFACE"; done
    ip route del default dev "$IFACE" 2>/dev/null || true
    if ! ip -4 -o addr show dev "$IFACE" | awk '{print $4}' | grep -Fxq "$AP_IP/24"; then
        ip addr replace "$AP_IP/24" dev "$IFACE" || return 1
        gateway_changed=1
        log "restored $IFACE -> $AP_IP/24"
    fi
}

ensure_dnsmasq() {
    # bind-interfaces 在启动时固定监听地址。网关被抢走后，即使进程活着也
    # 需要重新绑定，不能仅看 pid 存活就跳过 DHCP/DNS 修复。
    if [ "$gateway_changed" = 1 ] && daemon_alive dnsmasq; then
        stop_owned dnsmasq
    fi
    if daemon_alive dnsmasq; then return 0; fi
    rm -f "$CONF/dnsmasq.pid"
    dnsmasq -C "$CONF/dnsmasq.conf" --pid-file="$CONF/dnsmasq.pid" 9>&- || {
        log "dnsmasq FAILED (check ports 53/67 and config)"; return 1;
    }
    daemon_alive dnsmasq || { log "dnsmasq not alive after start"; return 1; }
    log "dnsmasq ready"
}

gateway_changed=0
if [ "$reuse_ap" = 1 ]; then
    ensure_gateway && ensure_dnsmasq
    exit $?
fi
# 不干扰其他服务管理的热点。
if pgrep -x hostapd >/dev/null 2>&1; then
    log "another hostapd owns the radio; refusing to reset it"
    exit 1
fi
sleep 1

# 配静态网关地址（先 down/flush 再 up，确保干净）
ip link set "$IFACE" down 2>/dev/null || true
sleep 1
ip addr flush dev "$IFACE" 2>/dev/null || true
ip link set "$IFACE" up

# 趁 hostapd 启动前同步扫一次周边热点（单射频：AP 起后就扫不了），供
# captive portal 列表点选免手输 SSID。`iw scan` 本身同步阻塞到扫描完成，
# 输出即全量结果——直接落盘复用，无需额外 sleep 等待（旧版白等 4s）。
log "scanning nearby networks..."
DUMP="$CONF/scan-dump.txt"
timeout -k 1 8 iw dev "$IFACE" scan > "$DUMP" 2>/dev/null || log "scan unavailable, use manual SSID input"

# SSID 列表：按信号强->弱去重取前 8
awk '/signal:/{sig=$2} /SSID:/{s=substr($0,index($0,"SSID:")+6); if(s!="" && s!~/^</) printf "%s\t%s\n", sig, s}' "$DUMP" \
  | sort -t$'\t' -k1,1 -rn \
  | awk -F'\t' '!seen[$2]++ {print $2}' \
  | head -8 > "$CONF/scan.txt"
log "scan.txt networks: $(wc -l < "$CONF/scan.txt" 2>/dev/null)"

# 保留附近 SSID 扫描，但不再据扫描结果自动换 AP 信道。
# UWE5622 真机上信道 11 可见却无法与测试网卡完成关联，切回 6 即恢复；
# 固定使用已验证的 2.4GHz 信道 6，也避免同一 BSSID 重启后变更信道。
if [ -f "$CONF/hostapd.conf" ]; then
    sed -i 's/^channel=.*/channel=6/' "$CONF/hostapd.conf"
    log "channel fixed: 6 (compatibility)"
fi

ip addr replace "$AP_IP/24" dev "$IFACE" || exit 1
log "$IFACE -> $AP_IP/24"

# 起热点（-B 后台，pid 落盘便于 wifi-off.sh 清理 + C++ 侧自愈检查）
if [ -f "$CONF/hostapd.conf" ]; then
    if hostapd -B "$CONF/hostapd.conf" -P "$CONF/hostapd.pid" 9>&-; then
        log "hostapd started"
    else
        log "hostapd FAILED (查 hostapd.conf / 驱动是否支持 AP)"
        exit 1
    fi
else
    log "missing $CONF/hostapd.conf"
    exit 1
fi
sleep 1
# 存活回显：起完 1s 后确认进程还在（驱动 AP 模式/信道不支持会静默退出）
if daemon_alive hostapd; then
    log "hostapd alive (pid $(cat "$CONF/hostapd.pid"))"
else
    log "hostapd NOT alive after start (驱动不支持 AP/该信道？)"
    exit 1
fi

# 起 DHCP + DNS（dnsmasq.conf 里 interface+bind-interfaces 只绑 wlan0；
# 必须在静态地址配好之后启动，否则 bind 失败）
gateway_changed=1
ensure_dnsmasq || exit 1

log "AP done (SSID/pass 见 hostapd.conf；captive portal 由 dutyon-pet 监听 :80)"
