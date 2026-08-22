// 会话状态机 —— src-tauri/src/state_manager.rs 的 C++ 移植（逻辑 1:1）。

#include "backend/state_manager.h"

#include <algorithm>
#include <chrono>
#include <regex>
#include <set>

#include "backend/backend_config.h"

namespace dutyon::backend {

namespace {

constexpr const char* kWindowPrefix = "__window:";
// 真实 hook 会话连续未在窗口扫描中出现这么多次后才删除（窗口标题随版本/
// 语言变化，启动时窗口可能短暂消失，单次 miss 不能杀掉活会话）。
constexpr uint32_t kWindowMissGrace = 3;
// Codex/OpenCode 会话连续未在进程表中出现这么多次后删除（进程枚举可能
// 瞬时漏掉进程，单次 miss 不能杀）。
constexpr uint32_t kCliMissGrace = 2;

bool containsStr(const char* const* arr, int count, const std::string& v) {
    for (int i = 0; i < count; i++)
        if (v == arr[i]) return true;
    return false;
}

// ASCII 小写（关键词里的中文不受影响；Rust to_lowercase 对这些纯 ASCII
// 关键词等价）
std::string asciiLower(const std::string& s) {
    std::string out = s;
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = (char)(c + ('a' - 'A'));
    return out;
}

// Notification 消息里的 shell 命令模式：AI 请求授权运行命令时，消息里通常
// 带命令名（PowerShell Verb-Noun 形如 "Remove-Item"，或 .exe 调用）。
// 对应 Rust: (?i)\b[A-Z][a-z]+-[A-Z]\w+\b|\b\w+\.exe\b
bool matchesCmdPattern(const std::string& msg) {
    static const std::regex pattern(R"(\b[A-Z][a-z]+-[A-Z]\w+\b|\b\w+\.exe\b)",
                                    std::regex::icase);
    return std::regex_search(msg, pattern);
}

uint8_t dedupPriority(SessionStatus s) {
    switch (s) {
        case SessionStatus::ConfirmationNeeded: return 3;
        case SessionStatus::Working:
        case SessionStatus::Thinking:
        case SessionStatus::ToolUse: return 2;
        case SessionStatus::Idle: return 1;
    }
    return 1;
}

} // namespace

uint64_t currentMillis() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---------------------------------------------------------------------------
// HookEvent 解析
// ---------------------------------------------------------------------------
std::optional<HookEvent> HookEvent::fromJson(const nlohmann::json& j) {
    if (!j.is_object()) return std::nullopt;
    HookEvent ev;
    auto read_str = [&](const char* key) -> std::string {
        auto it = j.find(key);
        return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
    };
    // 必填字段（Rust serde 无 default 的两个字段）
    ev.session_id = read_str("session_id");
    ev.hook_event_name = read_str("hook_event_name");
    if (ev.session_id.empty() || ev.hook_event_name.empty()) return std::nullopt;
    ev.project_path = read_str("project_path");
    ev.project_name = read_str("project_name");
    ev.cwd = read_str("cwd");
    ev.notification_type = read_str("notification_type");
    ev.tool_name = read_str("tool_name");
    ev.tool_use_id = read_str("tool_use_id");
    if (auto it = j.find("ide"); it != j.end() && it->is_string())
        ev.ide = ideKindFromStr(it->get<std::string>());
    ev.message = read_str("message");
    if (auto it = j.find("timestamp"); it != j.end() && it->is_number_unsigned())
        ev.timestamp = it->get<uint64_t>();
    return ev;
}

// ---------------------------------------------------------------------------
// StateManager
// ---------------------------------------------------------------------------
StateManager::StateManager() = default;

uint64_t StateManager::nowMs() const { return currentMillis(); }

void StateManager::handleHookEvent(const HookEvent& ev) {
    std::lock_guard<std::mutex> lk(mtx_);
    handleHookEventLocked(ev);
}

void StateManager::handleHookEventLocked(const HookEvent& ev) {
    const uint64_t now = nowMs();

    // 取出或创建会话
    auto it = sessions_.find(ev.session_id);
    if (it == sessions_.end()) {
        Session s;
        s.session_id = ev.session_id;
        s.project_path = !ev.project_path.empty() ? ev.project_path : ev.cwd;
        s.project_name = !ev.project_name.empty()
                             ? ev.project_name
                             : [](const std::string& path) {
                                   if (path.empty()) return std::string("Unknown");
                                   std::string norm;
                                   for (char c : path) norm += (c == '\\') ? '/' : c;
                                   std::string last;
                                   size_t pos = 0;
                                   while (pos <= norm.size()) {
                                       size_t next = norm.find('/', pos);
                                       std::string seg = norm.substr(
                                           pos, next == std::string::npos ? std::string::npos
                                                                          : next - pos);
                                       if (!seg.empty()) last = seg;
                                       if (next == std::string::npos) break;
                                       pos = next + 1;
                                   }
                                   return last.empty() ? std::string("Unknown") : last;
                               }(s.project_path);
        s.ide = ev.ide;
        s.status = SessionStatus::Idle;
        s.last_event = ev.hook_event_name;
        s.last_event_time = now;
        it = sessions_.emplace(ev.session_id, std::move(s)).first;
    }
    Session& session = it->second;

    // 更新会话信息（后到事件缺字段时保留旧的非空值，避免会话丢标签）
    if (!ev.project_name.empty()) session.project_name = ev.project_name;
    if (!ev.project_path.empty()) session.project_path = ev.project_path;
    if (ev.ide.has_value()) session.ide = ev.ide;
    session.last_event = ev.hook_event_name;
    session.last_event_time = now;
    last_event_at_ = now;

    const bool ask_user_tool =
        ev.tool_name.has_value() &&
        containsStr(bc::kAskUserTools, bc::kAskUserToolsCount, *ev.tool_name);
    const bool confirm_tool =
        ev.tool_name.has_value() &&
        containsStr(bc::kConfirmTools, bc::kConfirmToolsCount, *ev.tool_name);
    auto alert_msg = [&]() {
        if (ev.tool_name.has_value()) return "需要确认: " + *ev.tool_name;
        if (ev.message.has_value()) return *ev.message;
        return std::string("需要你的确认");
    };

    const std::string& name = ev.hook_event_name;
    // 工具执行记账：PreToolUse 记 pending（工具开始跑）、PostToolUse 销账
    // （结束）。非 pending 期间静默才可能是真空闲。Pre/Post 之间完全无
    // 事件是常态（长构建/长命令/子代理长任务），降级超时见
    // bc::kToolRunningTimeout。SessionStart/UserPromptSubmit/Stop 到来说明
    // 回合推进，兜底清账。Notification 例外：异步事件、不推进工具生命
    // 周期（permission_prompt 到达时工具仍在等待批准 = 仍 in-flight），
    // 不得清账 —— 否则合法的待确认通知会被乱序防护误判为过期。
    if (name == "PreToolUse") {
        session.pending_tool =
            ev.tool_use_id.has_value() && !ev.tool_use_id->empty()
                ? *ev.tool_use_id
                : (ev.tool_name.has_value() ? *ev.tool_name : "unknown");
    } else if (name == "Notification") {
        // 不动 pending_tool
    } else if (name == "PostToolUse") {
        session.pending_tool.clear();
    } else {
        session.pending_tool.clear();
    }
    if (name == "SessionStart") {
        session.status = SessionStatus::Idle;
        session.alert_message.reset();
    } else if (name == "UserPromptSubmit") {
        session.status = SessionStatus::Thinking;
        session.alert_message.reset();
    } else if (name == "PreToolUse") {
        if (ask_user_tool || confirm_tool) {
            // 代理被阻塞等待用户回答/点击"运行"（Qoder 无 Notification 事件；
            // computer-use 卡片出现即等待）
            session.status = SessionStatus::ConfirmationNeeded;
            session.alert_message = alert_msg();
        } else {
            session.status = SessionStatus::ToolUse;
            session.alert_message.reset();
        }
    } else if (name == "PostToolUse") {
        if (ask_user_tool) {
            // 用户已作答，代理继续思考
            session.status = SessionStatus::Thinking;
            session.alert_message.reset();
        } else if (session.status == SessionStatus::ToolUse ||
                   session.status == SessionStatus::Working ||
                   session.status == SessionStatus::Thinking ||
                   session.status == SessionStatus::ConfirmationNeeded) {
            // 工具完成 —— 包括 ConfirmationNeeded：工具完成意味着用户已经
            // 批准（或回答了挂起的 ask），保持 alert 会永远响
            session.status = SessionStatus::Thinking;
            session.alert_message.reset();
        }
        // Idle（如手动中止后的 Stop）时迟到的 PostToolUse 不得复活会话
    } else if (name == "Notification") {
        // check_confirmation_needed 三级判定
        bool needs_confirmation = false;
        bool stale_ignore = false;  // 过期乱序通知：保持现状态，只当没来过
        {
            // 1. 显式类型分类（最高优先级）
            bool classified = false;
            if (ev.notification_type.has_value()) {
                const std::string& nt = *ev.notification_type;
                if (containsStr(bc::kNotificationCompleteTypes,
                                bc::kNotificationCompleteTypesCount, nt)) {
                    needs_confirmation = false;
                    classified = true;
                } else if (containsStr(bc::kNotificationConfirmTypes,
                                       bc::kNotificationConfirmTypesCount, nt)) {
                    needs_confirmation = true;
                    classified = true;
                    // 过期乱序防护：Notification 是异步事件（官方文档：不阻塞
                    // 主流程），permission_prompt 类可迟于对应工具的
                    // PostToolUse 送达 —— 此时 pending_tool 已清空，若仍置
                    // alert 会永久卡死（ConfirmationNeeded 豁免所有超时）。
                    // permission_prompt 定义上绑定 in-flight 工具（带
                    // tool_use_id），无 pending 即过期，忽略。
                    // document_review / browser_interaction / ask_user_question
                    // 是 UI 流程等待，不一定有 in-flight 工具，不设此防护。
                    if (session.pending_tool.empty() &&
                        containsStr(bc::kNotificationToolBoundConfirmTypes,
                                    bc::kNotificationToolBoundConfirmTypesCount, nt)) {
                        stale_ignore = true;
                    }
                }
            }
            // 2. 会话状态上下文
            if (!classified && session.status == SessionStatus::ConfirmationNeeded) {
                needs_confirmation = false;
                if (ev.message.has_value()) {
                    const std::string lower = asciiLower(*ev.message);
                    for (int i = 0; i < bc::kNotificationConfirmKeywordsCount; i++)
                        if (lower.find(asciiLower(bc::kNotificationConfirmKeywords[i])) !=
                            std::string::npos) {
                            needs_confirmation = true;
                            break;
                        }
                }
                classified = true;  // confirmation-needed + 歧义 = 完成通知
            }
            // 3. 非 confirmation 状态（idle/working）：tool_name 暗示待授权
            if (!classified) {
                if (ev.tool_name.has_value()) {
                    needs_confirmation = true;
                } else if (ev.message.has_value()) {
                    const std::string& msg = *ev.message;
                    const std::string lower = asciiLower(msg);
                    for (int i = 0; i < bc::kNotificationConfirmKeywordsCount; i++) {
                        if (lower.find(asciiLower(bc::kNotificationConfirmKeywords[i])) !=
                            std::string::npos) {
                            needs_confirmation = true;
                            break;
                        }
                    }
                    if (!needs_confirmation && matchesCmdPattern(msg))
                        needs_confirmation = true;
                }
                if (!needs_confirmation) needs_confirmation = bc::kAlertOnAmbiguousNotification;
            }
        }
        if (stale_ignore) {
            // 过期乱序通知：保持现状态（见上方防护注释）
        } else if (needs_confirmation) {
            session.status = SessionStatus::ConfirmationNeeded;
            session.alert_message = alert_msg();
        } else {
            // 任务完成通知 -> 直接 Idle
            session.status = SessionStatus::Idle;
            session.alert_message.reset();
        }
    } else if (name == "PermissionRequest") {
        // 代理请求工具权限 —— 需要用户输入
        session.status = SessionStatus::ConfirmationNeeded;
        session.alert_message = alert_msg();
    } else if (name == "Stop") {
        session.status = SessionStatus::Idle;
        session.alert_message.reset();
    }

    // 真实 hook 会话已覆盖该项目；删除窗口扫描建立的占位会话避免列表重复
    if (!session.project_name.empty()) {
        sessions_.erase(kWindowPrefix + session.project_name);
    }

    recomputeStateLocked();
}

void StateManager::resetAllToIdle() {
    std::lock_guard<std::mutex> lk(mtx_);
    const uint64_t now = nowMs();
    for (auto& [id, s] : sessions_) {
        s.status = SessionStatus::Idle;
        s.alert_message.reset();
        s.last_event_time = now;
    }
    recomputeStateLocked();
}

void StateManager::removeSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (sessions_.erase(session_id) > 0) {
        last_signature_.reset();  // 强制下次发送 update
        recomputeStateLocked();
    }
}

