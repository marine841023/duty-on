// IDE hook 安装器实现 —— hooks_installer.rs 1:1 移植（Windows 路径）。

#include "backend/hooks_installer.h"

#include <windows.h>

#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>

#include "backend/backend_config.h"

namespace dutyon::backend {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path homeDir() {
    const wchar_t* home = _wgetenv(L"USERPROFILE");
    if (!home || !*home) home = _wgetenv(L"HOME");
    return home ? fs::path(home) : fs::path();
}

fs::path codexHome() {
    const wchar_t* override_home = _wgetenv(L"CODEX_HOME");
    return override_home && *override_home ? fs::path(override_home) : homeDir() / ".codex";
}

// 当前平台的桥接脚本名（Windows 走 PowerShell）
const char* bridgeFilename() { return "trae-hook-bridge.ps1"; }

// hook 配置条目形状：Nested（Trae/Qoder/Codex，Claude-Code 风格）vs
// Flat（Cursor：event -> [{command, timeout, loop_limit}]）
enum class HookFormat { Nested, Flat };

// 写进 IDE hooks 配置的命令串。Windows 上显式 `powershell -File` +
// `-ExecutionPolicy Bypass`：企业机器常把执行策略设为 Restricted，会静默
// 拒绝 .ps1 桥接（hook 触发了但什么都没发生）。-HookEvent 不能叫 -Event
// （与 PowerShell 自动变量 $Event 冲突，会把解析出的 JSON 对象降级成
// String —— 实测所有 POST 422）。
std::string hookCommand(const std::string& ide, const std::string& event) {
    std::string event_arg = event.empty() ? "" : " -HookEvent " + event;
    const auto bridge = homeDir() / ".dutyon" / "hooks" / bridgeFilename();
    return "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
           bridge.u8string() + "\" -Ide " + ide + event_arg;
}

// 命令是否属于本应用（同时匹配 .dutyon 与旧名 .trae-pet，改名后重装
// 去重替换而非并排叠加）
bool isPetCommand(const std::string& cmd) {
    return (cmd.find(".dutyon") != std::string::npos ||
            cmd.find(".trae-pet") != std::string::npos) &&
           cmd.find("trae-hook-bridge.") != std::string::npos;
}

bool isPetHook(const json& hook) {
    if (!hook.is_object()) return false;
    for (const char* key : {"command", "commandWindows"}) {
        auto it = hook.find(key);
        if (it != hook.end() && it->is_string() && isPetCommand(it->get<std::string>()))
            return true;
    }
    return false;
}

// 读整个文本文件（不存在返回 nullopt）
std::optional<std::string> readFileIfExists(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool writeFile(const fs::path& p, const std::string& content) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return out.good();
}

// 复制为同目录备份（<名>.dutyon-backup-<unix-ts>），返回备份路径
std::optional<std::string> backupFile(const fs::path& path) {
    const auto ts = (uint64_t)std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    fs::path backup = path;
    backup.replace_filename(path.filename().u8string() + ".dutyon-backup-" + std::to_string(ts));
    std::error_code ec;
    fs::copy_file(path, backup, ec);
    if (ec) return std::nullopt;
    return backup.u8string();
}

// 把宠物 hook 条目合并进配置文件。返回 warning（nullopt = 无）；失败时
// err 置错误消息。稳健性（对其他工具改过的配置）：
//   - 容忍 UTF-8 BOM
//   - 文件存在但不可解析/根非 object：先备份原文件再从 {} 重建，并警告
//   - 事件的值不是数组：原值包装为数组首元素保留而非丢弃
std::optional<std::string> mergeHooksIntoFile(
    const fs::path& path, const char* const* events, int events_count,
    const std::function<std::string(const char*)>& command_for_event, const char* shell,
    bool add_version, bool strip_version, HookFormat format, std::string& err) {
    if (path.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
    }

    std::optional<std::string> warning;
    json existing = json::object();
    if (auto content = readFileIfExists(path)) {
        std::string s = *content;
        // 剥 UTF-8 BOM（某些编辑器写 JSON 配置带 BOM，会让解析失败）
        if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
            (unsigned char)s[2] == 0xBF) {
            s = s.substr(3);
        }
        bool blank = true;
        for (char c : s)
            if (!isspace((unsigned char)c)) { blank = false; break; }
        if (!blank) {
            json parsed = json::parse(s, nullptr, /*allow_exceptions=*/false);
            if (!parsed.is_discarded() && parsed.is_object()) {
                existing = std::move(parsed);
            } else {
                // 存在但不是合法 JSON / 根不是 object —— 绝不静默清空：
                // 备份原文件、从 {} 重建、上报
                const auto backup = backupFile(path);
                if (!backup) {
                    err = "Cannot back up invalid configuration; left unchanged: " + path.u8string();
                    return std::nullopt;
                }
                const std::string& bak = *backup;
                warning = path.u8string() + (parsed.is_discarded()
                                               ? ": not valid JSON; backed up to "
                                               : ": root was not a JSON object; backed up to ") +
                          bak + " and rebuilt";
            }
        }
    }

