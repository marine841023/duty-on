#pragma once

#include <string>
#include <optional>

namespace dutyon {

// 与桌面端 /api/status 返回的 Snapshot 结构对应
// （src-tauri/src/state_manager.rs，serde camelCase）
//
//   {
//     "overallState": "sleeping" | "working" | "alert",
//     "sessions": [ { "sessionId": ..., "status": "idle" | ... } ],
//     "lastEventAt": 1692...,
//     "timestamp": 1692...
//   }
struct PetStatus {
    std::string overall_state;  // "sleeping" | "working" | "alert"
    int session_count = 0;      // 活跃会话数（sessions 数组长度）
    bool has_confirmation = false; // 有会话处于 confirmation-needed
};

// 与桌面端 /api/metrics 返回的 MetricsSnapshot 对应
// （src-tauri/src/sys_monitor.rs，serde camelCase）
// GPU 字段在无 N 卡时服务端给 null，这里用 has_gpu 标记
struct SysMetrics {
    float cpu_usage = 0.0f;     // 0-100
    unsigned long long mem_total = 0;
    unsigned long long mem_used = 0;
    bool has_gpu = false;
    std::string gpu_name;
    float gpu_usage = 0.0f;
    unsigned long long vram_total = 0;
    unsigned long long vram_used = 0;
    unsigned long long net_rx_rate = 0;  // bytes/sec
    unsigned long long net_tx_rate = 0;  // bytes/sec
    float self_cpu = 0.0f;
    unsigned long long self_mem = 0;
};

class ApiClient {
public:
    explicit ApiClient(const std::string& base_url);
    ~ApiClient();

    // 轮询一次状态，失败返回 std::nullopt（网络错误容忍，主循环继续）
    std::optional<PetStatus> poll();

    // 轮询系统监控数据。首次调用会激活服务端采样线程，
    // 采样尚未就绪（503）或网络失败均返回 std::nullopt。
    std::optional<SysMetrics> pollMetrics();

private:
    std::string base_url_;
    struct Impl;
    Impl* impl_;
};

} // namespace dutyon
