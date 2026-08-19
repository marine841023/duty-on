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

class ApiClient {
public:
    explicit ApiClient(const std::string& base_url);
    ~ApiClient();

    // 轮询一次状态，失败返回 std::nullopt（网络错误容忍，主循环继续）
    std::optional<PetStatus> poll();

private:
    std::string base_url_;
    struct Impl;
    Impl* impl_;
};

} // namespace dutyon
