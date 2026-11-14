// Hook 状态机：按会话、代理、工具调用及确认请求独立跟踪并聚合。

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
// 目录名是随机 ID（Qoder 无工作区对话的临时目录形如 Documents/Qoder/<日期>/
// b8f041b1）：纯十六进制的 8/16/32 位串或完整 UUID。这种名字对人没有意义。
bool looksLikeRandomId(const std::string& name) {
    const size_t n = name.size();
    if (n != 8 && n != 16 && n != 32 && n != 36) return false;
    bool digit = false;
    for (size_t i = 0; i < n; ++i) {
        const char c = name[i];
        const bool dash = n == 36 && (i == 8 || i == 13 || i == 18 || i == 23);
        if (dash ? c != '-' : !isxdigit((unsigned char)c)) return false;
        if (c >= '0' && c <= '9') digit = true;
    }
    return digit;
}

const char* ideDisplayName(IdeKind k) {
    switch (k) {
        case IdeKind::Trae: return "Trae";
        case IdeKind::Qoder: return "Qoder";
        case IdeKind::Cursor: return "Cursor";
        case IdeKind::Codex: return "Codex";
        case IdeKind::OpenCode: return "OpenCode";
        case IdeKind::Claude: return "Claude";
    }
    return "AI";
}

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
    const auto& name = ev.hook_event_name;
    if (!containsStr(bc::kHookEvents, bc::kHookEventsCount, name) &&
        !containsStr(bc::kQoderHookEvents, bc::kQoderHookEventsCount, name) &&
        !containsStr(bc::kCodexHookEvents, bc::kCodexHookEventsCount, name) &&
        !containsStr(bc::kClaudeHookEvents, bc::kClaudeHookEventsCount, name) &&
        name != "PermissionResolved") return std::nullopt;
    ev.project_path = read_str("project_path");
    ev.project_name = read_str("project_name");
    ev.cwd = read_str("cwd");
    auto read_optional = [&](const char* key) -> std::optional<std::string> {
        auto value = read_str(key);
        return value.empty() ? std::nullopt : std::make_optional(value);
    };
    ev.notification_type = read_optional("notification_type");
    ev.tool_name = read_optional("tool_name");
    ev.tool_use_id = read_optional("tool_use_id");
    if (auto it = j.find("ide"); it != j.end() && it->is_string())
        ev.ide = ideKindFromStr(it->get<std::string>());
    ev.message = read_optional("message");
    ev.source = read_optional("source");
    ev.turn_id = read_optional("turn_id");
    ev.agent_id = read_optional("agent_id");
    ev.parent_session_id = read_optional("parent_session_id");
    ev.request_id = read_optional("request_id");
    if (auto it = j.find("timestamp"); it != j.end() && it->is_number_unsigned())
        ev.timestamp = it->get<uint64_t>();
    return ev;
}

// ---------------------------------------------------------------------------
// StateManager
// ---------------------------------------------------------------------------
StateManager::StateManager(std::function<uint64_t()> clock) : clock_(std::move(clock)) {}

uint64_t StateManager::nowMs() const { return clock_(); }

void StateManager::handleHookEvent(const HookEvent& ev) {
    std::lock_guard<std::mutex> lk(mtx_);
    handleHookEventLocked(ev);
}

