// IDE 窗口扫描实现 —— ide_scanner.rs Windows 路径 1:1 移植。

#ifdef _WIN32

#include "backend/ide_scanner.h"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <optional>
#include <set>

#include "backend/backend_config.h"

namespace dutyon::backend {

namespace {

// 我们追踪的 IDE 可执行文件名（大小写不敏感，匹配进程镜像最后一段）。
// Qoder CN 版主进程叫 "Qoder CN IDE.exe"，另有伴生进程 "Qoder CN.exe"；
// 国际版为 "Qoder.exe"。
const wchar_t* const kIdeProcessExes[] = {
    L"Qoder.exe", L"Qoder CN.exe", L"Qoder CN IDE.exe", L"Trae CN.exe", L"Trae.exe",
    L"Cursor.exe",
};
constexpr int kIdeProcessExesCount = 6;

// 每个标题请求的硬超时（毫秒）
constexpr UINT kTitleQueryTimeoutMs = 250;

std::string utf16ToUtf8(const wchar_t* ws, size_t len) {
    if (len == 0) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, ws, (int)len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws, (int)len, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring utf8ToUtf16(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trimStr(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char)s[b])) b++;
    while (e > b && std::isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

// 剥离提权后缀：" - Qoder [管理员]" -> " - Qoder"。后缀随系统语言变化，
// 按 "<IDE 名> 后跟单个括号串" 的形状匹配而非枚举各语言翻译。
// 返回剥好的标题（无匹配则原样返回）。
std::string stripPrivilegeSuffix(const std::string& title) {
    const char* suffixes[] = {
        bc::kQoderCnIdeTitleSuffix,  bc::kQoderCnTitleSuffix,
        bc::kQoderTitleSuffix,       bc::kTraeTitleSuffix,
        bc::kTraeCodeTitleSuffix,    bc::kCursorAgentsTitleSuffix,
        bc::kCursorTitleSuffix,      bc::kCursorAgentsTitle,
    };
    for (const char* suffix : suffixes) {
        const size_t ide_end = title.rfind(suffix);
        if (ide_end == std::string::npos) continue;
        const std::string rest = trimStr(title.substr(ide_end + strlen(suffix)));
        if (rest.empty()) return title.substr(0, ide_end + strlen(suffix));
        // 余部必须恰好是一个括号串 "[...]"（内部不得再有 '['）
        if (rest[0] == '[' && rest.back() == ']' &&
            rest.find('[', 1) == std::string::npos) {
            return title.substr(0, ide_end + strlen(suffix));
        }
    }
    return title;
}

// 解析 IDE 窗口标题 -> (项目名, IDE 类型)。
//   Trae:   "<file> - <project> - Trae CN" / " - TraeCode CN"（改名后的新版）
//   Qoder:  "<file> - <project> - Qoder" / " - Qoder CN IDE"（CN 版）
//   Cursor: "<file> - <project> - Cursor" / " - Cursor Agents" /
//           独立 "Cursor Agents"（3.x Agents 面板）
// 无项目打开的通用标题（"Trae CN - Trae CN"）返回 nullopt。
std::optional<std::pair<std::string, IdeKind>> parseTitle(const std::string& raw_title) {
    const std::string title = stripPrivilegeSuffix(raw_title);
    // 独立 Agents 面板标题（精确匹配，两个后缀都匹配不上）
    if (title == bc::kCursorAgentsTitle) {
        return std::make_pair(std::string(bc::kCursorAgentsTitle), IdeKind::Cursor);
    }
    IdeKind ide;
    if (endsWith(title, bc::kTraeTitleSuffix) || endsWith(title, bc::kTraeCodeTitleSuffix)) {
        ide = IdeKind::Trae;
    } else if (endsWith(title, bc::kQoderCnIdeTitleSuffix) ||
               endsWith(title, bc::kQoderCnTitleSuffix) ||
               endsWith(title, bc::kQoderTitleSuffix)) {
        ide = IdeKind::Qoder;
    } else if (endsWith(title, bc::kCursorAgentsTitleSuffix)) {
        ide = IdeKind::Cursor;  // 必须在 " - Cursor" 之前判定
    } else if (endsWith(title, bc::kCursorTitleSuffix)) {
        ide = IdeKind::Cursor;
    } else {
        return std::nullopt;
    }

    // 按 " - " 切段，倒数第二段是项目名
    std::vector<std::string> parts;
    size_t pos = 0;
    while (true) {
        const size_t next = title.find(" - ", pos);
        if (next == std::string::npos) {
            parts.push_back(title.substr(pos));
            break;
        }
        parts.push_back(title.substr(pos, next - pos));
        pos = next + 3;
    }
    if (parts.size() < 2) return std::nullopt;

    // 剥多根工作区后缀："project (工作区)" -> "project"，否则与 hook 上报的
    // 项目名（来自 cwd）不匹配，同一项目会裂成两个会话
    std::string raw = trimStr(parts[parts.size() - 2]);
    static const char* const kWorkspaceSuffixes[] = {
        "(工作区)", "(Workspace)", "(ワークスペース)", "(작업 영역)",
    };
    for (const char* ws : kWorkspaceSuffixes) {
        if (endsWith(raw, ws)) {
            raw = trimStr(raw.substr(0, raw.size() - strlen(ws)));
            break;
        }
    }
    if (raw.empty() || raw == "Trae" || raw == "Trae CN" || raw == "TraeCode CN" ||
        raw == "Qoder" || raw == "Qoder CN" || raw == "Qoder CN IDE" || raw == "Cursor") {
        return std::nullopt;
    }
    return std::make_pair(raw, ide);
}

template <typename StrT>
StrT asciiLowerCopy(StrT s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = (typename StrT::value_type)(c + ('a' - 'A'));
    return s;
}

// 从全部在屏标题中提取 IDE 项目（按项目名去重，先见者定 IDE 类型），
// 字母序输出。
std::vector<DetectedProject> extractIdeProjects(const std::vector<std::string>& titles) {
    std::vector<DetectedProject> out;
    for (const auto& t : titles) {
        auto parsed = parseTitle(t);
        if (!parsed.has_value()) continue;
        const bool dup = std::any_of(out.begin(), out.end(), [&](const DetectedProject& p) {
            return p.name == parsed->first;
        });
        if (!dup) out.push_back(DetectedProject{parsed->first, parsed->second});
    }
    std::sort(out.begin(), out.end(),
              [](const DetectedProject& a, const DetectedProject& b) { return a.name < b.name; });
    return out;
}

// 提到 IDE 但没解析出项目的标题（诊断用：新版本/新语言的标题格式会在这里
// 现形）。截断到 200 字符、最多 8 条。
std::vector<std::string> suspectIdeTitles(const std::vector<std::string>& titles) {
    std::vector<std::string> out;
    for (const auto& t : titles) {
        if (out.size() >= 8) break;
        const std::string low = asciiLowerCopy(t);
        if (low.find("qoder") != std::string::npos || low.find("trae") != std::string::npos ||
            low.find("cursor") != std::string::npos || low.find("codex") != std::string::npos ||
            low.find("opencode") != std::string::npos) {
            out.push_back(t.size() > 200 ? t.substr(0, 200) : t);
        }
    }
    return out;
}

// 窗口属主进程是否为我们追踪的 IDE。true/false = 查询成功有结论；
// nullopt = 查询本身失败（调用方回退到超时保护读，不让查不了的窗口隐身）。
// 纯进程查询、不发窗口消息，对任何窗口都不可能挂死。
std::optional<bool> windowOwnedByIde(HWND hwnd) {
    DWORD pid = 0;
    if (GetWindowThreadProcessId(hwnd, &pid) == 0 || pid == 0) return std::nullopt;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return std::nullopt;
    wchar_t buf[512];
    DWORD len = 512;
    // flags=0 即 Win32 路径格式（PROCESS_NAME_WIN32 的值；该宏并非所有
    // SDK 头文件都提供，直接写字面量）
    const BOOL ok = QueryFullProcessImageNameW(process, 0, buf, &len);
    CloseHandle(process);
    if (!ok) return std::nullopt;
    // 取路径最后一段作为 exe 名
    const std::wstring path(buf, len);
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring exe = slash == std::wstring::npos ? path : path.substr(slash + 1);
    for (int i = 0; i < kIdeProcessExesCount; i++) {
        if (_wcsicmp(exe.c_str(), kIdeProcessExes[i]) == 0) return true;
    }
    return false;
}

// SendMessageTimeoutW 读窗口标题（250ms 硬超时）。超时/空标题返回 nullopt。
std::optional<std::string> readTitleTimeout(HWND hwnd) {
    DWORD_PTR len = 0;
    const LRESULT r1 = SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG,
                                           kTitleQueryTimeoutMs, &len);
    if (r1 <= 0 || len == 0) return std::nullopt;
    std::wstring buf(len + 1, L'\0');
    DWORD_PTR copied = 0;
    const LRESULT r2 = SendMessageTimeoutW(hwnd, WM_GETTEXT, (WPARAM)(len + 1),
                                           (LPARAM)buf.data(), SMTO_ABORTIFHUNG,
                                           kTitleQueryTimeoutMs, &copied);
    if (r2 <= 0 || copied == 0) return std::nullopt;
    return utf16ToUtf8(buf.data(), copied);
}

// ---- EnumWindows 回调共享的标题收集 ----
struct EnumTitlesCtx {
    std::vector<std::string>* titles;
};

BOOL CALLBACK enumProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = (EnumTitlesCtx*)lparam;
    if (!IsWindowVisible(hwnd)) return TRUE;
    // 只向 IDE 属主的窗口要标题（按进程名判定，对其他窗口不发任何消息）。
    // 属主查不到的窗口回退到超时保护读 —— 忙死的非 IDE 窗口（如跑长命令
    // 的 CMD）不可能卡住枚举、扫描循环及其后面的状态管道。
    if (windowOwnedByIde(hwnd) != false) {
        if (auto title = readTitleTimeout(hwnd)) ctx->titles->push_back(std::move(*title));
    }
    return TRUE;  // 继续枚举
}

// ---- focus_project_window 上下文 ----
struct FocusCtx {
    std::string name;        // 目标项目名（UTF-8）
    std::wstring name_wide;  // 小写化的宽字符版本（子串回退用）
    HWND best = nullptr;     // 精确 folder 匹配
    HWND substring = nullptr;  // 标题子串回退
};

BOOL CALLBACK focusProc(HWND hwnd, LPARAM lparam) {
    auto* ctx = (FocusCtx*)lparam;
    if (!IsWindowVisible(hwnd)) return TRUE;
    auto title = readTitleTimeout(hwnd);
    if (!title.has_value()) return TRUE;
    auto parsed = parseTitle(*title);
    if (!parsed.has_value()) return TRUE;
    const std::string& folder = parsed->first;
    if (_stricmp(folder.c_str(), ctx->name.c_str()) == 0) {
        ctx->best = hwnd;
    } else if (!ctx->best && !ctx->substring) {
        const std::wstring title_low = asciiLowerCopy(utf8ToUtf16(*title));
        if (title_low.find(ctx->name_wide) != std::wstring::npos) ctx->substring = hwnd;
    }
    return TRUE;
}

} // namespace

