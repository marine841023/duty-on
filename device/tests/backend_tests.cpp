#include "backend/state_manager.h"
#include "backend/backend_config.h"
#include "backend/ide_scanner.h"
#ifdef _WIN32
#include "backend/hooks_installer.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#endif
#include <iostream>
#include <stdexcept>

using namespace dutyon::backend;
using nlohmann::json;

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
HookEvent event(const std::string& name, const std::string& sid = "s1",
                const std::string& tool = "", const std::string& call = "") {
    json j = {{"session_id", sid}, {"hook_event_name", name}, {"ide", "codex"},
              {"cwd", "D:/projects/demo"}, {"tool_name", tool}, {"tool_use_id", call}};
    const auto ev = HookEvent::fromJson(j);
    check(ev.has_value(), "fixture event must parse");
    return *ev;
}
std::string status(StateManager& sm) {
    return sm.snapshotJson()["sessions"].at(0)["status"].get<std::string>();
}

void stateTests() {
    uint64_t now = 100000;
    StateManager sm([&] { return now; });
    check(!HookEvent::fromJson({{"session_id", "x"}, {"hook_event_name", "garbage"}}),
          "unknown event must not create a ghost");
    check(!event("PermissionRequest").tool_name, "absent optional string must stay absent");
    sm.handleHookEvent(event("UserPromptSubmit"));
    sm.handleHookEvent(event("PreToolUse", "s1", "Bash", "a"));
    sm.handleHookEvent(event("PreToolUse", "s1", "Bash", "b"));
    sm.handleHookEvent(event("PreToolUse", "s1", "Bash", "b")); // duplicate delivery
    sm.handleHookEvent(event("PostToolUse", "s1", "Bash", "a"));
    check(status(sm) == "tool-use", "one completion must not clear another parallel tool");
    sm.handleHookEvent(event("PermissionRequest", "s1", "Bash", "b"));
    check(status(sm) == "confirmation-needed", "permission must alert");
    sm.handleHookEvent(event("PreModelRequest"));
    check(status(sm) == "confirmation-needed", "parallel model event must not clear alert");
    sm.handleHookEvent(event("PreToolUse", "s1", "Bash", "parallel"));
    check(status(sm) == "confirmation-needed", "parallel tool start must not clear alert");
    sm.handleHookEvent(event("PostToolUse", "s1", "Bash", "parallel"));
    check(status(sm) == "confirmation-needed", "unrelated tool completion must not clear permission");
    sm.handleHookEvent(event("PostToolUse", "s1", "Bash", "b"));
    check(status(sm) == "thinking", "matching completion must clear permission and pending tool");

    sm.handleHookEvent(event("PreToolUse", "s1", "computer_use", "c"));
    check(status(sm) == "tool-use", "Codex computer use does not imply waiting for permission");
    sm.handleHookEvent(event("Interrupt"));
    check(status(sm) == "idle", "interrupt must finish turn immediately");
    sm.handleHookEvent(event("PostToolUse", "s1", "request_user_input", "c"));
    sm.handleHookEvent(event("PreModelRequest"));
    check(status(sm) == "idle", "late events must not revive interrupted turn");
    auto late_compact = event("SessionStart"); late_compact.source = "compact";
    sm.handleHookEvent(late_compact);
    check(status(sm) == "idle", "late compaction must not revive interrupted turn");

    auto prompt = event("UserPromptSubmit"); prompt.turn_id = "new-turn";
    sm.handleHookEvent(prompt);
    auto old_stop = event("Stop"); old_stop.turn_id = "old-turn";
    sm.handleHookEvent(old_stop);
    check(status(sm) == "thinking", "old turn Stop must not finish new turn");
    auto compact = event("SessionStart"); compact.source = "compact";
    sm.handleHookEvent(compact);
    check(status(sm) == "thinking", "compact SessionStart is not idle");
    sm.handleHookEvent(event("PreToolUse", "s1", "request_user_input", "q"));
    check(status(sm) == "confirmation-needed", "Codex input request must alert");
    sm.handleHookEvent(event("SessionEnd"));
    check(sm.sessionCount() == 0, "SessionEnd must remove session");
    sm.handleHookEvent(event("PostModelRequest"));
    check(sm.sessionCount() == 0, "late event must not recreate ended session");
    sm.handleHookEvent(late_compact);
    check(sm.sessionCount() == 0, "late compaction must not recreate ended session");

    sm.handleHookEvent(event("UserPromptSubmit"));
    const auto last_event = sm.snapshotJson()["lastEventAt"];
    CliLiveness alive; alive.scan_succeeded = true; alive.codex_alive = true;
    now += bc::kThinkingTimeout + 1;
    sm.syncCliLiveness(alive);
    check(sm.snapshotJson()["lastEventAt"] == last_event, "process scan is not a hook event");
    sm.cleanupStaleSessions();
    check(sm.snapshotJson()["overallState"] == "sleeping", "alive process must not make stale turn immortal");
    check(sm.sessionCount() == 0, "stale session must expire despite another live Codex");

    sm.handleHookEvent(event("UserPromptSubmit"));
    CliLiveness absent; absent.scan_succeeded = true;
    sm.syncCliLiveness(absent);
    sm.syncCliLiveness(CliLiveness{}); // failed enumeration breaks consecutive misses
    sm.syncCliLiveness(absent);
    check(sm.sessionCount() == 1, "failed scan must not count as process exit");
    sm.syncCliLiveness(absent);
    check(sm.sessionCount() == 0, "two successful missing scans remove crashed agent");

    sm.handleHookEvent(event("PreToolUse", "s1", "Bash", "long"));
    now += bc::kThinkingTimeout + 1;
    sm.cleanupStaleSessions();
    check(sm.sessionCount() == 1 && status(sm) == "tool-use", "long running tools need extended timeout");
    sm.resetAllToIdle();
    now += bc::kSessionTimeout + 1;
    sm.cleanupStaleSessions();
    check(sm.sessionCount() == 0, "reset must clear pending tools");

    auto one = event("UserPromptSubmit", "one"); one.project_path = "D:/one/demo";
    auto two = event("UserPromptSubmit", "two"); two.project_path = "D:/two/demo";
    sm.handleHookEvent(one); sm.handleHookEvent(two);
    check(sm.petStatus().session_count == 2, "same folder basename in different paths must not merge");
    now -= 5000;
    sm.cleanupStaleSessions();
    check(sm.sessionCount() == 2, "clock rollback must not underflow timeout");

    check(cliProcessIde(L"CODEX.EXE") == IdeKind::Codex, "exact binary match ignores case");
    for (const auto* name : {L"codex-relay.exe", L"codex-windows-sandbox-service.exe",
                            L"codex-computer-use.exe", L"codex-code-mode-host.exe", L"my-codex.exe"})
        check(!cliProcessIde(name), "helper processes must not keep Codex sessions alive");
}