    // 变化检测基线：解析出的原始内容（文件不存在/损坏则为空 object）。合并
    // 完若与之深比较相等，说明磁盘上已是要写的结果 -> 跳过写盘，避免每次
    // 启动重装都 bump mtime、触发 IDE 反复重载 hook（nlohmann == 与键序/
    // 缩进无关，只比语义）。须在 version/hooks 归一化之前取，才能覆盖
    // "文件本就缺 version" 这类需要补写的情况。
    const json baseline = existing;

    if (strip_version) {
        existing.erase("version");  // Codex CLI 拒绝 version 字段（unknown field）
    } else if (add_version && !existing.contains("version")) {
        existing["version"] = 1;
    }
    if (existing.contains("hooks") && !existing["hooks"].is_object()) {
        const auto backup = backupFile(path);
        if (!backup) {
            err = "Cannot back up invalid hooks; left unchanged: " + path.u8string();
            return std::nullopt;
        }
        const std::string& bak = *backup;
        const std::string note = path.u8string() + ": \"hooks\" was not a JSON object; backed up to " +
                                 bak + " and reset it";
        warning = warning.has_value() ? *warning + "; " + note : note;
        existing["hooks"] = json::object();
    } else if (!existing.contains("hooks")) {
        existing["hooks"] = json::object();
    }

    json& hooks = existing["hooks"];
    for (int i = 0; i < events_count; i++) {
        const char* event = events[i];
        const std::string cmd = command_for_event(event);

        // 本事件的宠物条目（IDE 原生形状）
        json group;
        if (format == HookFormat::Nested) {
            // Codex 的 Interrupt/SessionEnd 最多允许 3 秒，其他事件用常规上限。
            const uint64_t timeout = std::string(event) == "Interrupt" ||
                                     std::string(event) == "SessionEnd"
                                         ? 3 : bc::kBridgeTimeoutSec;
            json hook = {{"type", "command"}, {"command", cmd}, {"timeout", timeout}};
            if (shell) hook["shell"] = shell;
            group = {{"hooks", json::array({std::move(hook)})}};
        } else {
            // Cursor 的 Flat 条目。loop_limit:null 解除 Cursor 对 stop 类
            // 事件默认 5 次调用上限，长会话持续收到事件
            group = {{"command", cmd},
                     {"timeout", bc::kBridgeTimeoutSec},
                     {"loop_limit", nullptr}};
        }

        if (!hooks.contains(event)) hooks[event] = json::array();
        json& arr = hooks[event];
        if (!arr.is_array()) {
            // 别的工具给这个事件写了非数组值：包装成数组首元素保留
            json old = arr;
            arr = json::array({std::move(old)});
            if (!warning.has_value()) {
                warning = path.u8string() + ": event \"" + event +
                          "\" had a non-array value; preserved it inside the merged array";
            }
        }

        // 去重：移除已有宠物条目（重装不叠加；旧 .trae-pet 条目一并替换）
        json kept = json::array();
        for (auto e : arr) {
            if (format == HookFormat::Nested && e.is_object() &&
                e.contains("hooks") && e["hooks"].is_array()) {
                json handlers = json::array();
                bool removed = false;
                for (const auto& h : e["hooks"]) {
                    if (isPetHook(h)) removed = true;
                    else handlers.push_back(h);
                }
                // 只移除自己的 handler；共享 matcher 内的其他 handler 和元数据保留。
                if (removed && handlers.empty()) continue;
                e["hooks"] = std::move(handlers);
            } else if (format == HookFormat::Flat && isPetHook(e)) {
                continue;
            }
            kept.push_back(std::move(e));
        }
        kept.push_back(std::move(group));
        arr = std::move(kept);
    }

