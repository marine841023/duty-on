#pragma once

// ---------------------------------------------------------------------------
// 配对管理器（Wi-Fi 配对码方案，仅 PC 端）—— 替代旧的 USB 直连信任模型。
//
// 握手流程（设备=客户端，PC=服务端）：
//   1. 设备入网后 UDP 广播 DUTYON_DISCOVER <device_id>（见 net/pc_discovery）。
//      PC 单播回 DUTYON_OFFER <api_port> <paired>，paired 查 isPaired()。
//   2. 未配对设备 POST /api/pair-request {deviceId, code}：handleRequest 记
//      pending 并回 {status:"pending"}；用户在 PC 菜单「设备→配对设备」输入
//      设备屏幕上的 6 位码，confirmPairing 比对 pending.code，匹配即签发
//      token 存 paired_ 并持久化到 ~/.dutyon/config.json 的 pairedDevices。
//   3. 设备下次 pair-request 命中 paired_ → 回 {status:"paired", token}，
//      设备持久化 token，此后带 X-DutyOn-Token 头轮询 /api/*。
//   4. token 门控（http_server pre-routing）：validateToken 失败一律 401。
//
// 线程安全：discovery 线程 / HTTP 处理线程 / UI 线程并发访问，全程持 mtx_。
// ---------------------------------------------------------------------------

#ifdef _WIN32

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dutyon::backend {

// 待配对请求（设备已 POST pair-request，等用户在 PC 输码确认）
struct PendingPair {
    std::string device_id;
    std::string code;         // 设备屏幕上显示的 6 位码（用户据此在 PC 输入）
    std::string ip;           // 请求来源 IP（UI 展示辅助识别）
    long long last_seen_ms = 0;  // steady_clock 毫秒（老化清理）
};

class PairingManager {
public:
    PairingManager();  // 从 config.json 载入已配对设备表

    // /api/pair-request 处理：已配对 → out_token 填其 token 并返回 "paired"
    //（设备据此拿回令牌）；否则记/刷新 pending 并返回 "pending"。
    std::string handleRequest(const std::string& device_id,
                              const std::string& code, const std::string& ip,
                              std::string* out_token);

    bool isPaired(const std::string& device_id) const;
    // token 是否属于任一已配对设备（token 门控用；空 token 恒 false）
    bool validateToken(const std::string& token) const;

    // ---- UI 面（菜单「设备→配对设备」）----
    std::vector<PendingPair> pendingList();   // 顺带清理超时 pending
    std::vector<std::string> pairedList() const;
    // 用户输入 code 确认配对：匹配某 pending 请求的 code 则签发 token、
    // 持久化、移出 pending，返回该 device_id；无匹配返回空串。
    std::string confirmByCode(const std::string& code);
    // 解除配对：移除 token 并持久化（设备下次轮询得 401 → 自动重新握手）
    bool unpair(const std::string& device_id);

    // ---- PC 端"重新配网"指令（换 WiFi 场景）----
    // 挂起指令（仅已配对设备）：目标设备轮询 /api/status 时经 deviceCmd
    // 字段下发，设备回执 cmd-ack 后清除。设备离线则一直挂起（持久化到
    // config.json，PC 重启不丢），设备上线后自然收到。
    bool requestResetWifi(const std::string& device_id);
    // token 所属设备若有挂起的 reset 指令则返回其 id（不消费，等 ack）
    std::string pendingResetWifiFor(const std::string& device_id) const;
    // 设备回执（/api/cmd-ack）：id 匹配挂起指令则清除并持久化
    void ackResetWifi(const std::string& device_id, const std::string& id);
    // token -> device_id（status 注入 deviceCmd 时定向目标用；无匹配返回空）
    std::string deviceByToken(const std::string& token) const;

private:
    void persistLocked();               // 写 paired_ 到 config.json（调用方持锁）
    void persistCmdLocked();            // 写挂起指令到 config.json（调用方持锁）
    static std::string generateToken(); // 随机 32 位十六进制令牌
    static std::string newCmdId();      // 指令序号（system_clock 毫秒字符串）

    mutable std::mutex mtx_;
    std::map<std::string, PendingPair> pending_;  // device_id -> 待配对请求
    std::map<std::string, std::string> paired_;   // device_id -> token
    // 挂起的"重新配网"指令（同一时刻至多一条：目标 + 序号）
    std::string cmd_device_id_;
    std::string cmd_id_;
};

} // namespace dutyon::backend

#endif // _WIN32