void StateManager::handleHookEventLocked(const HookEvent& ev) {
    const uint64_t now = nowMs();

    const std::string& name = ev.hook_event_name;
    const std::string sid = ev.parent_session_id.value_or(ev.session_id);
    const bool child_session = sid != ev.session_id;
    // Separate actors inside one IDE conversation. OpenCode children have their
    // own session IDs; Trae/Qoder children share a session and carry agent_id.
    const bool child_lifecycle = name == "SubagentStart" || name == "SubagentStop";
    std::string actor_id = child_session ? ev.session_id : ev.agent_id.value_or("");
    // Cursor currently supplies subagent_id on Start but not on Stop. Observe
    // these as conversation activity instead of creating an unfinishable actor.
    if (child_lifecycle && ev.ide == IdeKind::Cursor) actor_id.clear();
    if (name == "SessionEnd" && !child_session && !ev.agent_id) {
        sessions_.erase(sid);
        window_miss_counts_.erase(sid);
        cli_miss_counts_.erase(sid);
        ended_sessions_[sid] = now;
        last_event_at_ = now;
        recomputeStateLocked();
        return;
    }
    const bool restart = name == "UserPromptSubmit" ||
                         (name == "SessionStart" && ev.source != "compact");
    if (restart && !child_session && !child_lifecycle) ended_sessions_.erase(sid);
    else if (ended_sessions_.count(sid)) return;

    auto [it, inserted] = sessions_.try_emplace(sid);
    Session& session = it->second;
    if (inserted) {
        session.session_id = sid;
        session.project_name = "Unknown";
    }
    if (!child_session && !child_lifecycle && restart && ev.agent_id &&
        !session.root_agent_id) session.root_agent_id = ev.agent_id;
    if (!child_session && ev.agent_id == session.root_agent_id) actor_id.clear();
    if (name == "SessionEnd" && actor_id.empty()) {
        sessions_.erase(sid);
        window_miss_counts_.erase(sid);
        cli_miss_counts_.erase(sid);
        ended_sessions_[sid] = now;
        last_event_at_ = now;
        recomputeStateLocked();
        return;
    }
    if (restart && actor_id.empty()) session.interrupted = false;
    else if (session.interrupted) return;
    // Anonymous subagent lifecycles are deliberately not counted: without an
    // identity a duplicate Start or out-of-order Stop cannot be paired safely.
    // They still refresh Thinking; identified children get independent ledgers.
    Actor& actor = session.actors[actor_id];
    const bool codex = ev.ide.value_or(session.ide.value_or(IdeKind::Trae)) == IdeKind::Codex;
    if (codex && !restart && !child_lifecycle && ev.turn_id && actor.turn_id &&
        ev.turn_id != actor.turn_id) return;
    // Claude Code runs hooks synchronously and in order, so a tool call after
    // Stop is not a late event: background agents, scheduled wake-ups and
    // asyncRewake hooks start a new turn without a UserPromptSubmit.
    const bool claude = ev.ide.value_or(session.ide.value_or(IdeKind::Trae)) == IdeKind::Claude;
    if (claude && actor.finished && name == "PreToolUse") actor = Actor{};
    if (actor.finished && !restart &&
        (actor.confirmations.empty() || (name != "PermissionResolved" && name != "PostToolUseFailure" &&
                                        name != "Interrupt" && name != "SessionEnd"))) return;
    if (ev.turn_id && !child_lifecycle) actor.turn_id = ev.turn_id;

    if (!ev.project_name.empty()) session.project_name = ev.project_name;
    if (!ev.project_path.empty()) session.project_path = ev.project_path;
    else if (!ev.cwd.empty()) session.project_path = ev.cwd;
    if (session.project_name == "Unknown" && !session.project_path.empty()) {
        auto path = session.project_path;
        std::replace(path.begin(), path.end(), '\\', '/');
        while (!path.empty() && path.back() == '/') path.pop_back();
        session.project_name = path.substr(path.find_last_of('/') + 1);
    }
    if (ev.ide) session.ide = ev.ide;
    // 随机 ID 目录不是项目：显示成"<工具> 对话"（去重仍按完整路径，互不合并）
    if (looksLikeRandomId(session.project_name))
        session.project_name = std::string(ideDisplayName(session.ide.value_or(IdeKind::Trae))) + " 对话";
    session.last_event = name;
    session.last_event_time = now;
    last_event_at_ = now;

    const bool identified_tool = ev.tool_use_id.has_value();
    const std::string tool_key = identified_tool ? "id:" + *ev.tool_use_id
                                               : "name:" + ev.tool_name.value_or("unknown");
    // Hosts may report either the short name or a namespace-qualified name.
    std::string short_tool = ev.tool_name.value_or("");
    auto separator = short_tool.find_last_of('.');
    if (separator != std::string::npos) short_tool.erase(0, separator + 1);
    separator = short_tool.rfind("__");
    if (separator != std::string::npos) short_tool.erase(0, separator + 2);
    const bool ask_user_tool = containsStr(bc::kAskUserTools, bc::kAskUserToolsCount, short_tool);
    const bool async_question = short_tool == "request_user_input_async";
    // A browser/computer tool is not itself evidence of a permission prompt.
    // Retain this legacy fallback only for Trae/Qoder, which expose such cards.
    const auto ide = session.ide.value_or(IdeKind::Trae);
    const bool confirm_tool = (ide == IdeKind::Trae || ide == IdeKind::Qoder) && ev.tool_name &&
        containsStr(bc::kConfirmTools, bc::kConfirmToolsCount, *ev.tool_name);
    auto add_confirmation = [&]() {
        if (ev.request_id && actor.resolved_requests.count(*ev.request_id)) return;
        // Claude Code reports one prompt up to three times (ask-user PreToolUse,
        // PermissionRequest, permission_prompt Notification) and not all carry
        // tool_use_id. Do not count the same dialog as a new request.
        if (claude && !ev.tool_use_id && !ev.request_id) {
            if (name == "Notification" && !actor.confirmations.empty()) return;
            if (ev.tool_name)
                for (const auto& [key, c] : actor.confirmations)
                    if (c.message == "需要确认: " + *ev.tool_name) return;
        }
        std::optional<std::string> bound_tool;
        if (ev.tool_use_id || ev.tool_name) bound_tool = tool_key;
        else if (!ev.request_id && actor.pending_tools.size() == 1)
            bound_tool = *actor.pending_tools.begin();
        const std::string key = ev.request_id ? "request:" + *ev.request_id :
            bound_tool.value_or("notification:" + ev.notification_type.value_or("unknown"));
        const std::string message = ev.tool_name ? "需要确认: " + *ev.tool_name :
            ev.message.value_or("需要你的确认");
        if (actor.confirmations.emplace(key, Confirmation{bound_tool, message, async_question}).second) {
            ++alert_seq_;
            last_alert_time_ = now;
            // 新请求重新武装提醒：即使上一轮已因无响应被抑制，也重新给两轮
            alert_rounds_ = 1;
            alert_suppressed_ = false;
        }
    };
    // Claude prompts without a call ID are bound by tool name or not at all;
    // the tool finishing is the only answer signal Claude Code sends.
    auto claude_unpaired = [&](const Confirmation& c) {
        return claude && (!c.tool_key || (ev.tool_name && *c.tool_key == "name:" + *ev.tool_name));
    };
    auto finish_tool = [&]() {
        auto pending = actor.pending_tools.find(tool_key);
        if (pending != actor.pending_tools.end()) actor.pending_tools.erase(pending);
        if (identified_tool) {
            // Bound history used only for duplicate/late events, never as work.
            if (actor.completed_tools.size() >= 512) actor.completed_tools.erase(actor.completed_tools.begin());
            actor.completed_tools.insert(tool_key);
        }
        for (auto c = actor.confirmations.begin(); c != actor.confirmations.end();) {
            if ((c->second.tool_key == tool_key || claude_unpaired(c->second)) &&
                (!c->second.asynchronous || name == "PostToolUseFailure")) c = actor.confirmations.erase(c);
            else ++c;
        }
    };
    if (restart) {
        actor = Actor{};
        actor.turn_id = ev.turn_id;
        actor.status = name == "UserPromptSubmit" ? SessionStatus::Thinking : SessionStatus::Idle;
    } else if (name == "PreToolUse") {
        if (!(identified_tool && actor.completed_tools.count(tool_key))) {
            if (!identified_tool || !actor.pending_tools.count(tool_key)) actor.pending_tools.insert(tool_key);
            if (ask_user_tool || confirm_tool) add_confirmation();
            actor.status = SessionStatus::ToolUse;
        }
    } else if (name == "PostToolUse" || name == "PostToolUseFailure") {
        finish_tool();
        actor.status = SessionStatus::Thinking;
    } else if (name == "PermissionRequest" || name == "Notification") {
        bool needs_confirmation = name == "PermissionRequest";
        bool complete = false;
        if (ev.notification_type) {
            needs_confirmation = needs_confirmation || containsStr(bc::kNotificationConfirmTypes,
                bc::kNotificationConfirmTypesCount, *ev.notification_type);
            complete = containsStr(bc::kNotificationCompleteTypes,
                bc::kNotificationCompleteTypesCount, *ev.notification_type);
        }
        if (!needs_confirmation && !complete) {
            needs_confirmation = ev.tool_name.has_value();
            if (ev.message) {
                const auto lower = asciiLower(*ev.message);
                for (int i = 0; i < bc::kNotificationConfirmKeywordsCount; ++i)
                    if (lower.find(asciiLower(bc::kNotificationConfirmKeywords[i])) != std::string::npos)
                        needs_confirmation = true;
                if (matchesCmdPattern(*ev.message)) needs_confirmation = true;
            }
        }
        // Permission may precede PreToolUse. Only positive evidence of that
        // exact call completing makes it stale; an empty ledger is not evidence.
        if (needs_confirmation && !(identified_tool && actor.completed_tools.count(tool_key))) {
            add_confirmation();
        } else if (complete && claude && actor_id.empty() &&
                   ev.notification_type == std::string("idle_prompt")) {
            // Claude Code: the prompt has sat idle for a minute, so the main
            // turn is over. This is the only signal after an Esc interrupt or a
            // dismissed permission dialog, neither of which fires Stop.
            actor = Actor{};
            actor.finished = true;
        } else if (complete) {
            // Completion notices must not clear a concurrent permission request.
            if (actor.pending_tools.empty() && actor.confirmations.empty()) actor.status = SessionStatus::Idle;
        }
        // Ambiguous notifications are informational and never terminate a turn.
    } else if (name == "PermissionResolved") {
        if (ev.request_id) {
            actor.confirmations.erase("request:" + *ev.request_id);
            if (actor.resolved_requests.size() >= 512) actor.resolved_requests.erase(actor.resolved_requests.begin());
            actor.resolved_requests.insert(*ev.request_id);
        }
        else if (ev.tool_use_id || ev.tool_name) {
            for (auto c = actor.confirmations.begin(); c != actor.confirmations.end();) {
                if (c->second.tool_key == tool_key) c = actor.confirmations.erase(c);
                else ++c;
            }
        }
        actor.status = SessionStatus::Thinking;
    } else if (name == "Stop" || name == "StopFailure" || name == "Interrupt" || name == "SessionEnd" ||
               (name == "SubagentStop" && !actor_id.empty())) {
        // Ending an assistant turn does not answer an asynchronous question.
        // Explicit cancellation/session end still clears the entire actor.
        auto questions = std::move(actor.confirmations);
        actor = Actor{};
        if (name == "Stop") {
            for (auto& [key, confirmation] : questions)
                if (confirmation.asynchronous) actor.confirmations.emplace(key, std::move(confirmation));
            actor.turn_id = ev.turn_id;
        }
        actor.finished = true;
        // Interrupt of the main conversation cancels its whole actor tree.
        if (name == "Interrupt" && actor_id.empty()) {
            session.interrupted = true;
            for (auto& [id, child] : session.actors) { child = Actor{}; child.finished = true; }
        }
    } else if (name == "SubagentStart" || name == "SubagentStop" ||
               name == "PreCompact" || name == "PostCompact" ||
               name == "PreModelRequest" || name == "PostModelRequest" ||
               (name == "SessionStart" && ev.source == "compact")) {
        actor.status = SessionStatus::Thinking;
        actor.finished = false;
    }

    if (actor.finished) actor.status = SessionStatus::Idle;

    // Derive the conversation from all actors. A sibling event cannot clear
    // another actor's pending tool or confirmation, including a parent Stop.
    session.status = SessionStatus::Idle;
    session.alert_message.reset();
    for (const auto& [id, a] : session.actors) {
        auto state = !a.confirmations.empty() ? SessionStatus::ConfirmationNeeded :
                     !a.pending_tools.empty() ? SessionStatus::ToolUse : a.status;
        if (dedupPriority(state) > dedupPriority(session.status) ||
            (state == SessionStatus::ToolUse && session.status == SessionStatus::Thinking))
            session.status = state;
        if (!a.confirmations.empty() && !session.alert_message)
            session.alert_message = a.confirmations.begin()->second.message;
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
        s.actors.clear();
        s.interrupted = false;
        s.last_event_time = now;
    }
    recomputeStateLocked();
}