void StateManager::recomputeStateLocked() {
    PetState new_state = PetState::Sleeping;
    for (const auto& [id, s] : sessions_) {
        if (s.status == SessionStatus::ConfirmationNeeded) {
            new_state = PetState::Alert;
            break;
        }
        if (s.status == SessionStatus::Working || s.status == SessionStatus::Thinking ||
            s.status == SessionStatus::ToolUse) {
            new_state = PetState::Working;
        }
    }

    const PetState old_state = overall_state_;
    overall_state_ = new_state;

    // 脏检查：仅"有意义的快照"变化时才算新版本（不含 lastEventTime/timestamp）
    const std::string signature = snapshotSignatureLocked();
    const bool sig_changed = !last_signature_.has_value() || *last_signature_ != signature;
    if (sig_changed) {
        last_signature_ = signature;
        version_++;
        cv_.notify_all();
    }
    if (old_state != new_state && new_state == PetState::Alert) {
        last_alert_time_ = nowMs();
        version_++;  // 对应 Rust 的 alert_tx.send
        cv_.notify_all();
    }
    // 重提醒由 checkAndRemindAlert() 独立定时器驱动
}

std::string StateManager::snapshotSignatureLocked() const {
    std::vector<std::string> parts;
    parts.reserve(sessions_.size());
    for (const auto& [id, s] : sessions_) {
        std::string p = s.session_id;
        p += "|" + s.project_name;
        p += "|" + s.project_path;
        p += '|';
        p += (s.ide.has_value() ? ideKindStr(*s.ide) : "");
        p += "|" + std::string(sessionStatusStr(s.status));
        p += "|" + s.last_event;
        p += "|" + s.alert_message.value_or("");
        parts.push_back(std::move(p));
    }
    std::sort(parts.begin(), parts.end());
    std::string sig = std::string(petStateStr(overall_state_)) + "::";
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) sig += ";;";
        sig += parts[i];
    }
    return sig;
}