std::pair<std::vector<DetectedProject>, std::vector<std::string>> scanIdeProjects() {
    std::vector<std::string> titles;
    EnumTitlesCtx ctx{&titles};
    EnumWindows(enumProc, (LPARAM)&ctx);
    return {extractIdeProjects(titles), suspectIdeTitles(titles)};
}

CliLiveness scanCliProcesses() {
    CliLiveness lv;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return lv;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            const std::wstring name_low = asciiLowerCopy(std::wstring(pe.szExeFile));
            // codex-relay 是协议转换代理（Responses API -> Chat Completions），
            // 不是 codex CLI 本体。排除它：TUI 退出后运行中的 relay 不能
            // 假装会话还活着。
            if (name_low.find(L"codex") != std::wstring::npos &&
                name_low.find(L"relay") != std::wstring::npos) {
                continue;
            }
            if (name_low.find(L"codex") != std::wstring::npos) lv.codex_alive = true;
            if (name_low.find(L"opencode") != std::wstring::npos) lv.opencode_alive = true;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return lv;
}

bool focusProjectWindow(const std::string& name) {
    FocusCtx ctx;
    ctx.name = name;
    ctx.name_wide = asciiLowerCopy(utf8ToUtf16(name));
    EnumWindows(focusProc, (LPARAM)&ctx);
    const HWND target = ctx.best ? ctx.best : ctx.substring;
    if (!target) return false;
    if (IsIconic(target)) ShowWindowAsync(target, SW_RESTORE);
    return SetForegroundWindow(target) != FALSE;
}

} // namespace dutyon::backend

#endif // _WIN32
