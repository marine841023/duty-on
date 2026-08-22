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

std::string homeDir() {
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    return home ? std::string(home) : std::string();
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
    return "powershell -NoProfile -ExecutionPolicy Bypass -File "
           "\"$env:USERPROFILE\\.dutyon\\hooks\\trae-hook-bridge.ps1\" -Ide " +
           ide + event_arg;
}

// 命令是否属于本应用（同时匹配 .dutyon 与旧名 .trae-pet，改名后重装
// 去重替换而非并排叠加）
bool isPetCommand(const std::string& cmd) {
    return cmd.find(".dutyon") != std::string::npos ||
           cmd.find(".trae-pet") != std::string::npos;
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
    backup.replace_filename(path.filename().string() + ".dutyon-backup-" + std::to_string(ts));
    std::error_code ec;
    fs::copy_file(path, backup, ec);
    if (ec) return std::nullopt;
    return backup.string();
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
                const std::string bak = backupFile(path).value_or("<backup failed>");
                warning = path.string() + (parsed.is_discarded()
                                               ? ": not valid JSON; backed up to "
                                               : ": root was not a JSON object; backed up to ") +
                          bak + " and rebuilt";
            }
        }
    }

    if (strip_version) {
        existing.erase("version");  // Codex CLI 拒绝 version 字段（unknown field）
    } else if (add_version && !existing.contains("version")) {
        existing["version"] = 1;
    }
    if (existing.contains("hooks") && !existing["hooks"].is_object()) {
        const std::string bak = backupFile(path).value_or("<backup failed>");
        const std::string note = path.string() + ": \"hooks\" was not a JSON object; backed up to " +
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
            json hook = {{"type", "command"}, {"command", cmd},
                         {"timeout", bc::kBridgeTimeoutSec}};
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
                warning = path.string() + ": event \"" + event +
                          "\" had a non-array value; preserved it inside the merged array";
            }
        }

        // 去重：移除已有宠物条目（重装不叠加；旧 .trae-pet 条目一并替换）
        json kept = json::array();
        for (const auto& e : arr) {
            bool is_pet = false;
            if (format == HookFormat::Nested) {
                if (e.contains("hooks") && e["hooks"].is_array()) {
                    for (const auto& h : e["hooks"]) {
                        if (h.contains("command") && h["command"].is_string() &&
                            isPetCommand(h["command"].get<std::string>())) {
                            is_pet = true;
                            break;
                        }
                    }
                }
            } else {
                is_pet = e.contains("command") && e["command"].is_string() &&
                         isPetCommand(e["command"].get<std::string>());
            }
            if (!is_pet) kept.push_back(e);
        }
        kept.push_back(std::move(group));
        arr = std::move(kept);
    }

    if (!writeFile(path, existing.dump(2) + "\n")) {
        err = "Failed to write " + path.string();
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
// 事件在那里触发）。事件映射与 Rust 版逐字一致 —— 这是已在 OpenCode 现场
// 验证过的字段路径，勿改动。
const char* kOpencodePluginSource = R"dutyon_src(// DutyOn (开工啦) bridge plugin for OpenCode.
// Auto-generated by the DutyOn desktop pet. Subscribes to OpenCode lifecycle
// events and POSTs canonical hook events to DutyOn's local HTTP server
// (http://127.0.0.1:17521/hook) so the pet reflects your OpenCode session
// state (idle/working/alert) in real time.
//
// OpenCode auto-loads JS plugins from ~/.config/opencode/plugins/ at startup,
// so if OpenCode is already running, restart it after installing. Safe to
// delete — re-installing from DutyOn recreates this file.
//
// Why a plugin (not a config hook): OpenCode has no Claude-Code-style shell
// hook config; its only extension point is a JS plugin on the event bus. The
// `export default { id, server }` shape registers the `event` hook on
// OpenCode's server side, where session/message/permission events fire.

import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'

const ENDPOINT = 'http://127.0.0.1:17521/hook'
const IDE = 'opencode'
const LOG_DIR = path.join(os.homedir(), '.dutyon', 'hooks')
const LOG_PATH = path.join(LOG_DIR, 'opencode-bridge.log')

function log(msg) {
  try {
    fs.mkdirSync(LOG_DIR, { recursive: true })
    try {
      const lines = fs.readFileSync(LOG_PATH, 'utf8').split('\n')
      if (lines.length > 500) fs.writeFileSync(LOG_PATH, lines.slice(-250).join('\n'))
    } catch (e) {}
    const ts = new Date().toISOString().replace('T', ' ').slice(0, 19)
    fs.appendFileSync(LOG_PATH, '[' + ts + '] ' + msg + '\n')
  } catch (e) {}
}

async function post(payload) {
  try {
    const ctrl = new AbortController()
    const timer = setTimeout(() => ctrl.abort(), 2000)
    const res = await fetch(ENDPOINT, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
      signal: ctrl.signal,
    })
    clearTimeout(timer)
    log('POST ' + payload.hook_event_name + ' -> ' + (res ? res.status : '?'))
  } catch (e) {
    log('POST ' + payload.hook_event_name + ' failed: ' + ((e && e.message) || e))
  }
}