    // 无实质变化：磁盘内容已与合并结果语义一致，跳过写盘（见 baseline 注释）
    if (existing == baseline) return warning;

    if (!writeFile(path, existing.dump(2) + "\n")) {
        err = "Failed to write " + path.u8string();
        return std::nullopt;
    }
    return warning;
}

InstallResult fail(const std::string& error) {
    InstallResult r;
    r.success = false;
    r.error = error;
    return r;
}

// is_installed 用：OpenCode 插件源码固定携带的标记（区分我们的插件与
// 用户同名文件）
constexpr const char* kOpencodePluginMarker = "id: 'dutyon'";

// OpenCode 桥接插件源码（写入 ~/.config/opencode/plugins/dutyon-bridge.js）。
// OpenCode 没有配置式 hook，唯一扩展点是事件总线 JS 插件；`export default
// { id, server }` 形状把 event 钩子注册在服务端（session/message/permission
// 事件在那里触发）。按调用/请求 ID 去重流式事件并关联父子会话；
// hooks/tests/opencode.test.cjs 直接测试此处源码，避免安装内容与测试分叉。
const char* kOpencodePluginSource = R"dutyon_src(// DutyOn OpenCode server event bridge. Generated by the desktop installer.
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
const ENDPOINT = 'http://127.0.0.1:17521/hook'
const LOG_PATH = path.join(os.homedir(), '.dutyon', 'hooks', 'opencode-bridge.log')
function log(msg) {
  try {
    fs.mkdirSync(path.dirname(LOG_PATH), { recursive: true })
    try {
      const lines = fs.readFileSync(LOG_PATH, 'utf8').split('\n')
      if (lines.length > 500) fs.writeFileSync(LOG_PATH, lines.slice(-250).join('\n'))
    } catch {}
    fs.appendFileSync(LOG_PATH, new Date().toISOString() + ' ' + msg + '\n')
  } catch {}
}
async function post(payload) {
  const ctrl = new AbortController()
  const timer = setTimeout(() => ctrl.abort(), 2000)
  try {
    const res = await fetch(ENDPOINT, { method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload), signal: ctrl.signal })
    log('POST ' + payload.hook_event_name + ' -> ' + res.status)
  } catch { log('POST ' + payload.hook_event_name + ' failed') }
  finally { clearTimeout(timer) }
}
function makeHooks() {
  const sessions = new Map(), messages = new Map(), tools = new Map(), requests = new Map()
  function remember(map, key, value, limit = 2048) {
    map.set(key, value)
    if (map.size > limit) map.delete(map.keys().next().value)
  }
  function info(id) {
    if (!sessions.has(id)) remember(sessions, id, { cwd: '', active: false }, 512)
    return sessions.get(id)
  }
  function base(id) {
    const s = info(id)
    let parent = s.parent, root = parent
    const seen = new Set([id])
    while (parent && !seen.has(parent)) {
      seen.add(parent); root = parent; parent = sessions.get(parent)?.parent
    }
    const cwd = s.cwd || sessions.get(root)?.cwd || ''
    return { session_id: 'opencode-' + id, ide: 'opencode', cwd, project_path: cwd,
      project_name: cwd.replace(/\\/g, '/').split('/').filter(Boolean).pop() || '',
      ...(root && !seen.has(parent) ? { parent_session_id: 'opencode-' + root } : {}) }
  }
  async function emit(id, name, extra = {}) {
    if (id) await post({ ...base(id), hook_event_name: name, ...extra })
  }
  async function start(id) {
    if (!info(id).active) {
      info(id).active = true
      await emit(id, 'UserPromptSubmit')
    }
  }
  function forget(id) {
    sessions.delete(id)
    for (const map of [messages, tools, requests])
      for (const [key, value] of map) if (value.sessionID === id) map.delete(key)
  }
  return { event: async ({ event }) => {
    const type = event?.type?.replace('question.v2.', 'question.'), p = event?.properties || {}
    try {
      if ((type === 'session.created' || type === 'session.updated') && p.info?.id) {
        const id = p.info.id, s = info(id)
        if (p.info.directory) s.cwd = p.info.directory
        if (p.info.parentID) s.parent = p.info.parentID
        if (p.info.time?.archived) { await emit(id, 'SessionEnd'); forget(id) }
        else if (type === 'session.created') await emit(id, 'SessionStart')
        return
      }
      if (type === 'session.deleted' && p.info?.id) {
        await emit(p.info.id, 'SessionEnd'); forget(p.info.id); return
      }
      if ((type === 'session.status' || type === 'session.idle') && p.sessionID) {
        const state = type === 'session.idle' ? 'idle' : p.status?.type
        if (state === 'idle') { info(p.sessionID).active = false; await emit(p.sessionID, 'Stop') }
        else if (state === 'busy' || state === 'retry') { await start(p.sessionID); await emit(p.sessionID, 'PreModelRequest') }
        return
      }
      if (type === 'session.error' && p.sessionID) {
        info(p.sessionID).active = false; await emit(p.sessionID, 'Interrupt'); return
      }
      if (type === 'message.updated' && p.info?.id && p.info.sessionID) {
        const prev = messages.get(p.info.id)
        remember(messages, p.info.id, { role: p.info.role, sessionID: p.info.sessionID, prompted: prev?.prompted || false })
        return
      }
      const part = p.part
      if (type === 'message.part.updated' && part?.type === 'text') {
        const meta = messages.get(part.messageID)
        if (meta?.role === 'user' && part.text && !meta.prompted) {
          meta.prompted = true; await start(meta.sessionID)
        }
        return
      }
      if (type === 'message.part.updated' && part?.type === 'tool' && part.sessionID) {
        const call = part.callID || part.id
        if (!call) return
        const key = part.sessionID + ':' + call, state = part.state?.status, prev = tools.get(key)
        const phase = state === 'pending' || state === 'running' ? 'start' :
          state === 'completed' || state === 'error' ? 'end' : ''
        if (!phase || prev?.phase === phase || prev?.phase === 'end') return
        remember(tools, key, { phase, sessionID: part.sessionID })
        if (phase === 'start') await start(part.sessionID)
        await emit(part.sessionID, phase === 'start' ? 'PreToolUse' :
          state === 'error' ? 'PostToolUseFailure' : 'PostToolUse', { tool_name: part.tool || 'tool', tool_use_id: call })
        return
      }
      if ((type === 'permission.asked' || type === 'question.asked') && p.sessionID && p.id) {
        const key = type.split('.')[0] + ':' + p.id
        if (requests.has(key)) return
        const extra = { request_id: key, ...(p.tool?.callID ? { tool_use_id: p.tool.callID } : {}) }
        remember(requests, key, { sessionID: p.sessionID, extra })
        await start(p.sessionID); await emit(p.sessionID, 'PermissionRequest', extra); return
      }
      if (['permission.replied', 'question.replied', 'question.rejected'].includes(type)) {
        if (!p.requestID && !p.id) return
        const key = type.split('.')[0] + ':' + (p.requestID || p.id || '')
        const request = requests.get(key)
        if (request?.resolved) return
        if (request || p.sessionID) {
          // Keep a bounded tombstone: a late asked must not call start() and
          // reset the backend's already-resolved request history.
          remember(requests, key, { ...request, sessionID: request?.sessionID || p.sessionID, resolved: true })
          await emit(request?.sessionID || p.sessionID, 'PermissionResolved', request?.extra || { request_id: key })
        }
      }
    } catch { log('event ' + type + ' failed') }
  } }
}
export default { id: 'dutyon', server: async () => makeHooks() }
)dutyon_src";

} // namespace

