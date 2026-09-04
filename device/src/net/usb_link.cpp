#ifndef _WIN32  // 仅设备端（ARM Linux）

#include "net/usb_link.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "config.h"

namespace dutyon {

namespace {

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string readFileTrimmed(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::string s;
    in >> s;
    return s;
}

// 从 ARP 表发现 PC：点对点子网（设备 .1 + PC 一台）里唯一非自身的邻居即 PC。
// 不用 DHCP 租约文件：systemd-networkd 的 DHCPServer（服务端）不落盘租约，
// /run/systemd/netif/leases/ 只存客户端租约；ARP 在 DHCP/首包后必有记录。
std::string discoverPeerFromArp(const std::string& link_name) {
    std::ifstream arp("/proc/net/arp");
    std::string line;
    std::getline(arp, line);  // 表头
    while (std::getline(arp, line)) {
        char ip[64] = {}, dev[32] = {};
        int flags = 0;
        if (sscanf(line.c_str(), "%63s %*x %x %*s %*s %31s",
                   ip, &flags, dev) != 3)
            continue;
        if (flags == 0) continue;  // 未完成解析的条目（全 0 MAC）
        if (link_name != dev) continue;
        if (strncmp(ip, "169.254.", 8) == 0) continue;  // IPv4LL 兜底地址不算
        return ip;
    }
    return {};
}

} // namespace

std::optional<std::string> UsbLink::poll() {
    const long long now = nowMs();
    // 1s 节流：主循环 30fps 每帧调用，sysfs/租约文件读不必更频繁
    if (has_checked_ && now - last_check_ms_ < 1000) {
        return connected_ ? std::optional<std::string>(cached_url_) : std::nullopt;
    }
    has_checked_ = true;
    last_check_ms_ = now;

    const std::string net_dir = std::string("/sys/class/net/") + kUsbLinkName;

    // 1. 链路状态：NO-CARRIER/未插线时 operstate != up
    if (readFileTrimmed(net_dir + "/operstate") != "up") {
        if (connected_) printf("[UsbLink] %s link down\n", kUsbLinkName);
        connected_ = false;
        cached_url_.clear();
        return std::nullopt;
    }

    // 2. ARP 发现对端（PC 拿到 DHCP 地址后首次通信即入表）；
    //    未入表时主动 ping 子网广播促一下（后台无感，1s 节流）
    std::string pc_addr = discoverPeerFromArp(kUsbLinkName);
    if (pc_addr.empty()) {
        std::string cmd = "ping -c 1 -W 1 -I ";
        cmd += kUsbLinkName;
        cmd += " 192.168.7.255 >/dev/null 2>&1";
        (void)system(cmd.c_str());
        pc_addr = discoverPeerFromArp(kUsbLinkName);
    }
    if (pc_addr.empty()) {
        // 链路已通但 PC 尚未拿到 DHCP 地址（Windows 首次识别约 5-10s）
        if (connected_) printf("[UsbLink] lease lost, waiting for PC DHCP\n");
        connected_ = false;
        cached_url_.clear();
        return std::nullopt;
    }

    cached_url_ = "http://" + pc_addr + ":" + std::to_string(kApiPort);
    if (!connected_) printf("[UsbLink] PC discovered: %s\n", cached_url_.c_str());
    connected_ = true;
    return cached_url_;
}

} // namespace dutyon

#endif // !_WIN32