function sid(raw) {
  return raw ? 'opencode-' + raw : 'opencode-session'
}

function projectName(dir) {
  if (!dir) return ''
  const parts = String(dir).replace(/\\/g, '/').split('/').filter(Boolean)
  return parts[parts.length - 1] || ''
}

function makeHooks() {
  // messageID -> { role, sessionID }, populated from message.updated. Needed
  // because message.part.updated (text) carries messageID, not sessionID.
  const messageRoles = new Map()
  // OpenCode session id -> cwd, captured from session.created/updated so later
  // events (which may omit the directory) still report the right project.
  const sessionCwd = new Map()

  return {
    event: async ({ event }) => {
      const type = event && event.type
      const p = (event && event.properties) || {}
      try {
        if (type === 'session.created' && p.info && p.info.id) {
          const cwd = p.info.directory || ''
          if (cwd) sessionCwd.set(p.info.id, cwd)
          await post({ hook_event_name: 'SessionStart', session_id: sid(p.info.id), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd) })
          return
        }
        if (type === 'session.updated' && p.info && p.info.id) {
          if (p.info.directory) sessionCwd.set(p.info.id, p.info.directory)
          if (p.info.time && p.info.time.archived) {
            const cwd = sessionCwd.get(p.info.id) || ''
            await post({ hook_event_name: 'Stop', session_id: sid(p.info.id), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd) })
          }
          return
        }
        if (type === 'session.deleted' && p.info && p.info.id) {
          const cwd = sessionCwd.get(p.info.id) || ''
          await post({ hook_event_name: 'Stop', session_id: sid(p.info.id), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd) })
          return
        }
        // session.status(idle) = the agent finished its turn -> Stop.
        if (type === 'session.status' && p.sessionID && p.status && p.status.type === 'idle') {
          const cwd = sessionCwd.get(p.sessionID) || ''
          await post({ hook_event_name: 'Stop', session_id: sid(p.sessionID), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd) })
          return
        }
        // Track message role/session so the text-part event below can resolve
        // its session. Cap the map to avoid unbounded growth across long runs.
        if (type === 'message.updated' && p.info && p.info.id && p.info.sessionID) {
          messageRoles.set(p.info.id, { role: p.info.role, sessionID: p.info.sessionID })
          if (messageRoles.size > 300) messageRoles.delete(messageRoles.keys().next().value)
          return
        }
        // User-submitted prompt text -> the agent is now working (thinking).
        if (type === 'message.part.updated' && p.part && p.part.messageID && p.part.type === 'text') {
          const meta = messageRoles.get(p.part.messageID)
          if (!meta) return
          if (meta.role === 'user' && p.part.text) {
            const cwd = sessionCwd.get(meta.sessionID) || ''
            await post({ hook_event_name: 'UserPromptSubmit', session_id: sid(meta.sessionID), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd) })
          }
          return
        }
        // Tool calls arrive as message parts with type=tool; map running/
        // completed/error to PreToolUse/PostToolUse (keeps the session Working;
        // session.status(idle) later returns it to Idle).
        if (type === 'message.part.updated' && p.part && p.part.sessionID && p.part.type === 'tool') {
          const cwd = sessionCwd.get(p.part.sessionID) || ''
          const toolName = p.part.tool || 'tool'
          const status = p.part.state && p.part.state.status
          const base = { session_id: sid(p.part.sessionID), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd), tool_name: toolName }
          if (status === 'running' || status === 'pending') {
            await post(Object.assign({}, base, { hook_event_name: 'PreToolUse' }))
          } else if (status === 'completed' || status === 'error') {
            await post(Object.assign({}, base, { hook_event_name: 'PostToolUse' }))
          }
          return
        }
        // Agent needs your input (permission prompt or a direct question).
        if (type === 'permission.asked' && p.sessionID) {
          const cwd = sessionCwd.get(p.sessionID) || ''
          await post({ hook_event_name: 'PermissionRequest', session_id: sid(p.sessionID), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd) })
          return
        }
        if (type === 'question.asked' && p.sessionID) {
          const cwd = sessionCwd.get(p.sessionID) || ''
          await post({ hook_event_name: 'PermissionRequest', session_id: sid(p.sessionID), ide: IDE, cwd: cwd, project_path: cwd, project_name: projectName(cwd), tool_name: 'AskUserQuestion' })
        }
      } catch (e) {
        log('event ' + type + ' failed: ' + ((e && e.message) || e))
      }
    },
  }
}