nlohmann::json InstallResult::toJson() const {
    json j = {{"success", success}, {"needsEnable", needs_enable}};
    auto opt = [](const char* k, const std::optional<std::string>& v, json& j) {
        if (v.has_value()) j[k] = *v;
    };
    opt("error", error, j);
    opt("warning", warning, j);
    opt("hookDir", hook_dir, j);
    opt("hooksPath", hooks_path, j);
    opt("qoderHooksPath", qoder_hooks_path, j);
    opt("cursorHooksPath", cursor_hooks_path, j);
    opt("codexHooksPath", codex_hooks_path, j);
    opt("opencodePluginPath", opencode_plugin_path, j);
    return j;
}

nlohmann::json InstalledStatus::toJson() const {
    return {
        {"installed", installed},          {"hooksExist", hooks_exist},
        {"bridgeExists", bridge_exists},   {"qoderHooksExist", qoder_hooks_exist},
        {"cursorHooksExist", cursor_hooks_exist}, {"codexHooksExist", codex_hooks_exist},
        {"opencodePluginExist", opencode_plugin_exist},
    };
}

std::string resolveHooksSourceDir() {
    std::vector<fs::path> candidates;
    wchar_t exe_buf[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_buf, MAX_PATH) > 0) {
        const fs::path exe(exe_buf);
        if (exe.has_parent_path()) {
            const fs::path dir = exe.parent_path();
            candidates.push_back(dir / "hooks");
            candidates.push_back(dir / "resources" / "hooks");
            candidates.push_back(dir / ".." / ".." / ".." / "hooks");
        }
    }
    for (const auto& c : candidates) {
        if (fs::exists(c / bridgeFilename())) return c.u8string();
    }
    return candidates.empty() ? std::string() : candidates.front().u8string();
}

