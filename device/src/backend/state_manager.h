#pragma once

// ---------------------------------------------------------------------------
// 跨 IDE 实例的 AI 会话状态机 —— 从 src-tauri/src/state_manager.rs 1:1 移植。
//
// 会话生命周期（SessionStatus = Idle + 4 个活跃变体）：
//   SessionStart                       -> Idle
//   UserPromptSubmit                   -> Thinking
//   PreToolUse（非 ask-user）          -> ToolUse
//   PostToolUse（非 ask-user）         -> Thinking（工具结果 -> 继续思考）
//   PreToolUse(AskUserQuestion)        -> ConfirmationNeeded；其 PostToolUse -> Thinking
//   Notification（确认类）             -> ConfirmationNeeded
//   Notification（完成类）/ Stop       -> Idle
//   CLI 代理（Codex/OpenCode）崩溃     -> 由 syncCliLiveness 清除
//
// 宠物总状态（三档）：
//   - 任一会话 confirmation-needed -> alert
//   - 否则任一会话 working 系      -> working（Thinking/ToolUse/Working）
//   - 否则                          -> sleeping
//
// 线程模型：所有公开方法内部加锁，HTTP 线程 / 扫描线程 / 主循环线程可
// 并发调用。version() 每次有效变更单调递增，供 SSE（条件变量等待）与
// 本机 UI（seq 防重复消费）共享。
// ---------------------------------------------------------------------------

#include <cstdint>
#include <condition_variable>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/client.h"          // PetStatus / SessionInfo（UI 消费结构）
#include "backend/ide_kind.h"

namespace dutyon::backend {

enum class PetState { Sleeping, Working, Alert };
enum class SessionStatus { Idle, Working, ConfirmationNeeded, Thinking, ToolUse };

inline const char* petStateStr(PetState s) {
    switch (s) {
        case PetState::Sleeping: return "sleeping";
        case PetState::Working: return "working";
        case PetState::Alert: return "alert";
    }
    return "sleeping";
}

inline const char* sessionStatusStr(SessionStatus s) {
    switch (s) {
        case SessionStatus::Idle: return "idle";
        case SessionStatus::Working: return "working";
        case SessionStatus::ConfirmationNeeded: return "confirmation-needed";
        case SessionStatus::Thinking: return "thinking";
        case SessionStatus::ToolUse: return "tool-use";
    }
    return "idle";
}

// IDE 桥接脚本 POST /hook 上报的事件（snake_case 字段，与 1.x/2.0 协议一致）
struct HookEvent {
    std::string session_id;
    std::string hook_event_name;
    std::string project_path;
    std::string project_name;
    std::string cwd;
    std::optional<std::string> notification_type;
    std::optional<std::string> tool_name;
    std::optional<std::string> tool_use_id;  // Trae 原生 hook 带；配对 Pre/Post
    std::optional<IdeKind> ide;
    std::optional<std::string> message;
    std::optional<uint64_t> timestamp;

    // 从 POST body JSON 解析（snake_case；非法输入返回 nullopt）
    static std::optional<HookEvent> fromJson(const nlohmann::json& j);
};

// 窗口扫描结果（ide_scanner 产出）
struct DetectedProject {
    std::string name;
    IdeKind ide;
};

// CLI 代理进程存活（Codex/OpenCode 无 GUI 窗口，靠进程表判活）
struct CliLiveness {
    bool codex_alive = false;
    bool opencode_alive = false;
};

class StateManager {
public:
    StateManager();

    // ---- 事件驱动（HTTP /hook 线程调用）----
    void handleHookEvent(const HookEvent& ev);
    void removeSession(const std::string& session_id);
    void resetAllToIdle();

    // ---- 定时器（30s 清理 / 60s alert 重提醒）----
    void cleanupStaleSessions();
    void checkAndRemindAlert();

    // ---- 扫描线程同步 ----
    void syncDetectedWindows(const std::vector<DetectedProject>& detected);
    void syncCliLiveness(const CliLiveness& lv);
    size_t sessionCount();

    // ---- 消费侧 ----
    // 快照版本号：每次有效状态变更 +1（SSE 与本机 UI 用它做脏检查）
    uint64_t version();
    // 阻塞等待 version 超过 last，最多 ms 毫秒；返回 true = 有新版本
    bool waitVersion(uint64_t last, int timeout_ms);
    // 完整 Snapshot JSON（camelCase，含 sessionId/projectPath/lastEvent 等全字段）
    nlohmann::json snapshotJson();
    // 本机 UI 用的精简视图（按 (project, ide) 去重、字母序），结构与旧
    // /api/status 响应解析结果一致
    dutyon::PetStatus petStatus();

private:
    struct Session {
        std::string session_id;
        std::string project_path;
        std::string project_name;
        std::optional<IdeKind> ide;
        SessionStatus status = SessionStatus::Idle;
        std::string last_event;
        uint64_t last_event_time = 0;
        std::optional<std::string> alert_message;
        // 执行中的工具（PreToolUse 记账的 tool_use_id，PostToolUse 销账）。
        // 非空 = 工具正在跑，静默是预期的（见 bc::kToolRunningTimeout）——
        // 覆盖主代理长命令与子代理长任务（子代理工具调用同 session 发
        // hook，agent_id 不同；Task 工具本身不发 hook）
        std::string pending_tool;
    };

    // 快照里的单个会话（对外形态）
    struct SessionSnapshot {
        std::string session_id;
        std::string project_name;
        std::string project_path;
        std::optional<IdeKind> ide;
        SessionStatus status;
        std::string last_event;
        std::optional<std::string> alert_message;
    };

    // ---- 以下方法要求调用方已持锁 ----
    void handleHookEventLocked(const HookEvent& ev);
    void recomputeStateLocked();
    std::string snapshotSignatureLocked() const;
    std::vector<SessionSnapshot> dedupSessionsLocked() const;
    uint64_t nowMs() const;

    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::map<std::string, Session> sessions_;  // key = session_id
    PetState overall_state_ = PetState::Sleeping;
    uint64_t last_alert_time_ = 0;
    std::optional<std::string> last_signature_;
    uint64_t last_event_at_ = 0;
    uint64_t version_ = 0;
    std::unordered_map<std::string, uint32_t> window_miss_counts_;
    std::unordered_map<std::string, uint32_t> cli_miss_counts_;
};

// 当前毫秒时间戳（对齐 JS Date.now()）
uint64_t currentMillis();

} // namespace dutyon::backend
