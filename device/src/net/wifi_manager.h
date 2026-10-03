#pragma once

// 设备端 Wi-Fi 管理器（仅 ARM Linux）：配网 + 模式切换状态机。
//
// 流程（Wi-Fi 配对码方案，替代 USB 配对）：
//   1. 开机无已保存家庭 Wi-Fi 凭据 -> AP 模式：hostapd + dnsmasq 起一个
//      可被搜到的热点（SSID DutyOn-<后4位>），设备 wlan0 = 192.168.4.1；
//      屏幕大字显示热点名/密码（配网引导，main.cpp WifiProvision）。
//   2. 手机连上热点后 captive portal 自动弹出（dnsmasq 把任意域名解析到
//      192.168.4.1，本类内嵌 httplib 监听 80，对系统探测路径回 302），
//      提交家庭 Wi-Fi SSID+密码。
//   3. 凭据持久化到 ~/.dutyon/wifi.json，切 client 模式（wpa_supplicant +
//      dhcp）加入家庭 Wi-Fi，与 PC 同局域网。
//   4. 已有凭据则开机直接 client；连不上家庭 Wi-Fi 时回退 AP 重新配网。
//   5. PC 端可下发 reset-wifi 指令（换路由器场景）：resetToAp() 清凭据
//      重进配网，配对关系保留。
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
    ApProvisioning,  // AP 模式，等待手机配网（屏幕配网引导）
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

    // AP 模式信息（供配网屏幕大字与提示文案）
    const std::string& apSsid() const { return ap_ssid_; }
    const std::string& apPass() const { return ap_pass_; }
    // captive portal 手动访问地址（自动弹窗失败时手机浏览器手动打开兜底）
    const std::string& portalUrl() const { return portal_url_; }

    // client 模式拿到 IP 后非空（Online 状态）
    std::string clientIp() const;

    // captive portal 提交的家庭 Wi-Fi 凭据 -> 持久化并请求切 client（portal
    // 线程调用；仅置标志，实际切换在 poll() 里做，避免跨线程动射频）
    void saveAndJoin(const std::string& ssid, const std::string& pass);

    // PC 端"重新配网"指令入口（api/client 收到 deviceCmd=reset-wifi 后经
    // main 调用）：清除已存家庭 Wi-Fi 凭据并重回 AP 配网模式（换路由器
    // 场景）。配对关系（token）不动：新网络入网后自动恢复连接
    void resetToAp();

    // 主循环每帧调用（内部 1s 节流）：驱动状态机（AP<->client 切换、
    // 等待 DHCP、client 掉线回退 AP、hostapd 存活自愈）。不阻塞：切换
    // 脚本后台起守护进程，连接与否由后续帧轮询 wlan0 是否有 IP 判定。
    void poll();

private:
    void enterAp();              // 起 AP + captive portal
    void enterClient();          // 停 AP、起 client
    void setState(WifiState s);  // 改状态并记时间戳
    bool clientConnected();      // wlan0 有 IP 且非 AP 地址
    bool hostapdAlive() const;   // hostapd 进程存活（pid 文件 + kill 0）
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
    long long last_ap_check_ms_ = 0; // hostapd 存活检查的上次时刻（AP 自愈）
    int join_attempts_ = 0;
    // AP 自愈连续失败计数：≥3 判定驱动 beacon 槽位泄漏（同 boot 内
    // hostapd 起过一次后必 ENOMEM，软件层无法复位）→ 自动重启设备
    int ap_revives_ = 0;
    // ARP 广播保活上次执行时刻（配网态每 5s 一次，见 poll()）
    long long last_arp_keepalive_ms_ = 0;
    std::string cached_ip_;          // client IP 缓存（poll 刷新）
};

} // namespace dutyon

#endif // !_WIN32