InstallResult installHooks(const std::string& hooks_source_dir) {
    const fs::path home = homeDir();
    if (home.empty()) return fail("Cannot resolve user home directory");
    const fs::path bridge_src = fs::u8path(hooks_source_dir) / bridgeFilename();
    if (!fs::exists(bridge_src)) {
        return fail("Bridge script not found: " + bridge_src.u8string());
    }

    const fs::path target_hook_dir = fs::path(home) / ".dutyon" / "hooks";
    std::error_code ec;
    fs::create_directories(target_hook_dir, ec);
    if (ec) return fail("Failed to create hook dir: " + ec.message());

    // 复制桥接脚本（内容一致则跳过，避免每次启动重装都重写 bump mtime）
    if (auto content = readFileIfExists(bridge_src)) {
        const fs::path bridge_dst = target_hook_dir / bridgeFilename();
        const auto cur = readFileIfExists(bridge_dst);
        if (!cur.has_value() || *cur != *content) {
            if (!writeFile(bridge_dst, *content)) {
                return fail("Failed to copy bridge: " + bridge_dst.u8string());
            }
        }
    } else {
        return fail("Failed to read bridge: " + bridge_src.u8string());
    }

    // 单独的安装脚本（存在则一并复制，内容一致跳过，不存在跳过）
    const fs::path installer_src = fs::u8path(hooks_source_dir) / "install-hooks.ps1";
    if (auto content = readFileIfExists(installer_src)) {
        const fs::path installer_dst = target_hook_dir / "install-hooks.ps1";
        const auto cur = readFileIfExists(installer_dst);
        if (!cur.has_value() || *cur != *content) writeFile(installer_dst, *content);
    }

    std::vector<std::string> warnings;

    // ---- Trae: ~/.trae-cn/hooks.json（Nested，无 shell，version:1）----
    const fs::path trae_hooks_path = fs::path(home) / ".trae-cn" / "hooks.json";
    const std::string trae_cmd = hookCommand("trae", "");
    {
        std::string err;
        auto w = mergeHooksIntoFile(
            trae_hooks_path, bc::kHookEvents, bc::kHookEventsCount,
            [&](const char*) { return trae_cmd; }, nullptr, /*add_version=*/true,
            /*strip_version=*/false, HookFormat::Nested, err);
        if (!err.empty()) return fail(err);
        if (w.has_value()) warnings.push_back(*w);
    }

    // ---- Qoder: ~/.qoder/settings.json 与 ~/.qoder-cn/settings.json ----
    // 国际版用 ~/.qoder，Qoder CN 版用 ~/.qoder-cn（进程名 "Qoder CN IDE.exe"）；
    // 两者共存时都接线，均仅在已安装时才写。
    std::optional<std::string> qoder_path_str;
    const char* const kQoderDirs[] = {".qoder", ".qoder-cn"};
    for (const char* qdir : kQoderDirs) {
        const fs::path qoder_dir = fs::path(home) / qdir;
        if (!fs::exists(qoder_dir)) continue;
        const fs::path qoder_hooks_path = qoder_dir / "settings.json";
        const std::string qoder_cmd = hookCommand("qoder", "");
        std::string err;
        auto w = mergeHooksIntoFile(
            qoder_hooks_path, bc::kQoderHookEvents, bc::kQoderHookEventsCount,
            [&](const char*) { return qoder_cmd; }, "powershell",
            /*add_version=*/false, /*strip_version=*/false, HookFormat::Nested, err);
        if (err.empty()) {
            if (w.has_value()) warnings.push_back(std::string(qdir) + ": " + *w);
            if (!qoder_path_str.has_value()) qoder_path_str = qoder_hooks_path.u8string();
        } else {
            warnings.push_back(std::string(qdir) + " settings.json merge failed: " + err);
        }
    }

    // ---- Cursor: ~/.cursor/hooks.json（Flat + camelCase 事件 + 事件烘焙进命令）----
    std::optional<std::string> cursor_path_str;
    const fs::path cursor_dir = fs::path(home) / ".cursor";
    if (fs::exists(cursor_dir)) {
        const fs::path cursor_hooks_path = cursor_dir / "hooks.json";
        std::string err;
        auto w = mergeHooksIntoFile(
            cursor_hooks_path, bc::kCursorHookEvents, bc::kCursorHookEventsCount,
            [](const char* ev) { return hookCommand("cursor", ev); }, nullptr,
            /*add_version=*/true, /*strip_version=*/false, HookFormat::Flat, err);
        if (err.empty()) {
            if (w.has_value()) warnings.push_back("Cursor: " + *w);
            cursor_path_str = cursor_hooks_path.u8string();
        } else {
            warnings.push_back("Cursor hooks.json merge failed: " + err);
        }
    }

    // ---- Codex: ~/.codex/hooks.json（Nested，剥 version —— CLI 拒绝该字段）----
    std::optional<std::string> codex_path_str;
    const fs::path codex_dir = codexHome();
    if (fs::exists(codex_dir)) {
        const fs::path codex_hooks_path = codex_dir / "hooks.json";
        const std::string codex_cmd = hookCommand("codex", "");
        std::string err;
        auto w = mergeHooksIntoFile(
            codex_hooks_path, bc::kCodexHookEvents, bc::kCodexHookEventsCount,
            [&](const char*) { return codex_cmd; }, nullptr,
            /*add_version=*/false, /*strip_version=*/true, HookFormat::Nested, err);
        if (err.empty()) {
            if (w.has_value()) warnings.push_back("Codex: " + *w);
            codex_path_str = codex_hooks_path.u8string();
        } else {
            warnings.push_back("Codex hooks.json merge failed: " + err);
        }
    }

    // ---- OpenCode: JS 插件（~/.config/opencode 存在才写，避免凭空生成目录树）----
    std::optional<std::string> opencode_path_str;
    const fs::path plugins_dir = fs::path(home) / bc::kOpencodePluginSubdir;
    if (plugins_dir.has_parent_path() && fs::exists(plugins_dir.parent_path())) {
        std::error_code ec2;
        fs::create_directories(plugins_dir, ec2);
        if (ec2) {
            warnings.push_back("OpenCode plugins dir create failed: " + ec2.message());
        } else {
            const fs::path plugin_path = plugins_dir / bc::kOpencodePluginFilename;
            if (writeFile(plugin_path, kOpencodePluginSource)) {
                opencode_path_str = plugin_path.u8string();
            } else {
                warnings.push_back("OpenCode plugin write failed: " + plugin_path.u8string());
            }
        }
    }

    InstallResult r;
    r.success = true;
    if (!warnings.empty()) {
        std::string all;
        for (size_t i = 0; i < warnings.size(); i++) {
            if (i) all += "\n";
            all += warnings[i];
        }
        r.warning = all;
    }
    r.hook_dir = target_hook_dir.u8string();
    r.hooks_path = trae_hooks_path.u8string();
    r.qoder_hooks_path = qoder_path_str;
    r.cursor_hooks_path = cursor_path_str;
    r.codex_hooks_path = codex_path_str;
    r.opencode_plugin_path = opencode_path_str;
    r.needs_enable = true;
    return r;
}