std::vector<StateManager::SessionSnapshot> StateManager::dedupSessionsLocked() const {
    // 内部全量快照
    std::vector<SessionSnapshot> sessions;
    sessions.reserve(sessions_.size());
    for (const auto& [id, s] : sessions_) {
        sessions.push_back(SessionSnapshot{id, s.project_name, s.project_path, s.ide,
                                           s.status, s.last_event, s.alert_message});
    }

    // 按 (project_name, ide) 折叠同项目多会话（如 Qoder 专家/子代理流），
    // 优先级 ConfirmationNeeded(3) > Working 系(2) > Idle(1)；同优先级时
    // 真实 hook 会话胜过 __window: 占位会话。仅影响展示快照，内部按
    // session_id 的跟踪 / 超时清理 / 窗口同步不受影响。
    std::map<std::pair<std::string, std::string>, size_t> group_index;  // key -> 下标
    std::vector<SessionSnapshot> out;
    for (auto& s : sessions) {
        const std::string ide_key = s.ide.has_value() ? ideKindStr(*s.ide) : "";
        const auto key = std::make_pair(s.project_name, ide_key);
        auto it = group_index.find(key);
        if (it == group_index.end()) {
            group_index[key] = out.size();
            out.push_back(std::move(s));
        } else {
            SessionSnapshot& existing = out[it->second];
            const bool existing_placeholder =
                existing.session_id.rfind(kWindowPrefix, 0) == 0;
            const bool new_placeholder = s.session_id.rfind(kWindowPrefix, 0) == 0;
            const bool replace =
                dedupPriority(s.status) > dedupPriority(existing.status) ||
                (dedupPriority(s.status) == dedupPriority(existing.status) &&
                 existing_placeholder && !new_placeholder);
            if (replace) existing = std::move(s);
        }
    }

    // 按项目名稳定字母序（大小写不敏感）；曾按状态优先级排序导致会话每次
    // 变化都重排，用户期望项目位置不动
    std::sort(out.begin(), out.end(), [](const SessionSnapshot& a, const SessionSnapshot& b) {
        return asciiLower(a.project_name) < asciiLower(b.project_name);
    });
    return out;
}

