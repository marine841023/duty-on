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

# 【幂等短路】hostapd 已在跑就直接复用，绝不杀它——Unisoc WCN 驱动
# (sprdwl_ng) 的 AP 是 boot 周期一次性的：hostapd 退出后固件 beacon 槽位
# 不释放，同 boot 内第二次启动必报 "Beacon set failed -12 (ENOMEM)"
# （接口 del/type 切换/模块重载均被驱动拒绝，软件层无解）。
# SSID/密码由设备 MAC 固定派生、conf 不变，复用安全。
# （注意：本短路只跳过"起 AP"，上面的 client 残留清理仍需执行——见
#   rogue 分支：C++ 侧周期调 `wifi-ap.sh <iface> clean` 只清残留不动 AP）
if [ "$2" = "clean" ]; then
    # 只清 client 残留（wpa_supplicant client 抢接口会丢手机关联帧；
    # dhcpcd master 会把缓存的旧租约重新挂回 wlan0 产生双 IP），
    # 绝不碰 hostapd/dnsmasq
    pkill -9 -f "wpa_supplicant.*-i $IFACE" 2>/dev/null || true
    pkill -9 dhcpcd 2>/dev/null || true
    rm -f "$CONF/wpa_supplicant.pid"
    # dhcpcd 挂上来的多余 IP（旧租约/IPv4LL）也摘掉，只留 AP 网关地址
    ip -4 addr show dev "$IFACE" 2>/dev/null | grep -v "inet $AP_IP/" \
      | awk '/inet /{print $2}' | while read -r a; do
        ip addr del "$a" dev "$IFACE" 2>/dev/null || true
    done
    exit 0
fi
if pgrep -x hostapd >/dev/null 2>&1; then
    log "hostapd already running, reuse (driver AP is one-shot per boot)"
    # hostapd 在跑但 client 残留也必须清（fallback 后 wpa_supplicant/
    # dhcpcd 可能仍在抢占 wlan0）——上一版直接 exit 0 跳过了清理，导致
    # 热点广播正常但手机连不上
    pkill -9 -f "wpa_supplicant.*-i $IFACE" 2>/dev/null || true
    pkill -9 dhcpcd 2>/dev/null || true
    exit 0
fi

# 解除软/硬封锁（有些镜像出厂 rfkill 软关 Wi-Fi）
rfkill unblock wifi 2>/dev/null || true
rfkill unblock all  2>/dev/null || true

# 等 wlan0 出现（最多 10s）：dutyon.service 已不等网络（提早启动抢占屏幕），
# 本脚本可能跑在 sprdwl_ng 驱动加载完成之前。放在 clean/复用短路之后，
# 保证周期性 clean 调用与 hostapd 复用路径仍然快进快出
for i in $(seq 1 20); do
    ip link show "$IFACE" >/dev/null 2>&1 && break
    [ "$i" = 1 ] && log "waiting for $IFACE (driver loading...)"
    sleep 0.5
done

# NetworkManager 若存在，别让它管这个 iface（会与 hostapd 抢占）
if command -v nmcli >/dev/null 2>&1; then nmcli dev set "$IFACE" managed no 2>/dev/null || true; fi

# 停 client 侧 + 旧 AP 实例（幂等，可重复调用）。旧 pid 文件一并删掉：
# C++ 侧自愈检查（hostapdAlive）读到已死旧 pid 会误重跑。
# dhcpcd 必须用 pkill -9：`dhcpcd -k` 实测杀不死 master（control socket
# 通信失败时静默无效），残留的 master 会在 link-up 时把缓存的旧租约
# （如路由器网段 IP）重新挂回 wlan0 → AP 网关双 IP、路由错乱。
# wpa_supplicant 用 -9 同理：nl80211 ioctl 阻塞中可能不响应 TERM。
pkill -9 -f "wpa_supplicant.*-i $IFACE" 2>/dev/null || true
pkill -9 dhcpcd 2>/dev/null || true
pkill -x udhcpc  2>/dev/null || true
pkill -x hostapd 2>/dev/null || true
pkill -x dnsmasq 2>/dev/null || true
rm -f "$CONF/hostapd.pid" "$CONF/dnsmasq.pid"
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
iw dev "$IFACE" scan > "$DUMP" 2>/dev/null

# SSID 列表：按信号强->弱去重取前 8
awk '/signal:/{sig=$2} /SSID:/{s=substr($0,index($0,"SSID:")+6); if(s!="" && s!~/^</) printf "%s\t%s\n", sig, s}' "$DUMP" \
  | sort -t$'\t' -k1,1 -rn \
  | awk -F'\t' '!seen[$2]++ {print $2}' \
  | head -8 > "$CONF/scan.txt"
log "scan.txt networks: $(wc -l < "$CONF/scan.txt" 2>/dev/null)"

# 信道自适应：统计 2.4G 互不重叠主信道 1/6/11（freq 2412/2437/2462）上的
# BSS 数，选最空闲的写回 hostapd.conf（每个 BSS 只数第一个 2.4G freq）。
# 扫描失败/无结果保持 C++ 侧写入的默认 channel=6 不动。
NBSS=$(grep -c '^BSS ' "$DUMP" 2>/dev/null || true)
if [ "${NBSS:-0}" -gt 0 ] && [ -f "$CONF/hostapd.conf" ]; then
    CH=$(awk '
      /^BSS /{have=0}
      /freq:/{ if(have==0 && $2>=2412 && $2<=2472){have=1; c=int(($2-2407)/5);
               if(c<=3)ch=1; else if(c<=8)ch=6; else ch=11; cnt[ch]++} }
      END{ best=1; n=1e9; split("1 6 11",ks," ");
           for(i=1;i<=3;i++){k=ks[i]; v=(k in cnt)?cnt[k]:0; if(v<n){n=v;best=k}}
           print best }' "$DUMP")
    if [ -n "$CH" ]; then
        sed -i "s/^channel=.*/channel=$CH/" "$CONF/hostapd.conf"
        log "channel auto: $CH (least congested of 1/6/11)"
    fi
fi

ip addr add "$AP_IP/24" dev "$IFACE" 2>/dev/null || true
log "$IFACE -> $AP_IP/24"

# 起热点（-B 后台，pid 落盘便于 wifi-off.sh 清理 + C++ 侧自愈检查）
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
# 存活回显：起完 1s 后确认进程还在（驱动 AP 模式/信道不支持会静默退出）
if [ -f "$CONF/hostapd.pid" ] && kill -0 "$(cat "$CONF/hostapd.pid")" 2>/dev/null; then
    log "hostapd alive (pid $(cat "$CONF/hostapd.pid"))"
else
    log "hostapd NOT alive after start (驱动不支持 AP/该信道？)"
fi

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