InstalledStatus isHooksInstalled() {
    const fs::path home = homeDir();
    const fs::path trae_hooks_path = fs::path(home) / ".trae-cn" / "hooks.json";
    // Qoder 双配置目录：国际版 ~/.qoder、CN 版 ~/.qoder-cn，任一接线即算已装
    const fs::path cursor_hooks_path = fs::path(home) / ".cursor" / "hooks.json";
    const fs::path codex_hooks_path = codexHome() / "hooks.json";
    const fs::path opencode_plugin_path =
        fs::path(home) / bc::kOpencodePluginSubdir / bc::kOpencodePluginFilename;
    const fs::path bridge_path = fs::path(home) / ".dutyon" / "hooks" / bridgeFilename();

    InstalledStatus st;
    st.bridge_exists = fs::exists(bridge_path);

    auto contains_pet = [](const fs::path& p) {
        auto content = readFileIfExists(p);
        if (!content) return false;
        const auto config = json::parse(*content, nullptr, false);
        if (!config.is_object() || !config.contains("hooks") || !config["hooks"].is_object())
            return false;
        for (const auto& arr : config["hooks"]) {
            if (!arr.is_array()) continue;
            for (const auto& group : arr) {
                if (isPetHook(group)) return true;
                if (!group.is_object() || !group.contains("hooks") || !group["hooks"].is_array())
                    continue;
                for (const auto& handler : group["hooks"]) if (isPetHook(handler)) return true;
            }
        }
        return false;
    };
    st.hooks_exist = contains_pet(trae_hooks_path);
    st.qoder_hooks_exist = contains_pet(fs::path(home) / ".qoder" / "settings.json") ||
                           contains_pet(fs::path(home) / ".qoder-cn" / "settings.json");
    st.cursor_hooks_exist = contains_pet(cursor_hooks_path);
    st.codex_hooks_exist = contains_pet(codex_hooks_path);
    // OpenCode 无 hook 配置；"已安装" = 我们的插件文件存在且带 DutyOn 标记
    // （而非任何同名文件）
    if (auto content = readFileIfExists(opencode_plugin_path)) {
        st.opencode_plugin_exist =
            content->find(kOpencodePluginMarker) != std::string::npos;
    }

    st.installed = st.bridge_exists &&
                   (st.hooks_exist || st.qoder_hooks_exist || st.cursor_hooks_exist ||
                    st.codex_hooks_exist || st.opencode_plugin_exist);
    return st;
}

} // namespace dutyon::backend