void StateManager::removeSession(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(mtx_);
    window_miss_counts_.erase(session_id);
    cli_miss_counts_.erase(session_id);
    if (sessions_.erase(session_id) > 0) {
        last_signature_.reset();  // 强制下次发送 update
        recomputeStateLocked();
    }
}

void StateManager::recomputeStateLocked() {
    bool has_conf = false;
    bool any_working = false;
    for (const auto& [id, s] : sessions_) {
        if (s.status == SessionStatus::ConfirmationNeeded) has_conf = true;
        if (s.status == SessionStatus::Working || s.status == SessionStatus::Thinking ||
            s.status == SessionStatus::ToolUse) {
            any_working = true;
        }
    }
    has_confirmation_ = has_conf;
    // 确认全部清掉 = 本轮 alert 结束，重置轮数与抑制
    if (!has_conf) {
        alert_rounds_ = 0;
        alert_suppressed_ = false;
    }
    // 待确认优先 alert；但提醒达上限仍无响应时抑制 alert，回落 working
    PetState new_state = PetState::Sleeping;
    if (has_conf) new_state = alert_suppressed_ ? PetState::Working : PetState::Alert;
    else if (any_working) new_state = PetState::Working;

    const PetState old_state = overall_state_;
    overall_state_ = new_state;

    // 脏检查：仅"有意义的快照"变化时才算新版本（不含 lastEventTime/timestamp）
    const std::string signature = snapshotSignatureLocked() + "|alert:" + std::to_string(alert_seq_);
    const bool sig_changed = !last_signature_.has_value() || *last_signature_ != signature;
    if (sig_changed) {
        last_signature_ = signature;
        version_++;
        cv_.notify_all();
    }
    if (old_state != new_state && new_state == PetState::Alert) {
        last_alert_time_ = nowMs();
        alert_rounds_ = 1;  // 首轮提醒
        alert_suppressed_ = false;
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
                                           s.status, s.last_event, s.last_event_time,
                                           s.alert_message});
    }

    // 按 (project_name, ide) 折叠同项目多会话（如 Qoder 专家/子代理流），
    // 优先级 ConfirmationNeeded(3) > Working 系(2) > Idle(1)；同优先级时
    // 真实 hook 会话胜过 __window: 占位会话，其余平级对按最近活跃者胜。
    // 仅影响展示快照，内部按 session_id 的跟踪 / 超时清理 / 窗口同步不受影响。
    // 最近活跃者胜的动机（用户实测）：同项目开新对话时，旧会话往往残留
    // Thinking/ToolUse（未正常结束无 Stop），若按"先入者胜"，新会话第一个
    // UserPromptSubmit 的 Thinking 被旧会话平级挡住，直到旧会话超时清理
    // （最长 15 分钟）才浮出 —— 表现为"新任务第一个思考中识别不出来"。
    std::map<std::pair<std::string, std::string>, size_t> group_index;  // key -> 下标
    std::vector<SessionSnapshot> out;
    for (auto& s : sessions) {
        const std::string ide_key = s.ide.has_value() ? ideKindStr(*s.ide) : "";
        std::string project_key = s.project_path.empty() ? s.project_name : s.project_path;
        std::replace(project_key.begin(), project_key.end(), '\\', '/');
#ifdef _WIN32
        project_key = asciiLower(project_key);
#endif
        while (project_key.size() > 1 && project_key.back() == '/') project_key.pop_back();
        const auto key = std::make_pair(project_key, ide_key);
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
                 ((existing_placeholder && !new_placeholder) ||
                  (!new_placeholder && s.last_event_time > existing.last_event_time)));
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
        {"alertSeq", alert_seq_},
        // 权威待确认标志：含"提醒两轮无响应后抑制"，设备端据此回落 working
        {"hasConfirmation", has_confirmation_ && !alert_suppressed_},
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
    ps.alert_seq = alert_seq_;
    for (const auto& s : sessions) {
        dutyon::SessionInfo si;
        si.project_name = s.project_name;
        si.status = sessionStatusStr(s.status);
        si.ide = s.ide.has_value() ? ideKindStr(*s.ide) : "";
        si.alert_message = s.alert_message.value_or("");
        ps.sessions.push_back(std::move(si));
    }
    ps.has_confirmation = has_confirmation_ && !alert_suppressed_;
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
        const bool active_child_or_tool = std::any_of(it->second.actors.begin(), it->second.actors.end(),
            [](const auto& entry) { return (!entry.first.empty() && !entry.second.finished) || !entry.second.pending_tools.empty(); });
        const uint64_t limit = active_child_or_tool
                                   ? bc::kToolRunningTimeout
                                   : bc::kSessionTimeout;
        if (elapsed > limit) {
            window_miss_counts_.erase(id);
            cli_miss_counts_.erase(id);
            sessions_.erase(it);
            changed = true;
            continue;
        }
        // 长静默后才降 Idle：Qoder ask-user 对话框 / 工具执行期都会让会话
        // 合法地静默（后者可达十几分钟，放宽判定）。Thinking（LLM 生成期）
        // 静默时长 = 回复生成耗时，实测可达 4 分钟以上，单独用
        // kThinkingTimeout 放宽（kWorkingTimeout 的 3 分钟会误睡生成中的
        // 会话，见 backend_config.h 注释）。
        const uint64_t downgrade_timeout =
            active_child_or_tool
                ? bc::kToolRunningTimeout
                : (it->second.status == SessionStatus::Thinking
                       ? bc::kThinkingTimeout
                       : bc::kWorkingTimeout);
        if ((it->second.status == SessionStatus::Working ||
             it->second.status == SessionStatus::Thinking ||
             it->second.status == SessionStatus::ToolUse) &&
            elapsed > downgrade_timeout) {
            it->second.status = SessionStatus::Idle;
            changed = true;
        }
    }
    for (auto it = ended_sessions_.begin(); it != ended_sessions_.end();) {
        if (now >= it->second && now - it->second > bc::kSessionTimeout)
            it = ended_sessions_.erase(it);
        else ++it;
    }
    if (changed) recomputeStateLocked();
}