nlohmann::json StateManager::snapshotJson() {
    std::lock_guard<std::mutex> lk(mtx_);
    auto sessions = dedupSessionsLocked();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : sessions) {
        arr.push_back({
            {"sessionId", s.session_id},
            {"projectName", s.project_name},
            {"projectPath", s.project_path},
            {"ide", s.ide.has_value() ? nlohmann::json(ideKindStr(*s.ide)) : nlohmann::json()},
            {"status", sessionStatusStr(s.status)},
            {"lastEvent", s.last_event},
            {"alertMessage", s.alert_message.has_value() ? nlohmann::json(*s.alert_message)
                                                         : nlohmann::json()},
        });
    }
    return {
        {"overallState", petStateStr(overall_state_)},
        {"sessions", arr},
        {"lastEventAt", last_event_at_},
        {"timestamp", nowMs()},
    };
}

dutyon::PetStatus StateManager::petStatus() {
    std::lock_guard<std::mutex> lk(mtx_);
    auto sessions = dedupSessionsLocked();
    dutyon::PetStatus ps;
    ps.overall_state = petStateStr(overall_state_);
    for (const auto& s : sessions) {
        dutyon::SessionInfo si;
        si.project_name = s.project_name;
        si.status = sessionStatusStr(s.status);
        si.ide = s.ide.has_value() ? ideKindStr(*s.ide) : "";
        si.alert_message = s.alert_message.value_or("");
        if (s.status == SessionStatus::ConfirmationNeeded) ps.has_confirmation = true;
        ps.sessions.push_back(std::move(si));
    }
    ps.session_count = (int)ps.sessions.size();
    return ps;
}

