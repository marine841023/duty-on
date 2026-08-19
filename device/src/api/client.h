#pragma once

#include <string>
#include <optional>

namespace dutyon {

// 与桌面端 /api/status 返回结构对应
struct PetStatus {
    std::string state;      // "idle" | "working" | "alert" | "sleeping"
    std::string state_text; // 人类可读状态文本
    int active_sessions = 0;
};

class ApiClient {
public:
    explicit ApiClient(const std::string& base_url);
    ~ApiClient();

    // 轮询一次状态，失败返回 std::nullopt（网络错误容忍）
    std::optional<PetStatus> poll();

private:
    std::string base_url_;
    // cpr session 在 cpp 中持有（PIMPL 避免头文件引入 cpr）
    struct Impl;
    Impl* impl_;
};

} // namespace dutyon
