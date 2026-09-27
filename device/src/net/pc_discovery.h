#pragma once

// 设备端 PC 发现（UDP 广播；替代 USB 直连时代的 ARP 邻居表发现）。
// Wi-Fi 配对码方案里设备与 PC 同在家庭局域网，靠广播发现彼此：
//   设备周期广播 "DUTYON_DISCOVER <device_id>" -> 255.255.255.255:17522；
//   PC 后端收到后单播回 "DUTYON_OFFER <api_port> <paired>"；
//   设备据回包源 IP + api_port 组装 base url（http://<pc_ip>:<port>），
//   并记住 PC 报告的"是否已认得本设备"（paired）。
// 用法：主循环每帧 poll()（内部节流广播 + 非阻塞排空回包，绝不阻塞渲染）。
#ifndef _WIN32

#include <optional>
#include <string>

namespace dutyon {

class PcDiscovery {
public:
    explicit PcDiscovery(std::string device_id);
    ~PcDiscovery();

    // 返回最近有效（未超时）的 PC base url；未发现返回 nullopt。
    // paired_out 非空时写入 PC 报告的配对状态。
    std::optional<std::string> poll(bool* paired_out = nullptr);

private:
    std::string device_id_;
    int fd_ = -1;
    long long last_bcast_ms_ = 0;
    long long last_offer_ms_ = 0;
    std::string base_url_;
    bool paired_ = false;
};

} // namespace dutyon

#endif // !_WIN32