void StateManager::cleanupStaleSessions() {
    std::lock_guard<std::mutex> lk(mtx_);
    const uint64_t now = nowMs();
    bool changed = false;

    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for (const auto& [id, s] : sessions_) ids.push_back(id);

    for (const auto& id : ids) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) continue;
        // 时钟回拨（NTP/手动改时间）不能让无符号减法下溢
        const uint64_t elapsed = now >= it->second.last_event_time
                                     ? now - it->second.last_event_time
                                     : 0;
        // 等待用户确认（alert）的会话豁免所有超时：ask-user 对话框打开期间
        // 不会有 hook 事件，静默是预期的 —— 宠物必须持续提醒直到用户作答。
        // Windows 上窗口真正关闭时由 syncDetectedWindows 删除。
        if (it->second.status == SessionStatus::ConfirmationNeeded) continue;
        // 工具执行中：降级/删除超时放宽（见 bc::kToolRunningTimeout）
        const uint64_t limit = !it->second.pending_tool.empty()
                                   ? bc::kToolRunningTimeout
                                   : bc::kSessionTimeout;
        if (elapsed > limit) {
            sessions_.erase(it);
            changed = true;
            continue;
        }
        // 长静默后才降 Idle：Qoder ask-user 对话框 / 工具执行期都会让会话
        // 合法地静默（后者可达十几分钟，放宽判定）
        if ((it->second.status == SessionStatus::Working ||
             it->second.status == SessionStatus::Thinking ||
             it->second.status == SessionStatus::ToolUse) &&
            elapsed > (!it->second.pending_tool.empty()
                           ? bc::kToolRunningTimeout
                           : bc::kWorkingTimeout)) {
            it->second.status = SessionStatus::Idle;
            changed = true;
        }
    }
    if (changed) recomputeStateLocked();
}

void StateManager::checkAndRemindAlert() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (overall_state_ != PetState::Alert) return;
    const uint64_t now = nowMs();
    const uint64_t since = now >= last_alert_time_ ? now - last_alert_time_ : 0;
    if (since >= bc::kAlertReminder) {
        last_alert_time_ = now;
        last_signature_.reset();  // 使 update 也重新发出
        version_++;
        cv_.notify_all();
    }
}

size_t StateManager::sessionCount() {
    std::lock_guard<std::mutex> lk(mtx_);
    return sessions_.size();
}