void actorTests() {
    uint64_t now = 100000;
    StateManager sm([&] { return now; });
    auto send = [&](const std::string& name, const std::string& agent = "", const std::string& call = "", const std::string& request = "") {
        auto e = event(name, "actors", call.empty() ? "" : "Bash", call);
        if (!agent.empty()) e.agent_id = agent;
        if (!request.empty()) e.request_id = request;
        sm.handleHookEvent(e);
    };
    send("SessionStart", "root");
    send("UserPromptSubmit", "root");
    send("PreToolUse", "root", "same-call");
    send("SubagentStart", "child");
    send("PreToolUse", "child", "same-call");
    send("PermissionRequest", "child", "same-call", "q1");
    const auto first_seq = sm.petStatus().alert_seq;
    check(first_seq > 0, "new confirmation must increment reminder sequence");
    send("PostToolUse", "root", "same-call");
    check(status(sm) == "confirmation-needed", "same call ID in parent must not clear child confirmation");
    send("Stop", "root");
    check(status(sm) == "confirmation-needed", "parent Stop must preserve child confirmation");
    send("PermissionRequest", "child", "same-call", "q1");
    check(sm.petStatus().alert_seq == first_seq, "duplicate request must not replay sound");
    send("PermissionRequest", "child", "second-call", "q2");
    check(sm.petStatus().alert_seq == first_seq + 1, "additional request while alert must remind");
    send("PermissionResolved", "child", "", "q1");
    check(status(sm) == "confirmation-needed", "resolving one request must preserve the other");
    send("PermissionRequest", "child", "same-call", "q1");
    check(sm.petStatus().alert_seq == first_seq + 1, "late resolved request must not alert again");
    now += bc::kAlertReminder - 1; sm.checkAndRemindAlert();
    check(sm.petStatus().alert_seq == first_seq + 1, "reminder must respect interval");
    ++now; sm.checkAndRemindAlert();
    check(sm.petStatus().alert_seq == first_seq + 2, "ongoing confirmation must emit periodic reminder sequence");
    check(sm.snapshotJson()["alertSeq"] == sm.petStatus().alert_seq, "local and network reminder sequence must match");
    send("PostToolUseFailure", "child", "second-call");
    check(status(sm) == "tool-use", "failed call must clear its permission but keep other work");
    send("SubagentStop", "child");
    check(status(sm) == "idle", "finished child and finished parent must become idle");
    send("SubagentStart", "child");
    send("PermissionRequest", "child", "late");
    check(status(sm) == "idle", "duplicate lifecycle must not revive finished child");
    send("SessionEnd", "root");
    check(sm.sessionCount() == 0, "root SessionEnd with agent ID must delete conversation");

    send("UserPromptSubmit");
    send("PermissionRequest", "", "early", "early-request");
    check(status(sm) == "confirmation-needed", "permission before Pre must alert");
    send("PreToolUse", "", "early");
    send("PostToolUse", "", "unrelated");
    check(status(sm) == "confirmation-needed", "unrelated completion cannot clear early permission");
    send("PostToolUse", "", "early");
    send("PermissionRequest", "", "early", "late-request");
    check(status(sm) == "thinking", "permission for completed call must stay suppressed");
    auto notice = event("Notification", "actors");
    notice.notification_type = "document_review";
    sm.handleHookEvent(notice);
    send("PostToolUse", "", "unrelated");
    notice.notification_type = "idle_prompt";
    sm.handleHookEvent(notice);
    check(status(sm) == "confirmation-needed", "unbound review confirmation must survive unrelated completion notices");
    send("SubagentStart", "child2");
    send("PermissionRequest", "child2", "pending");
    send("Interrupt");
    send("PreToolUse", "child2", "late");
    send("SubagentStart", "previously-unseen-child");
    send("PermissionRequest", "previously-unseen-child", "late");
    check(status(sm) == "idle", "root interrupt must cancel whole tree and suppress late child work");

    sm.resetAllToIdle();
    auto child = event("UserPromptSubmit", "child-session");
    child.parent_session_id = "actors"; child.ide = IdeKind::OpenCode;
    sm.handleHookEvent(child);
    check(sm.sessionCount() == 1 && status(sm) == "thinking", "child sessions must aggregate under parent");
    now += bc::kThinkingTimeout + 1; sm.cleanupStaleSessions();
    check(sm.sessionCount() == 1 && status(sm) == "thinking", "active child needs longer silence allowance");
    child.hook_event_name = "SessionEnd"; sm.handleHookEvent(child);
    check(sm.sessionCount() == 1 && status(sm) == "idle", "child SessionEnd must not remove parent");

    sm.resetAllToIdle();
    auto cursor = event("UserPromptSubmit", "actors"); cursor.ide = IdeKind::Cursor;
    sm.handleHookEvent(cursor);
    cursor.hook_event_name = "SubagentStart"; cursor.agent_id = "unpairable-child";
    sm.handleHookEvent(cursor);
    cursor.hook_event_name = "Stop"; cursor.agent_id.reset();
    sm.handleHookEvent(cursor);
    check(status(sm) == "idle", "Cursor lifecycle without pairable stop must not leave ghost work");

    sm.resetAllToIdle();
    send("UserPromptSubmit");
    notice.notification_type.reset();
    notice.message = "progress updated";
    sm.handleHookEvent(notice);
    check(status(sm) == "thinking", "informational notification must not finish active work");
}