export default {
  id: 'dutyon',
  server: async () => makeHooks(),
}
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
        if (fs::exists(c / bridgeFilename())) return c.string();
    }
    return candidates.empty() ? std::string() : candidates.front().string();
}

InstallResult installHooks(const std::string& hooks_source_dir) {
    const std::string home = homeDir();
    const fs::path bridge_src = fs::path(hooks_source_dir) / bridgeFilename();
    if (!fs::exists(bridge_src)) {
        return fail("Bridge script not found: " + bridge_src.string());
    }

    const fs::path target_hook_dir = fs::path(home) / ".dutyon" / "hooks";
    std::error_code ec;
    fs::create_directories(target_hook_dir, ec);
    if (ec) return fail("Failed to create hook dir: " + ec.message());

    // 复制桥接脚本
    if (auto content = readFileIfExists(bridge_src)) {
        if (!writeFile(target_hook_dir / bridgeFilename(), *content)) {
            return fail("Failed to copy bridge: " + (target_hook_dir / bridgeFilename()).string());
        }
    } else {
        return fail("Failed to read bridge: " + bridge_src.string());
    }

    // 单独的安装脚本（存在则一并复制，不存在跳过）
    const fs::path installer_src = fs::path(hooks_source_dir) / "install-hooks.ps1";
    if (auto content = readFileIfExists(installer_src)) {
        writeFile(target_hook_dir / "install-hooks.ps1", *content);
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

    // ---- Qoder: ~/.qoder/settings.json（已安装才接线；shell 字段）----
    std::optional<std::string> qoder_path_str;
    const fs::path qoder_dir = fs::path(home) / ".qoder";
    if (fs::exists(qoder_dir)) {
        const fs::path qoder_hooks_path = qoder_dir / "settings.json";
        const std::string qoder_cmd = hookCommand("qoder", "");
        std::string err;
        auto w = mergeHooksIntoFile(
            qoder_hooks_path, bc::kQoderHookEvents, bc::kQoderHookEventsCount,
            [&](const char*) { return qoder_cmd; }, "powershell",
            /*add_version=*/false, /*strip_version=*/false, HookFormat::Nested, err);
        if (err.empty()) {
            if (w.has_value()) warnings.push_back("Qoder: " + *w);
            qoder_path_str = qoder_hooks_path.string();
        } else {
            warnings.push_back("Qoder settings.json merge failed: " + err);
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
            cursor_path_str = cursor_hooks_path.string();
        } else {
            warnings.push_back("Cursor hooks.json merge failed: " + err);
        }
    }

    // ---- Codex: ~/.codex/hooks.json（Nested，剥 version —— CLI 拒绝该字段）----
    std::optional<std::string> codex_path_str;
    const fs::path codex_dir = fs::path(home) / ".codex";
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
            codex_path_str = codex_hooks_path.string();
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
                opencode_path_str = plugin_path.string();
            } else {
                warnings.push_back("OpenCode plugin write failed: " + plugin_path.string());
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
    r.hook_dir = target_hook_dir.string();
    r.hooks_path = trae_hooks_path.string();
    r.qoder_hooks_path = qoder_path_str;
    r.cursor_hooks_path = cursor_path_str;
    r.codex_hooks_path = codex_path_str;
    r.opencode_plugin_path = opencode_path_str;
    r.needs_enable = true;
    return r;
}

InstalledStatus isHooksInstalled() {
    const std::string home = homeDir();
    const fs::path trae_hooks_path = fs::path(home) / ".trae-cn" / "hooks.json";
    const fs::path qoder_hooks_path = fs::path(home) / ".qoder" / "settings.json";
    const fs::path cursor_hooks_path = fs::path(home) / ".cursor" / "hooks.json";
    const fs::path codex_hooks_path = fs::path(home) / ".codex" / "hooks.json";
    const fs::path opencode_plugin_path =
        fs::path(home) / bc::kOpencodePluginSubdir / bc::kOpencodePluginFilename;
    const fs::path bridge_path = fs::path(home) / ".dutyon" / "hooks" / bridgeFilename();

    InstalledStatus st;
    st.bridge_exists = fs::exists(bridge_path);

    auto contains_pet = [](const fs::path& p) {
        auto content = readFileIfExists(p);
        return content.has_value() && isPetCommand(*content);
    };
    st.hooks_exist = contains_pet(trae_hooks_path);
    st.qoder_hooks_exist = contains_pet(qoder_hooks_path);
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