void StateManager::checkAndRemindAlert() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (overall_state_ != PetState::Alert) return;
    const uint64_t now = nowMs();
    const uint64_t since = now >= last_alert_time_ ? now - last_alert_time_ : 0;
    if (since < bc::kAlertReminder) return;
    if (alert_rounds_ < bc::kAlertMaxRounds) {
        last_alert_time_ = now;
        ++alert_rounds_;
        ++alert_seq_;
        last_signature_.reset();  // 使 update 也重新发出
        version_++;
        cv_.notify_all();
    } else {
        // 已播满两轮仍无响应：抑制 alert，回落 working（确认仍挂列表可响应）
        alert_suppressed_ = true;
        last_signature_.reset();
        recomputeStateLocked();
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
    for (const auto& d : detected) {
        detected_names.insert(d.name);
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
                it->second.window_seen = true;
                window_miss_counts_.erase(id);  // 窗口又可见了，重置宽限计数
                continue;
            }
            // Codex/OpenCode 是 CLI/TUI，没有 GUI 窗口可扫 —— 跳过窗口级
            // 清理，交给 SESSION_TIMEOUT / 进程存活同步
            const std::optional<IdeKind> session_ide = it->second.ide;
            // Claude Code 同样跳过，但不做进程判活（npm 版跑在 node.exe 里，
            // 桌面应用也叫 Claude.exe），靠 SessionEnd 与超时清理。
            if (session_ide == IdeKind::Codex || session_ide == IdeKind::OpenCode ||
                session_ide == IdeKind::Claude) {
                window_miss_counts_.erase(id);
                continue;
            }
            // 从没出现过同名窗口的 hook 会话：窗口扫描对它没有发言权，交给
            // Stop/SessionEnd 与静默超时。
            if (id.rfind(kWindowPrefix, 0) != 0 && !it->second.window_seen) continue;
            if (id.rfind(kWindowPrefix, 0) == 0) {
                // 占位会话 1:1 跟随窗口 —— 立即删除
                sessions_.erase(it);
                changed = true;
            } else {
                // 窗口确认消失 —— 与 hook 静默超时分档处理（用户决策）：
                // 思考中/工具执行期收不到 hook 信号可以等（10/15 分钟档），
                // 但 EnumWindows 是确定性信号，窗口没了就该尽快删，不能挂
                // 到 SESSION_TIMEOUT。仅保留 3 次扫描宽限（~12 秒）防闪烁：
                // 窗口重载/标题短暂不可解析时单次 miss 不至于误删。
                // 曾按"同 IDE 其他窗口仍在则豁免"处理，副作用是关掉的项目
                // 只要开着其他同 IDE 窗口就一直挂在列表（用户实测反馈）。
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
    if (!lv.scan_succeeded) {
        cli_miss_counts_.clear(); // 只有连续成功扫描的缺失才是退出证据
        return;
    }
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
            // 全局进程存在不能证明某个会话仍在工作，也不能刷新事件时间。
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