#ifdef _WIN32
namespace fs = std::filesystem;
json readJson(const fs::path& path) { std::ifstream in(path); return json::parse(in); }
void writeJson(const fs::path& path, const json& j) { std::ofstream(path) << j.dump(2); }

// Scoped environment overrides: installer tests can only touch an isolated temporary home.
struct TestHome {
    fs::path path = fs::temp_directory_path() / (L"dutyon-hooks-测试-" + std::to_wstring(currentMillis()));
    std::wstring old_home = _wgetenv(L"USERPROFILE") ? _wgetenv(L"USERPROFILE") : L"";
    std::wstring old_codex = _wgetenv(L"CODEX_HOME") ? _wgetenv(L"CODEX_HOME") : L"";
    TestHome() {
        fs::create_directories(path / "custom codex");
        _wputenv_s(L"USERPROFILE", path.c_str());
        _wputenv_s(L"CODEX_HOME", (path / "custom codex").c_str());
    }
    ~TestHome() {
        _wputenv_s(L"USERPROFILE", old_home.c_str());
        _wputenv_s(L"CODEX_HOME", old_codex.c_str());
        std::error_code ec; fs::remove_all(path, ec);
    }
};

void installerTests(const std::string& hooks) {
    TestHome home;
    const auto path = home.path / "custom codex" / "hooks.json";
    json foreign = {{"type", "command"}, {"command", "echo keep-me"}, {"timeout", 9}};
    json other_app = {{"type", "command"}, {"command", "run .dutyon/other-script.ps1"}};
    json old = {{"type", "command"}, {"command", "powershell .dutyon/hooks/trae-hook-bridge.ps1"}};
    json shared = {{"matcher", "Bash"}, {"description", "keep group metadata"},
                   {"hooks", json::array({foreign, old, other_app})}};
    writeJson(path, {{"description", "user config"}, {"hooks", {{"PreToolUse", json::array({shared})}}}});
    const auto r = installHooks(hooks);
    check(r.success, r.error.value_or("installation failed").c_str());
    auto installed = readJson(path);
    check(!fs::exists(home.path / ".codex"), "CODEX_HOME must override default directory");
    const auto& group = installed["hooks"]["PreToolUse"][0];
    check(group["hooks"] == json::array({foreign, other_app}), "merge must preserve other handlers in same matcher");
    check(group["matcher"] == "Bash" && group["description"] == shared["description"], "merge must preserve group metadata");
    check(installed["description"] == "user config", "merge must preserve unrelated keys");
    for (const auto* name : bc::kCodexHookEvents) {
        const auto& handler = installed["hooks"][name].back()["hooks"][0];
        check(handler["command"].get<std::string>().find("$env:") == std::string::npos,
              "hook command must not depend on parent shell env expansion");
        check(handler["command"].get<std::string>().find(home.path.u8string()) != std::string::npos,
              "hook command must preserve Chinese paths and spaces");
    }
    check(installed["hooks"]["Interrupt"][0]["hooks"][0]["timeout"] == 3, "Interrupt timeout must fit Codex limit");
    check(installed["hooks"]["SessionEnd"][0]["hooks"][0]["timeout"] == 3, "SessionEnd timeout must fit Codex limit");
    auto mtime = fs::last_write_time(path);
    check(installHooks(hooks).success, "second install succeeds");
    check(readJson(path) == installed && fs::last_write_time(path) == mtime, "reinstall must be idempotent without rewriting config");
    check(isHooksInstalled().codex_hooks_exist, "status must honor CODEX_HOME");
    writeJson(path, {{"description", ".dutyon/hooks/trae-hook-bridge.ps1"}, {"hooks", json::object()}});
    check(!isHooksInstalled().codex_hooks_exist, "text in description is not an installed hook");
    check(scanCliProcesses().scan_succeeded, "real Windows process enumeration must complete");
    const auto trae_path = home.path / ".trae-cn" / "hooks.json";
    const auto seconds = currentMillis() / 1000;
    for (uint64_t offset = 0; offset < 10; ++offset)
        fs::create_directory(fs::path(trae_path.wstring() + L".dutyon-backup-" +
                                     std::to_wstring(seconds + offset)));
    std::ofstream(trae_path) << "invalid original config";
    check(!installHooks(hooks).success, "backup failure must abort configuration replacement");
    std::ifstream original(trae_path);
    const std::string unchanged((std::istreambuf_iterator<char>(original)), {});
    check(unchanged == "invalid original config", "failed backup must preserve original bytes");
    original.close();
    writeJson(trae_path, {{"hooks", "invalid hooks shape"}});
    check(!installHooks(hooks).success, "backup failure must also preserve invalid hooks shape");
    check(readJson(trae_path)["hooks"] == "invalid hooks shape", "invalid hooks must remain unchanged");
}
#endif

int main(int argc, char** argv) {
    try {
        stateTests();
        actorTests();
#ifdef _WIN32
        check(argc == 2, "pass hooks source directory");
        installerTests(argv[1]);
#endif
        std::cout << "All backend regression checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
