#ifndef _WIN32  // 仅设备端（ARM Linux）

#include "net/pc_discovery.h"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"

namespace dutyon {

namespace {

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

const long long kBcastIntervalMs = 2000;  // 广播周期（PC 上线后最多 2s 被发现）
const long long kOfferTtlMs = 8000;       // 回包有效期（超时视为 PC 已离开）

} // namespace

PcDiscovery::PcDiscovery(std::string device_id) : device_id_(std::move(device_id)) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        fprintf(stderr, "[Discover] socket: %s\n", strerror(errno));
        return;
    }
    const int one = 1;
    if (setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one)) < 0)
        fprintf(stderr, "[Discover] SO_BROADCAST: %s\n", strerror(errno));

    // 绑 0.0.0.0 + 临时端口：同一 socket 既发广播也收 PC 单播回包
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = 0;  // 内核分配临时端口
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0)
        fprintf(stderr, "[Discover] bind: %s\n", strerror(errno));
}

PcDiscovery::~PcDiscovery() {
    if (fd_ >= 0) ::close(fd_);
}

std::optional<std::string> PcDiscovery::poll(bool* paired_out) {
    if (fd_ < 0) {
        if (paired_out) *paired_out = false;
        return std::nullopt;
    }
    const long long now = nowMs();

    // 周期广播发现包（携带 device_id 供 PC 判断是否已配对）
    if (now - last_bcast_ms_ >= kBcastIntervalMs) {
        last_bcast_ms_ = now;
        const std::string msg = "DUTYON_DISCOVER " + device_id_;
        sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_port = htons((uint16_t)kDiscoveryPort);
        dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        (void)::sendto(fd_, msg.data(), msg.size(), 0,
                        reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    }

    // 非阻塞排空回包（可能多台 PC 响应，取最后一条有效的）
    for (int i = 0; i < 8; ++i) {
        char buf[256] = {};
        sockaddr_in src{};
        socklen_t sl = sizeof(src);
        const ssize_t n = ::recvfrom(fd_, buf, sizeof(buf) - 1, MSG_DONTWAIT,
                                     reinterpret_cast<sockaddr*>(&src), &sl);
        if (n <= 0) break;  // EAGAIN（无更多数据）或错误
        buf[n] = '\0';
        int api_port = 0, paired = 0;
        if (sscanf(buf, "DUTYON_OFFER %d %d", &api_port, &paired) == 2 &&
            api_port > 0 && api_port < 65536) {
            char ip[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
            std::string url =
                std::string("http://") + ip + ":" + std::to_string(api_port);
            if (url != base_url_)
                printf("[Discover] PC offer: %s (paired=%d)\n", url.c_str(), paired);
            base_url_ = url;
            paired_ = (paired != 0);
            last_offer_ms_ = now;
        }
    }

    if (paired_out) *paired_out = paired_;
    const bool fresh = (last_offer_ms_ != 0) && (now - last_offer_ms_ < kOfferTtlMs);
    if (!fresh) return std::nullopt;
    return base_url_;
}

} // namespace dutyon

#endif // !_WIN32