void StateManager::syncDetectedWindows(const std::vector<DetectedProject>& detected) {
    std::lock_guard<std::mutex> lk(mtx_);

    // 删除前先快照 hook 会话的项目名（避免为已有 hook 会话的项目建占位）
    std::set<std::string> hook_project_names;
    for (const auto& [id, s] : sessions_)
        if (id.rfind(kWindowPrefix, 0) != 0) hook_project_names.insert(s.project_name);

    std::set<std::string> detected_names;
    std::set<int> detected_ides;
    for (const auto& d : detected) {
        detected_names.insert(d.name);
        detected_ides.insert((int)d.ide);
    }
    bool changed = false;

    // Windows 上 EnumWindows 可靠 —— 空列表就是"没有 IDE 窗口"，照常删除
    {
        std::vector<std::string> ids;
        ids.reserve(sessions_.size());
        for (const auto& [id, s] : sessions_) ids.push_back(id);
        for (const auto& id : ids) {
            auto it = sessions_.find(id);
            if (it == sessions_.end()) continue;
            const std::string pname = it->second.project_name;
            if (detected_names.count(pname)) {
                window_miss_counts_.erase(id);  // 窗口又可见了，重置宽限计数
                continue;
            }
            // Codex/OpenCode 是 CLI/TUI，没有 GUI 窗口可扫 —— 跳过窗口级
            // 清理，交给 SESSION_TIMEOUT / 进程存活同步
            const std::optional<IdeKind> session_ide = it->second.ide;
            if (session_ide == IdeKind::Codex || session_ide == IdeKind::OpenCode) {
                window_miss_counts_.erase(id);
                continue;
            }
            if (id.rfind(kWindowPrefix, 0) == 0) {
                // 占位会话 1:1 跟随窗口 —— 立即删除
                sessions_.erase(it);
                changed = true;
            } else {
                // 同 IDE 类型的窗口仍在（只是标题格式变了，如 Qoder 显示
                // "安装 - Qoder" 设置页）—— IDE 还开着，抑制本次 miss
                if (session_ide.has_value() && detected_ides.count((int)*session_ide))
                    continue;
                // 真实会话给宽限期：窗口可能短暂不可见（启动/重载）或标题
                // 格式不匹配；只有连续 miss 才删，SESSION_TIMEOUT 兜底
                if (++window_miss_counts_[id] >= kWindowMissGrace) {
                    sessions_.erase(it);
                    window_miss_counts_.erase(id);
                    changed = true;
                }
            }
        }
    }

    // 为新检测到且无 hook 会话的项目建占位会话
    const uint64_t now = nowMs();
    for (const auto& dp : detected) {
        if (hook_project_names.count(dp.name)) continue;
        const std::string wid = kWindowPrefix + dp.name;
        auto it = sessions_.find(wid);
        if (it != sessions_.end()) {
            it->second.last_event_time = now;  // 窗口开着就续命
            // 项目可能换了个 IDE 打开
            if (it->second.ide != dp.ide) {
                it->second.ide = dp.ide;
                changed = true;
            }
        } else {
            Session s;
            s.session_id = wid;
            s.project_path.clear();
            s.project_name = dp.name;
            s.ide = dp.ide;
            s.status = SessionStatus::Idle;
            s.last_event = "WindowDetected";
            s.last_event_time = now;
            sessions_[wid] = std::move(s);
            changed = true;
        }
    }

    // 为桥接没上报 ide 的 hook 会话回填（窗口扫描是事实来源）
    for (auto& [id, s] : sessions_) {
        if (id.rfind(kWindowPrefix, 0) == 0 || s.ide.has_value()) continue;
        for (const auto& dp : detected) {
            if (dp.name == s.project_name) {
                s.ide = dp.ide;
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        last_signature_.reset();  // 强制 update 发出
        recomputeStateLocked();
    }
}

void StateManager::syncCliLiveness(const CliLiveness& lv) {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> ids;
    ids.reserve(sessions_.size());
    for (const auto& [id, s] : sessions_) ids.push_back(id);
    bool changed = false;
    for (const auto& id : ids) {
        auto it = sessions_.find(id);
        if (it == sessions_.end()) continue;
        const std::optional<IdeKind> ide = it->second.ide;
        if (!ide.has_value()) continue;
        // 只有 CLI 代理受进程存活治理；GUI IDE 由 syncDetectedWindows 处理
        if (*ide != IdeKind::Codex && *ide != IdeKind::OpenCode) continue;
        const bool alive = (*ide == IdeKind::Codex) ? lv.codex_alive : lv.opencode_alive;
        if (alive) {
            cli_miss_counts_.erase(id);
            // CLI 进程在跑但空闲时续命：Stop 后用户可能读几分钟回复而不触发
            // 新 hook 事件 —— 不续命会被 SESSION_TIMEOUT 误删（静默更新
            // last_event_time，前端"最后事件"显示保持真实）
            it->second.last_event_time = nowMs();
        } else {
            if (++cli_miss_counts_[id] >= kCliMissGrace) {
                sessions_.erase(it);
                cli_miss_counts_.erase(id);
                changed = true;
            }
        }
    }
    if (changed) {
        last_signature_.reset();
        recomputeStateLocked();
    }
}

uint64_t StateManager::version() {
    std::lock_guard<std::mutex> lk(mtx_);
    return version_;
}

bool StateManager::waitVersion(uint64_t last, int timeout_ms) {
    std::unique_lock<std::mutex> lk(mtx_);
    return cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [&] { return version_ != last; });
}

} // namespace dutyon::backend
