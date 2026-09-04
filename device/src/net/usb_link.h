#pragma once

#include <optional>
#include <string>

namespace dutyon {

// USB 直连链路监视器（仅设备端）：轮询 usb0 链路状态 + ARP 邻居表，
// 自动发现 PC 地址，无需手工配 IP。
//
// 接线形态：设备以 USB 网卡 gadget（NCM，Windows 内置 usbnet 免驱）
// 出现，设备侧 usb0 = 192.168.7.1 并通过 networkd 内置 DHCP 给 PC 派发
// 192.168.7.100~199；点对点子网里唯一邻居即 PC（读 /proc/net/arp）。
//
// 用法：主循环每帧 poll()（内部 1s 节流）；返回 base URL 表示已连接，
// nullopt 表示未接线/未握手完成（应显示引导画面）。
class UsbLink {
public:
    // 距上次检测 >=1s 时重新检查链路+租约；返回当前 PC 的 base URL
    //（形如 http://192.168.7.100:17521）；未连接返回 nullopt
    std::optional<std::string> poll();

    bool connected() const { return connected_; }

private:
    bool connected_ = false;
    bool has_checked_ = false;
    long long last_check_ms_ = 0;
    std::string cached_url_;
};

} // namespace dutyon
