#pragma once

// 设备端 Wi-Fi 管理器（仅 ARM Linux）：配网 + 模式切换状态机。
//
// 流程（Wi-Fi 配对码方案，替代 USB 配对）：
//   1. 开机无已保存家庭 Wi-Fi 凭据 -> AP 模式：hostapd + dnsmasq 起一个
//      可被搜到的热点（SSID DutyOn-<后4位>），设备 wlan0 = 192.168.4.1；
//      屏幕显示加入该热点的 QR（见 render/qr_banner）。
//   2. 手机扫 QR 加入热点后访问 captive portal（dnsmasq 把任意域名解析到
//      192.168.4.1，本类内嵌 httplib 监听 80）提交家庭 Wi-Fi SSID+密码。
//   3. 凭据持久化到 ~/.dutyon/wifi.json，切 client 模式（wpa_supplicant +
//      dhcp）加入家庭 Wi-Fi，与 PC 同局域网。
//   4. 已有凭据则开机直接 client；连不上家庭 Wi-Fi 时回退 AP 重新配网。
//
// 射频编排（hostapd/dnsmasq/wpa_supplicant/dhcpcd 的启停与 wlan0 地址配置）
// 集中在随包脚本 /opt/dutyon/wifi-{ap,client,off}.sh，本类只写配置并调用，
// 便于真机调整而不重编 C++。单射频不能并发 AP+client，模式切换串行。
#ifndef _WIN32

#include <atomic>
#include <mutex>
#include <string>

namespace dutyon {

enum class WifiState {
    Off,             // 未初始化/射频不可用
    ApProvisioning,  // AP 模式，等待手机配网（屏幕显示 QR）
    Joining,         // 正在加入家庭 Wi-Fi
    Online,          // 已加入家庭 Wi-Fi（有 IP，可发现 PC / 显示配对码）
    Failed,          // client 连接失败（将回退 AP）
};

const char* wifiStateStr(WifiState s);

class WifiManager {
public:
    WifiManager();
    ~WifiManager();

    // 读取已保存凭据、决定初始模式（无凭据 -> AP 配网；有凭据 -> client）。
    // 返回 false 表示无 wlan 设备/脚本缺失（保持 Off，调用方按无 Wi-Fi 处理）
    bool start();
    void stop();

    WifiState state() const { return state_.load(); }
    bool hasSavedCreds() const { return has_creds_; }

    // AP 模式信息（供 QR 与提示文案）
    const std::string& apSsid() const { return ap_ssid_; }
    const std::string& apPass() const { return ap_pass_; }

    // client 模式拿到 IP 后非空（Online 状态）
    std::string clientIp() const;

    // captive portal 提交的家庭 Wi-Fi 凭据 -> 持久化并请求切 client（portal
    // 线程调用；仅置标志，实际切换在 poll() 里做，避免跨线程动射频）
    void saveAndJoin(const std::string& ssid, const std::string& pass);

    // 主循环每帧调用（内部 1s 节流）：驱动状态机（AP<->client 切换、
    // 等待 DHCP、client 掉线回退 AP）。不阻塞：切换脚本后台起守护进程，
    // 连接与否由后续帧轮询 wlan0 是否有 IP 判定。
    void poll();

private:
    void enterAp();              // 起 AP + captive portal
    void enterClient();          // 停 AP、起 client
    void setState(WifiState s);  // 改状态并记时间戳
    bool clientConnected();      // wlan0 有 IP 且非 AP 地址
    void loadCreds();            // 读 ~/.dutyon/wifi.json
    void saveCreds();            // 写 ~/.dutyon/wifi.json
    void startPortal();          // 启动 httplib captive portal（80）
    void stopPortal();

    std::atomic<bool> run_{false};
    std::atomic<WifiState> state_{WifiState::Off};
    std::atomic<bool> pending_join_{false};  // portal 提交凭据后置位

    std::string ap_ssid_;
    std::string ap_pass_;
    std::string portal_url_ = "http://192.168.4.1";
    std::string wlan_ = "wlan0";

    mutable std::mutex cred_mu_;
    bool has_creds_ = false;
    std::string home_ssid_;
    std::string home_pass_;

    struct PortalImpl;
    PortalImpl* portal_ = nullptr;

    long long last_poll_ms_ = 0;
    long long state_since_ms_ = 0;   // 进入当前状态的时刻（超时回退用）
    int join_attempts_ = 0;
    std::string cached_ip_;          // client IP 缓存（poll 刷新）
};

} // namespace dutyon

#endif // !_WIN32
