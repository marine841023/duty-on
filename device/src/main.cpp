// Duty On 2.0 — 全平台原生入口
//
// PC (Windows):  GLFW 透明窗口 + OpenGL 3.3 + ImGui 监控面板
// ARM Linux:     EGL/GLES2 framebuffer 直渲 + headless 占位 UI
//
// 两种形态共用同一套：API 轮询 -> 状态机 -> Live2D 渲染 -> UI 叠加。
// 浏览器/WebView 已彻底退出。
//
// PC 端布局对齐 1.x（config.rs / styles.css）：
//   窗口宽 260，角色画布 240x260（左右各留 10px），
//   下方 4px 间距接状态栏（项目列表），再 4px 接系统监控面板；
//   迷你模式 130 宽（画布 120x130，状态栏半宽 120、监控隐藏）；
//   窗口总高随面板内容变化（底边固定向上生长）。
// 用户数据复用 1.x：直接读写 ~/.dutyon/config.json（翻转/迷你/语言/
// 动作设定/监控显隐/当前形象），模型来自 ~/.dutyon/live2d + 内置目录。

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <thread>

// GL 函数声明：PC 用 GLEW（桌面 OpenGL），设备用 GLES3
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>  // SHBrowseForFolderW（相框照片文件夹选择）
#include <commdlg.h>  // GetOpenFileNameW（自定义角色动画上传）
#include <mmsystem.h>  // mciSendStringW（状态音频试听）
#include <GL/glew.h>
#include <GLFW/glfw3.h>  // glfwGetFramebufferSize（视口 DPI 换算）
#else
#include <GLES3/gl3.h>
#endif

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "config.h"
#include "api/client.h"
#include "debug_diag.h"
#include "config/user_config.h"
#include "platform/window.h"
#include "render/gif_sprite.h"
#include "render/live2d_renderer.h"
#include "state/machine.h"
#include "ui/i18n.h"
#include "ui/ui_renderer.h"
#ifdef _WIN32
#include "backend/backend_service.h"  // 单进程后端（原 duty-on.exe 职责）
#include "platform/sync_progress.h"   // 设备程序自动更新进度窗口
#include "platform/pair_code_dialog.h"  // 配对码输入弹窗（验证码风格 6 格）
#else
#include <nlohmann/json.hpp>            // 用户模型 model3.json 解析（PC 同步）
#include "net/wifi_manager.h"         // Wi-Fi 配网 + AP/client 模式切换状态机
#include "net/pc_discovery.h"         // UDP 广播发现 PC（替代 USB ARP 发现）
#include "net/device_identity.h"      // device_id / pair_code / token 持久化
#include "render/photo_player.h"      // 电子相框照片逐张流式播放
#include "ui/task_panel.h"            // 下半屏任务列表（项目名 + 状态）
#include "audio/sound_player.h"       // 事件提示音（开始/结束/提醒）
#endif

using namespace dutyon;
using Clock = std::chrono::steady_clock;

static volatile bool g_running = true;

void signalHandler(int) {
    g_running = false;
}

// GIF 状态动画文件（1.x updateCustomAnimation 回退链：
// 本状态自身 → sleeping → working/alert 任一可用）
static std::string gifFileFor(const CustomCharacter& c, const std::string& state) {
    const std::string* f = nullptr;
    if (state == "sleeping" && !c.sleeping.empty()) f = &c.sleeping;
    else if (state == "working" && !c.working.empty()) f = &c.working;
    else if (state == "alert" && !c.alert.empty()) f = &c.alert;
    if (!f && !c.sleeping.empty()) f = &c.sleeping;
    if (!f && !c.working.empty()) f = &c.working;
    if (!f && !c.alert.empty()) f = &c.alert;
    return f ? *f : std::string();
}

#ifdef _WIN32
// ---------------------------------------------------------------------------
// 设备程序自动更新（连接时触发）：设备连上 PC 后比对「本机待推送源码树的内容
// 哈希」与「设备上报版本」（X-DutyOn-Version，来自 /opt/dutyon/VERSION），不
// 一致则后台跑 .userdata/sync-device.ps1 推送更新，独立置顶窗口显示进度。原
// 「同步程序到设备」菜单项已移除，改为此处全自动。
// ---------------------------------------------------------------------------

// 自动更新并发闸：连接跳变处预判 + launcher 内 exchange 双保险
static std::atomic<bool> g_syncing{false};

// 计算「待推送源码树」的内容哈希（FNV-1a 64 位 → 16 位十六进制）。文件集与
// sync-device.ps1 的 tar 一致，故「哈希变」<=>「推送内容变」<=>「需更新」。
// 仅 PC 端计算（设备只存 PC 下发值，无需跨平台一致）。无文件返回空串。
static std::string computeSourceVersion(const std::string& repo) {
    namespace fs = std::filesystem;
    const char* kRoots[] = {"device/CMakeLists.txt", "device/src",
                            "device/scripts", "frontend/assets/device",
                            ".userdata/deploy.sh"};
    const fs::path base(repo);
    std::vector<fs::path> files;
    for (const char* rel : kRoots) {
        const fs::path p = base / rel;
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        if (fs::is_regular_file(p, ec)) {
            files.push_back(p);
        } else if (fs::is_directory(p, ec)) {
            fs::recursive_directory_iterator it(
                p, fs::directory_options::skip_permission_denied, ec);
            for (fs::recursive_directory_iterator end; !ec && it != end;
                 it.increment(ec)) {
                std::error_code fec;
                if (it->is_regular_file(fec) && !fec) files.push_back(it->path());
            }
        }
    }
    if (files.empty()) return std::string();
    std::sort(files.begin(), files.end());  // 稳定顺序：哈希与枚举序无关
    unsigned long long h = 1469598103934665603ULL;  // FNV-1a offset basis
    auto feed = [&](const void* data, size_t n) {
        const unsigned char* b = static_cast<const unsigned char*>(data);
        for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    };
    for (const auto& f : files) {
        const std::string rel = fs::relative(f, base).generic_string();
        feed(rel.data(), rel.size());
        std::error_code ec;
        const unsigned long long s =
            static_cast<unsigned long long>(fs::file_size(f, ec));
        if (ec) continue;
        feed(&s, sizeof(s));
        std::ifstream in(f, std::ios::binary);
        if (!in) continue;
        char buf[8192];
        while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
            feed(buf, static_cast<size_t>(in.gcount()));
            if (in.gcount() < static_cast<std::streamsize>(sizeof(buf))) break;
        }
    }
    char out[17];
    snprintf(out, sizeof(out), "%016llx", h);
    return std::string(out);
}

// 读同步日志尾部，解析最后一条 PROGRESS=NN 与阶段关键字（ASCII 首词）
static bool parseSyncProgress(const std::wstring& logPath, int& pct,
                              std::string& stageKey) {
    std::ifstream f(logPath, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff sz = f.tellg();
    if (sz <= 0) return false;
    const std::streamoff n = std::min<std::streamoff>(sz, 4096);
    f.seekg(sz - n);
    std::string tail(static_cast<size_t>(n), '\0');
    f.read(&tail[0], n);
    const size_t pos = tail.rfind("PROGRESS=");
    if (pos == std::string::npos) return false;
    size_t i = pos + 9;  // strlen("PROGRESS=")
    int v = 0;
    while (i < tail.size() && tail[i] >= '0' && tail[i] <= '9') {
        v = v * 10 + (tail[i] - '0');
        i++;
    }
    if (v < 0 || v > 100) return false;
    while (i < tail.size() && (tail[i] == ' ' || tail[i] == '\t')) i++;
    size_t j = i;
    while (j < tail.size() && tail[j] != ' ' && tail[j] != '\r' &&
           tail[j] != '\n' && tail[j] != '\t')
        j++;
    pct = v;
    stageKey = tail.substr(i, j - i);
    return true;
}

// 阶段关键字 → 中文进度文案（关键字均为脚本内 ASCII，无需 UTF-8 转换）
static const wchar_t* syncStageLabel(const std::string& k) {
    if (k == "push") return L"推送源码…";
    if (k == "remote") return L"连接设备…";
    if (k == "unpack") return L"解包源码…";
    if (k == "configure") return L"配置构建…";
    if (k == "build") return L"编译中（约 1-5 分钟）…";
    if (k == "deploy") return L"部署到设备…";
    if (k == "restart") return L"重启服务…";
    if (k == "done") return L"完成";
    return L"同步中…";
}

// 后台推送更新：跑 sync-device.ps1 -Version <hash>，独立置顶窗口显示进度。
// 全程在工作线程内（含进度窗口创建/消息泵/销毁），不阻塞渲染主循环。
static void launchDeviceSync(const std::string& repo,
                             const std::string& version) {
    if (g_syncing.exchange(true)) return;  // 已有同步在跑
    std::thread([repo, version]() {
        namespace fs = std::filesystem;
        const fs::path script = fs::path(repo) / ".userdata" / "sync-device.ps1";
        std::error_code ec;
        SyncProgressDialog dlg;
        if (!fs::exists(script, ec)) { g_syncing = false; return; }

        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        const std::wstring log = std::wstring(tmp) + L"dutyon-device-sync.log";
        { std::ofstream clr(log, std::ios::binary | std::ios::trunc); }  // 清旧日志

        const std::wstring repoW(repo.begin(), repo.end());
        const std::wstring verW(version.begin(), version.end());
        const std::wstring scriptW = repoW + L"\\.userdata\\sync-device.ps1";
        std::wstring cmd =
            L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" +
            scriptW + L"\" -Repo \"" + repoW + L"\" -LogFile \"" + log +
            L"\" -Version \"" + verW + L"\"";

        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        dlg.create();
        dlg.update(3, L"正在启动同步…");
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            dlg.update(0, L"无法启动同步脚本");
            Sleep(2500);
            dlg.destroy();
            g_syncing = false;
            return;
        }

        // 轮询：每 200ms 等进程 + 泵消息 + tail 日志解析进度。真实标记到达即
        // 跳进；长阶段（编译）无新标记时按 ~2.4s/1% 缓慢爬升，封顶 95 防假死
        const auto t0 = std::chrono::steady_clock::now();
        const long long kTimeoutMs = 20LL * 60 * 1000;
        int shown = 3, lastLogged = 0, creep = 0;
        std::string stageKey;
        bool timedOut = false;
        while (true) {
            const DWORD wr = WaitForSingleObject(pi.hProcess, 200);
            dlg.pump();
            int p = 0;
            std::string k;
            if (parseSyncProgress(log, p, k) && p >= lastLogged) {
                lastLogged = p;
                if (!k.empty()) stageKey = k;
            }
            if (shown < lastLogged) {
                shown = lastLogged;  // 真实进度：跳到最新标记
            } else if (shown < std::min<int>(lastLogged + 20, 95)) {
                if (++creep >= 12) { creep = 0; shown++; }  // 停滞时缓慢爬升
            }
            dlg.update(shown, syncStageLabel(stageKey));
            if (wr == WAIT_OBJECT_0) break;
            const long long el =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
            if (el > kTimeoutMs) { timedOut = true; break; }
        }
        DWORD rc = 1;
        GetExitCodeProcess(pi.hProcess, &rc);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        // 结果确认：读日志尾部 RESULT=OK（脚本最后一行）
        std::string tail;
        {
            std::ifstream f(log, std::ios::binary);
            if (f) {
                f.seekg(0, std::ios::end);
                const std::streamoff sz = f.tellg();
                const std::streamoff n = std::min<std::streamoff>(sz, 800);
                f.seekg(sz - n);
                tail.resize(static_cast<size_t>(n));
                f.read(&tail[0], n);
            }
        }
        const bool ok = !timedOut && rc == 0 &&
                        tail.find("RESULT=OK") != std::string::npos;
        if (ok) {
            dlg.update(100, L"更新完成，设备已重启");
            printf("[Sync] device update done (version=%s)\n", version.c_str());
        } else {
            dlg.update(shown, timedOut ? L"更新超时，请检查设备连接"
                                       : L"更新失败，请重试或检查设备");
            printf("[Sync] device update FAILED (rc=%lu timedOut=%d)\n", rc,
                   timedOut ? 1 : 0);
        }
        Sleep(2800);  // 结果停留片刻再关窗
        dlg.destroy();
        g_syncing = false;
    }).detach();
}
#endif  // _WIN32

#ifndef _WIN32
// ---------------------------------------------------------------------------
// 用户 Live2D 模型从 PC 同步：PC 下发的 activeCharacter 对用户模型是 HTTP
// 路由键（http://127.0.0.1:17521/live2d/<相对路径>），设备本地无此文件。
// 下载 model3.json 并解析 FileReferences 引用的全部文件（moc3/贴图/物理/
// 动作/表情等），按原目录结构落到本地 ~/.dutyon/live2d/ 后按本地文件加载。
// 已存在的文件跳过（中断后续传）；失败返回 nullopt（由调用方节流重试）。
// ---------------------------------------------------------------------------
static std::optional<ModelEntry> fetchUserModelFromPC(ApiClient& api,
                                                      const std::string& key) {
    static const std::string kPrefix = "http://127.0.0.1:17521/live2d/";
    if (key.rfind(kPrefix, 0) != 0) return std::nullopt;
    const std::string rel = key.substr(kPrefix.size());
    if (rel.empty()) return std::nullopt;
    // 路径安全：拒绝 .. 段（与 PC 端 /live2d 路由校验一致）
    const std::filesystem::path rel_path(rel);
    for (const auto& seg : rel_path)
        if (seg == "..") return std::nullopt;
    const std::filesystem::path user_root(UserConfigStore::userModelsDir());
    const std::filesystem::path json_path = user_root / rel_path;
    if (json_path.filename().string().size() <= 12) return std::nullopt;

    // 1. model3.json 本体
    if (!std::filesystem::exists(json_path) &&
        !api.downloadLive2dFile(rel, json_path.generic_string())) {
        fprintf(stderr, "[Model] fetch failed: %s\n", rel.c_str());
        return std::nullopt;
    }

    // 2. 解析 FileReferences 收集引用文件（相对模型目录）。
    //    required = moc3/贴图（渲染必需，缺失判整体失败重下）；
    //    物理/表情/动作/声音等可选，缺失仅告警（Cubism 容忍）
    std::vector<std::pair<std::string, bool>> refs;
    {
        std::ifstream in(json_path);
        nlohmann::json j;
        try {
            in >> j;
        } catch (...) {
            // 本地文件截断残留：删除让下次重试重新下载
            std::error_code ec;
            std::filesystem::remove(json_path, ec);
            return std::nullopt;
        }
        auto add = [&refs](const nlohmann::json& v, bool required) {
            if (v.is_string()) {
                const std::string s = v.get<std::string>();
                if (!s.empty()) refs.emplace_back(s, required);
            }
        };
        if (j.contains("FileReferences") && j["FileReferences"].is_object()) {
            const auto& fr = j["FileReferences"];
            if (fr.contains("Moc")) add(fr["Moc"], true);
            for (const char* k : {"Physics", "Pose", "UserData",
                                  "DisplayInfo"})
                if (fr.contains(k)) add(fr[k], false);
            if (fr.contains("Textures") && fr["Textures"].is_array())
                for (const auto& t : fr["Textures"]) add(t, true);
            if (fr.contains("Expressions") && fr["Expressions"].is_array())
                for (const auto& e : fr["Expressions"])
                    if (e.is_object() && e.contains("File"))
                        add(e["File"], false);
            if (fr.contains("Motions") && fr["Motions"].is_object())
                for (auto it = fr["Motions"].begin(); it != fr["Motions"].end();
                     ++it)
                    if (it.value().is_array())
                        for (const auto& m : it.value())
                            if (m.is_object()) {
                                if (m.contains("File")) add(m["File"], false);
                                if (m.contains("Sound")) add(m["Sound"], false);
                            }
        }
        std::sort(refs.begin(), refs.end());
        refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
    }

    // 3. 逐个下载引用文件（已存在跳过；引用可向上级相对，但必须落在
    //    用户模型根内，与 PC 路由的 .. 拒绝策略一致）
    const std::filesystem::path model_dir = json_path.parent_path();
    int missing_opt = 0;
    for (const auto& [ref, required] : refs) {
        const std::filesystem::path local =
            (model_dir / std::filesystem::path(ref)).lexically_normal();
        const std::string rel_ref =
            local.lexically_relative(user_root).generic_string();
        if (rel_ref.empty() || rel_ref.rfind("..", 0) == 0 ||
            (!std::filesystem::exists(local) &&
             !api.downloadLive2dFile(rel_ref, local.generic_string()))) {
            fprintf(stderr, "[Model] fetch ref failed: %s\n", rel_ref.c_str());
            if (required) {
                // 必需文件缺失：删 model3.json 让下轮重试整套
                //（调用方节流），避免留下永远加载不了的半残模型
                std::error_code ec;
                std::filesystem::remove(json_path, ec);
                return std::nullopt;
            }
            ++missing_opt;
        }
    }
    if (missing_opt > 0)
        fprintf(stderr, "[Model] %s: %d optional ref(s) missing\n",
                rel.c_str(), missing_opt);

    ModelEntry e;
    e.json = json_path.filename().string();
    e.name = e.json.substr(0, e.json.size() - 12);
    e.dir = model_dir.generic_string();
    e.key = key;  // 与 PC 同键，后续轮询/重启直接命中本地
    e.builtin = false;
    return e;
}
#endif

// ---------------------------------------------------------------------------
// 崩溃诊断：未处理异常时打印出错地址与所在模块（区分自身 bug / 驱动崩溃）
// ---------------------------------------------------------------------------
#ifdef _WIN32
#include <psapi.h>
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    const void* addr = ep->ExceptionRecord->ExceptionAddress;
    fprintf(stderr, "[CRASH] code=0x%08lX addr=%p\n",
            ep->ExceptionRecord->ExceptionCode, addr);
    HMODULE mods[256];
    DWORD needed = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) {
        const DWORD n = needed / sizeof(HMODULE);
        for (DWORD i = 0; i < n && i < 256; i++) {
            MODULEINFO mi;
            char name[MAX_PATH];
            if (GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi)) &&
                GetModuleFileNameA(mods[i], name, MAX_PATH)) {
                const auto* base = static_cast<const BYTE*>(mi.lpBaseOfDll);
                if (reinterpret_cast<const BYTE*>(addr) >= base &&
                    reinterpret_cast<const BYTE*>(addr) < base + mi.SizeOfImage) {
                    fprintf(stderr, "[CRASH] module: %s + 0x%tx\n", name,
                            reinterpret_cast<const BYTE*>(addr) - base);
                    break;
                }
            }
        }
    }
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int main() {
    // stdout/stderr 无缓冲（重定向到文件时也实时可见）
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

#ifdef _WIN32
    if (getenv("DUTYON_FT_PROBE") && getenv("DUTYON_FT_PROBE")[0] == '1')
        dutyon::FtProbe();

    // 单实例互斥：已有实例在运行则提示后退出。
    // 背景：曾因双实例共存出故障 —— 两个进程靠 SO_REUSEADDR 同时绑定
    // 17521 端口，IDE hook 事件被随机分流到不同实例，状态撕裂导致桌宠
    // "没反应"。句柄保持到进程退出由 OS 自动释放。
    const HANDLE kSingleInstanceMutex =
        CreateMutexA(nullptr, TRUE, "DutyOn.SingleInstance");
    if (kSingleInstanceMutex != nullptr &&
        GetLastError() == ERROR_ALREADY_EXISTS) {
        // 必须 MessageBoxW：项目按 /utf-8 编译，窄字符串走 ANSI(GBK) 解码会
        // 把 UTF-8 中文渲染成乱码
        MessageBoxW(nullptr,
                    L"DutyOn 已经在运行中，请勿重复启动。\r\n\r\n"
                    L"DutyOn is already running.\r\n"
                    L"Check the desktop for the existing pet window.",
                    L"DutyOn", MB_OK | MB_ICONINFORMATION);
        CloseHandle(kSingleInstanceMutex);
        return 0;
    }
    // nullptr（创建失败，如权限异常）时失败放行：宁可双实例也不误拒启动
#endif

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
#ifdef _WIN32
    SetUnhandledExceptionFilter(CrashHandler);
#endif

#ifdef _WIN32
    const char* platform = "Windows (GLFW/OpenGL)";
    const char* api_url = "http://127.0.0.1:17521";
    // 尺寸对齐 1.x config.rs / styles.css（逻辑像素）：
    //   窗口 260 / 画布 240x260（左右各留 10）
    //   迷你：窗口 130 / 画布 120x130（左右各留 5）/ 状态栏半宽 120
    // 1.x 是 WebView：260 CSS px @125% DPI = 325 物理 px。原生窗口必须乘以
    // 监视器缩放，否则整体比 1.x 小一圈（角色/字体/面板全偏小）。
    constexpr int BASE_WIN_W = 260;
    constexpr int BASE_CANVAS_W = 240;
    constexpr int BASE_MODEL_AREA_H = 260;
    constexpr int BASE_MINI_WIN_W = 130;
    constexpr int BASE_MINI_CANVAS_W = 120;
    constexpr int BASE_MINI_MODEL_AREA_H = 130;
    constexpr int FPS = 30;
    // 联合缩放（窗口创建后按实际监视器取值，跨显示器拖动时动态重算）：
    //   ui_scale = DPI × clamp(显示器高度 / 1440)
    // 纯 DPI 缩放下，260px 窗口在 1080p 屏占屏 24%、在 1440p 屏仅 18%，
    // 低分辨率屏上角色明显偏大 —— 以 1440p（标定的合适尺寸）为参考按
    // 显示器高度归一，保证各分辨率下占屏比例一致
    float ui_scale = 1.0f;
    int win_w = BASE_WIN_W, canvas_w = BASE_CANVAS_W,
        model_area_h = BASE_MODEL_AREA_H;
    int mini_win_w = BASE_MINI_WIN_W, mini_canvas_w = BASE_MINI_CANVAS_W,
        mini_model_area_h = BASE_MINI_MODEL_AREA_H;
    const int initial_h = BASE_MODEL_AREA_H + 90;  // 状态栏首帧高度估算，之后自动校正
#else
    const char* platform = "ARM Linux (EGL/GLES2)";
    const char* api_url = "Wi-Fi 局域网（UDP 广播自动发现）";
    // 直出模式：逻辑尺寸 = 实际选中的 DRM 模式尺寸（竖屏 480x800 /
    // 横屏 800x480），init 后从 window 取回，布局全按实际尺寸自适应
    int WIN_W = kDisplayWidth;
    int WIN_H = kDisplayHeight;
    // 竖屏上下对半：上半屏角色，下半屏任务列表（面板高度按内容自适应）
    int MODEL_AREA_H = WIN_H / 2;
    constexpr int FPS = kTargetFps;
    const int initial_h = kDisplayHeight;
#endif

    printf("Duty On 2.0 — %s\n", platform);
    printf("API: %s | Window: %dx%d @ %dfps\n", api_url,
#ifdef _WIN32
           BASE_WIN_W, initial_h,
#else
           WIN_W, initial_h,
#endif
           FPS);

    // 1. 平台窗口（PC: GLFW 透明窗口 / 设备: EGL framebuffer）
    IPlatformWindow* window = createPlatformWindow();
    if (!window->init(
#ifdef _WIN32
            BASE_WIN_W, initial_h
#else
            WIN_W, initial_h
#endif
            )) {
        fprintf(stderr, "Window init failed\n");
        delete window;
        return 1;
    }

#ifndef _WIN32
    // 直出模式：逻辑尺寸回填为实际选中的 DRM 模式尺寸（init 内已按
    // 480x800 优先匹配，无匹配模式时取首个模式 800x480）。fb 与 mode
    // 尺寸一致 setCrtc 才不被拒；布局按实际尺寸自适应
    WIN_W = window->width();
    WIN_H = window->height();
    MODEL_AREA_H = WIN_H / 2;
    printf("[Mode] display %dx%d (direct)\n", WIN_W, WIN_H);
#endif

#ifdef _WIN32
    // 位置记忆恢复（1.x windowPosition 字段复用）：init() 默认放右下角，
    // 有有效记忆时移回上次位置（placeAt 自带在屏校验，屏外坐标放弃恢复）。
    // 坐标留存：首帧布局把窗口高度从估算值校正为实际值后重放一次（见
    // 主循环 restore replay），用最终高度把顶边夹回可见区
    int restore_x = 0, restore_bottom = 0;
    bool has_restore = false;
    {
        UserConfig pcfg = UserConfigStore::load();
        if (pcfg.has_window_pos) {
            restore_x = pcfg.win_x;
            restore_bottom = pcfg.win_y;
            has_restore = true;
            window->placeAt(restore_x, restore_bottom);
        }
    }
#endif

#ifdef _WIN32
    // 按窗口所在监视器重算联合缩放（DPI × 分辨率归一）；尺寸变化时
    // 更新全部布局变量并返回 true（调用方在 UI 就绪后补 ui.setScale）
    auto recomputeScale = [&]() -> bool {
        GLFWwindow* gw = static_cast<GLFWwindow*>(window->nativeHandle());
        float xs = 1.0f, ys = 1.0f;
        glfwGetWindowContentScale(gw, &xs, &ys);
        if (xs < 0.5f || xs > 4.0f) xs = ui_scale;  // 取不到时沿用当前值
        int mon_w = 0, mon_h = 1440;
        window->monitorSize(mon_w, mon_h);
        if (mon_h < 600) mon_h = 1440;              // 异常值按参考分辨率
        float f = (float)mon_h / 1440.0f;
        // 低分屏下限 1.0（不再缩小）：归一化只用于高分屏放大。曾试过
        // 0.85 下限，1080p@100% 副屏上窗口/角色/字体比 1.x（纯 DPI 260px）
        // 还小，且从主屏（1.25）拖过去视觉骤缩 32%，四档字号只有
        // 13/12/11/10px、几乎全走点阵光栅，用户反馈"人物和字体特别小特别
        // 丑"。钳到 1.0 后 ≤1440p 的屏与 1.x 尺寸完全一致，观感连续。
        if (f < 1.0f) f = 1.0f;
        if (f > 2.0f) f = 2.0f;
        const float s = xs * f;
        if (s > ui_scale - 0.01f && s < ui_scale + 0.01f) return false;
        ui_scale = s;
        win_w = (int)(BASE_WIN_W * s);
        canvas_w = (int)(BASE_CANVAS_W * s);
        model_area_h = (int)(BASE_MODEL_AREA_H * s);
        mini_win_w = (int)(BASE_MINI_WIN_W * s);
        mini_canvas_w = (int)(BASE_MINI_CANVAS_W * s);
        mini_model_area_h = (int)(BASE_MINI_MODEL_AREA_H * s);
        printf("[Scale] monitor %dx%d dpi %.2f -> ui_scale %.2f (window %dx%d)\n",
               mon_w, mon_h, xs, s, win_w, model_area_h);
        return true;
    };
    recomputeScale();
    // 诊断：进程 DPI 感知状态 + GLFW 窗口/帧缓冲实际尺寸 + Win32 rect
    {
        GLFWwindow* gw = static_cast<GLFWwindow*>(window->nativeHandle());
        int ww = 0, wh = 0, fw = 0, fh = 0;
        glfwGetWindowSize(gw, &ww, &wh);
        glfwGetFramebufferSize(gw, &fw, &fh);
        const BOOL dpi_aware = IsProcessDPIAware();
        RECT rc;
        GetWindowRect(static_cast<HWND>(window->nativeWinHandle()), &rc);
        printf("[Diag] dpiAware=%d glfwWin=%dx%d framebuffer=%dx%d win32Rect=(%ld,%ld)-(%ld,%ld) exStyle=0x%lX\n",
               (int)dpi_aware, ww, wh, fw, fh, rc.left, rc.top, rc.right, rc.bottom,
               GetWindowLong(static_cast<HWND>(window->nativeWinHandle()), GWL_EXSTYLE));
    }
#endif

    // 2. Cubism Framework（进程级一次）
    if (!Live2DRenderer::frameworkInit()) {
        fprintf(stderr, "Cubism Framework init failed\n");
        window->shutdown();
        delete window;
        return 1;
    }

    // 3. 用户配置（复用 1.x ~/.dutyon/config.json）——语言须在 UI 字体构建前生效
    UserConfig cfg = UserConfigStore::load();
    if (!cfg.language.empty()) I18n::setLang(cfg.language);

    // 4. 角色：自定义 GIF 形象（1.x customCharacters，动画在
    //    ~/.dutyon/animations/）或 Live2D 模型（内置 + 用户目录）。
    //    activeCharacterId 语义同 1.x：模型 URL 或 "char_xxx"。
    Live2DRenderer renderer;
    GifSprite gif;                          // 自定义 GIF 形象渲染器
    bool using_gif = false;                 // 当前形象是否为自定义 GIF
    const CustomCharacter* gif_char = nullptr;  // 当前自定义形象定义
    std::vector<std::string> builtin_roots;
#ifdef _WIN32
    {
        char exe_path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        const std::filesystem::path exe_dir =
            std::filesystem::path(exe_path).parent_path();
        // 发布包：<exe>/assets/live2d；开发运行：仓库根 frontend/assets/live2d
        builtin_roots.push_back((exe_dir / "assets" / "live2d").string());
        builtin_roots.push_back(
            (exe_dir / ".." / ".." / ".." / "frontend" / "assets" / "live2d")
                .lexically_normal()
                .string());
    }
#else
    // 设备端：固定部署目录（见 config.h kModelDir），布局同内置模型根，
    // 让 listModels 统一扫描（含 *.model3.json 枚举），无需用户目录。
    builtin_roots.push_back(kModelDir);
#endif

    std::vector<ModelEntry> model_entries = UserConfigStore::listModels(builtin_roots);
    std::string current_model_key;  // 1.x activeCharacterId（Live2D 模型唯一键）
#ifndef _WIN32
    // 最近一次状态动作（模型热切换后重放，避免新模型停在待机）
    std::string dev_motion_group;
    int dev_motion_idx = 0;
    // PC「动作设定」下发的状态动作覆盖（/api/status stateMotions，按当前
    // 角色过滤）；优先于设备本地 stateMotions，断连后保持最近值
    std::map<std::string, std::pair<std::string, int>> pc_state_motions;
    // 欢迎信号序号基准（/api/status welcomeSeq）：增大即播一次欢迎
    long long dev_welcome_prev = 0;
    // 硬件屏布局模式（PC 经 /api/status 下发；断连后保持最近值）：
    // multi=角色半屏+任务列表 / single=角色全屏+大时钟 / frame=相框全屏轮播。
    // 初始 frame：未连上 PC 时当电子相框用（连上后 PC 下发模式覆盖）
    std::string device_mode = "frame";
    // 时钟颜色主题（PC 菜单下发；断连保持最近值）
    std::string clock_color = "amber";
    // 屏幕亮度（PC 菜单"设备→亮度"下发，10-100）：有 sysfs 背光写背光，
    // 否则渲染层整屏压暗（renderDim）
    int device_brightness = 100;
    // 整屏旋转角（PC 菜单"设备→屏幕旋转"下发，0/90/180/270）：设备端逻辑
    // FBO + 旋转合成；断连后保持最近值
    int screen_rotation = 0;
#ifndef _WIN32
    // 默认朝向 = 上次运行时保存的旋转角（持久化于 ~/.dutyon/config.json 的
    // screenRotation，见下方 PC 下发时的 saveScreenRotation）：开机即恢复，
    // 无需等 PC 重新连上再下发——满足"默认显示方向与上次断开时一致"。
    // DUTYON_FORCE_ROTATION=<deg> 仍可覆盖（无 PC 连接时供快照验证
    // 0/90/180/270；正常发布无副作用，PC 下发会覆盖）。
    screen_rotation = cfg.screen_rotation;
    if (const char* rot_env = getenv("DUTYON_FORCE_ROTATION")) {
        if (*rot_env) screen_rotation = atoi(rot_env);
    }
    if (screen_rotation != 0) {
        window->setRotation(screen_rotation);
        WIN_W = window->width();
        WIN_H = window->height();
        MODEL_AREA_H = WIN_H / 2;
        printf("[Mode] startup rotation -> %d (%dx%d)\n", screen_rotation,
               WIN_W, WIN_H);
    }
#endif
    // 提示音边沿检测基准（首帧仅记录不发声，避免开机误报）
    std::string snd_prev_overall;
    bool snd_prev_confirm = false;
    bool snd_seen = false;
    // 相框模式轮播状态：动作列表签名（形象变化重建）+ 当前序号 + 计时
    std::string frame_sig;
    int frame_idx = 0;
    float frame_timer = 0.f;
    // 相框播放源（PC 经 /api/status 下发 frameSource；断连保持最近值）：
    // motion=动作轮播（现行）/ folder=指定文件夹照片（PhotoPlayer 逐张流式）
    std::string frame_source = "motion";
    bool photo_playing = false;   // PhotoPlayer 后台线程是否在跑
    bool photo_showing = false;   // 本帧是否正显示照片（true 时隐藏角色/时钟）
    // 时钟跟随 PC（设备无 RTC/NTP 不可信）：轮询到的 PC epoch 秒 +
    // steady_clock 基准，两次轮询间自行推进
    double clock_epoch = 0;
    std::chrono::steady_clock::time_point clock_sync_tp{};
#endif
    {
        // 自定义形象优先（1.x getCharacters：active 匹配 custom id）
        for (const auto& c : cfg.custom_characters) {
            if (c.id == cfg.active_character_id && !c.id.empty()) {
                using_gif = true;
                gif_char = &c;
                const std::string file =
                    gifFileFor(c, "sleeping");  // 初始状态 sleeping
                if (!file.empty() &&
                    gif.load(UserConfigStore::animationsDir() + "/" +file)) {
                    printf("GIF character: %s (%s)\n", c.name.c_str(), c.id.c_str());
                } else {
                    using_gif = false;
                    gif_char = nullptr;
                    fprintf(stderr, "GIF character load failed — 回退 Live2D\n");
                }
                break;
            }
        }
        if (!using_gif) {
            const ModelEntry* active = nullptr;
            // 优先配置里的当前形象；否则默认模型名；否则目录里第一个
            if (!cfg.active_character_id.empty())
                for (const auto& e : model_entries)
                    if (e.key == cfg.active_character_id) { active = &e; break; }
            if (!active)
                for (const auto& e : model_entries)
                    if (e.name == kDefaultModel) { active = &e; break; }
            if (!active && !model_entries.empty()) active = &model_entries.front();
            if (active && renderer.loadModelFile(active->dir, active->json)) {
                current_model_key = active->key;
                printf("Model: %s (%s)\n", active->name.c_str(), active->key.c_str());
            } else {
                fprintf(stderr, "Model load failed — 继续运行（无角色）\n");
            }
        }
    }
#ifndef _WIN32
    // 设备端无用户目录：固定模型目录 + 默认模型
    if (!renderer.isLoaded() && renderer.loadModel(kModelDir, kDefaultModel))
        current_model_key = kDefaultModel;
#endif

    // 5. UI 叠加层（PC: ImGui / 设备: 占位）
    UIRenderer ui;
#ifdef _WIN32
    ui.init(static_cast<GLFWwindow*>(window->nativeHandle()));
    ui.setScale(ui_scale);  // init 内部只取了 DPI，补上分辨率归一系数
#else
    ui.init(WIN_W, MODEL_AREA_H);

    // Wi-Fi 配对码方案（替代 USB 直连）：
    //   wifi          AP 配网 <-> 入网 状态机（每帧 poll，内部 1s 节流）；
    //   pc_discovery  入网后 UDP 广播发现 PC，拿 base url 喂 ApiClient；
    //   identity      device_id / pair_code / token（~/.dutyon/device.json）。
    // pc_ready = 已入网 + 已配对 + 已发现 PC（= 可显示任务数据）。
    auto& identity = DeviceIdentity::instance();
    WifiManager wifi;
    wifi.start();
    PcDiscovery pc_discovery(identity.deviceId());
    bool pc_ready = false;
    bool pc_paired = identity.paired();  // PC 是否已认得本设备（发现回包更新）
    std::string last_reset_cmd_id;  // 已执行的 PC"重新配网"指令序号（防重复）

    // 设备画面：由 Wi-Fi / 配对状态决定（主循环 poll 段计算，渲染段分派）
    enum class DevScreen { Normal, WifiProvision, JoiningWifi, PairCode, FindingPc };
    DevScreen dev_screen = DevScreen::Normal;

    // 任务列表面板（下半屏）：字体加载失败时只画底色无文字，不阻断运行
    TaskPanel task_panel;
    task_panel.init(kFontPath);
    // 事件提示音（后台线程播放；无可用声音设备时静默）
    SoundPlayer sound_player;
#endif

    // 1.x 配置生效：翻转 / 迷你 / 监控显隐
    if (cfg.has_flip) {
        renderer.setFlip(cfg.flip);
        gif.setFlip(cfg.flip);
    }
    bool mini_mode = cfg.has_mini && cfg.mini;
    ui.showMetrics = cfg.monitor_enabled;
    ui.monitorCollapsed = cfg.monitor_collapsed;
    ui.showCpu = cfg.monitor_show_cpu;
    ui.showRam = cfg.monitor_show_ram;
    ui.showGpu = cfg.monitor_show_gpu;
    ui.showNet = cfg.monitor_show_net;
    ui.showSelf = cfg.monitor_show_self;
    ui.showProjects = cfg.monitor_show_projects;

    // 6. 状态源 + 状态机
    //    PC：内嵌后端（单进程，HTTP /hook 接收 + 扫描 + 指标采样直供 UI，
    //        原 ApiClient 的本地轮询全部省掉）
    //    设备：HTTP 轮询 PC 端 API（保持双机形态）
#ifdef _WIN32
    backend::BackendService backend;
    backend.start();
    backend.setMonitorActive(cfg.monitor_enabled);  // 面板关 = 采样零开销
    auto& api = backend;  // 方法面与 ApiClient 兼容（takeStatus/菜单动作）
#else
    // 初始地址为空 = 轮询暂停；入网后由 pc_discovery 发现经 setBaseUrl 接入。
    // 提供设备身份：已配对则用持久化 token 直连，否则工作线程自动握手配对。
    ApiClient api("");
    api.setIdentity(identity.deviceId(), identity.pairCode(), identity.token());
    // 读取本机程序版本（部署时由 sync-device.ps1 写入 /opt/dutyon/VERSION）：
    // 附加到 /api/status 轮询头，PC 端据此与源码哈希比对触发自动更新。
    // 文件缺失（旧固件/首次）则为空串，PC 端视为不一致 → 触发首次同步。
    {
        std::string ver;
        std::ifstream vf("/opt/dutyon/VERSION");
        if (vf) std::getline(vf, ver);
        const auto b = ver.find_first_not_of(" \t\r\n");
        const auto e = ver.find_last_not_of(" \t\r\n");
        ver = (b == std::string::npos) ? std::string() : ver.substr(b, e - b + 1);
        api.setProgramVersion(ver);
    }
#endif
    StateMachine state_machine;

    std::string pending_bring_to_front; // 项目行点击（帧结束后处理，避免阻塞 ImGui 帧）
    bool menu_left_active = false;      // 菜单向左展开（右侧屏幕空间不足）

    // 状态动作映射：GIF 形象状态名即动作组（1.x selectAnimBackend 固定
    // STATE_MOTIONS）；Live2D 按当前模型的 stateMotions 应用到状态机
    auto apply_state_motions = [&]() {
        state_machine.clearOverrides();
        if (using_gif) {
            state_machine.setMotionFor("sleeping", "sleeping", 0);
            state_machine.setMotionFor("working", "working", 0);
            state_machine.setMotionFor("alert", "alert", 0);
            return;
        }
        for (const auto& [state, gi] :
             UserConfigStore::stateMotionsFor(cfg, current_model_key))
            state_machine.setMotionFor(state, gi.first, gi.second);
#ifndef _WIN32
        // PC「动作设定」下发的覆盖优先（设备本地仅作断连/离线回退）
        for (const auto& [state, gi] : pc_state_motions)
            state_machine.setMotionFor(state, gi.first, gi.second);
#endif
    };
    apply_state_motions();

#ifndef _WIN32
    // 相框模式轮播：遍历当前形象全部动作（GIF=三个状态动画；
    // Live2D=所有动作组的全部动作），15 秒一个循环推进
    auto frame_motion_count = [&]() -> int {
        if (using_gif) {
            if (!gif_char) return 0;
            int n = 0;
            for (const auto& f :
                 {gif_char->sleeping, gif_char->working, gif_char->alert})
                if (!f.empty()) n++;
            return n;
        }
        int n = 0;
        for (const auto& g : renderer.motionGroups()) n += g.count;
        return n;
    };
    auto play_frame_motion = [&](int idx) {
        const int n = frame_motion_count();
        if (n <= 0) return;
        frame_idx = ((idx % n) + n) % n;
        if (using_gif) {
            if (!gif_char) return;
            const std::string files[3] = {gif_char->sleeping,
                                          gif_char->working, gif_char->alert};
            int k = 0;
            for (const auto& f : files) {
                if (f.empty()) continue;
                if (k == frame_idx) {
                    gif.load(UserConfigStore::animationsDir() + "/" + f);
                    break;
                }
                k++;
            }
        } else {
            int k = 0;
            for (const auto& g : renderer.motionGroups())
                for (int i = 0; i < g.count; i++) {
                    if (k == frame_idx) {
                        renderer.setLoopMotion(g.group, i);
                        return;
                    }
                    k++;
                }
        }
    };
#endif

#ifndef _WIN32
    // 电子相框「指定文件夹」照片播放器：声明在 api 之后（栈对象后声明先
    // 析构，退出时先 join 后台线程，避免 worker 访问已释放的 ApiClient）。
    // 取字节回调走 ApiClient::fetchFramePhoto（在 PhotoPlayer 后台线程执行）。
    PhotoPlayer photo_player;
    photo_player.setFetcher(
        [&](std::vector<unsigned char>& out) {
            return api.fetchFramePhoto(out);
        });
#endif

    if (!using_gif) {
        auto [init_group, init_idx] = state_machine.currentMotion();
        renderer.setLoopMotion(init_group, init_idx);
    }

    // ---- 菜单数据缓存（menu_collect 每帧调用，避免每帧目录扫描/HTTP）----
    std::vector<UIRenderer::MenuEntry> motions_cache;  // 当前模型动作列表
    bool motions_dirty = true;
    std::string hook_hint_cache = "未检查";
    int autostart_cache = -1;  // 1 开 / 0 关 / -1 未知

    auto rebuild_motions = [&]() {
        motions_cache.clear();
        if (using_gif) {
            // 1.x customAnimBackend.getMotionList：只列出自身有动画文件的
            // 状态；动作名 = i18n("state.<状态>")
            if (gif_char) {
                const std::string states[] = {"sleeping", "working", "alert"};
                const std::string files[] = {gif_char->sleeping, gif_char->working,
                                             gif_char->alert};
                for (int i = 0; i < 3; i++) {
                    if (files[i].empty()) continue;
                    motions_cache.push_back({states[i] + ":0",
                                             I18n::t(("state." + states[i]).c_str()),
                                             false});
                }
            }
        } else {
            for (const auto& g : renderer.motionGroups())
                for (int i = 0; i < g.count; i++)
                    motions_cache.push_back({g.group + ":" + std::to_string(i),
                                             I18n::motionName(g.group, i), false});
        }
        motions_dirty = false;
    };

    // 菜单打开时刷新（autostart / hook 走一次性 HTTP，模型目录重扫）
    auto refresh_menu_caches = [&]() {
        autostart_cache = api.getAutostart();
        const std::string j = api.getHooks();
        if (j.empty()) {
            hook_hint_cache = "未连接";
        } else if (j.find("\"installed\":true") != std::string::npos ||
                   j.find("\"installed\": true") != std::string::npos) {
            hook_hint_cache = "已安装";
        } else {
            hook_hint_cache = "未安装";
        }
        model_entries = UserConfigStore::listModels(builtin_roots);
        motions_dirty = true;
    };
    ui.on_menu_open = refresh_menu_caches;

    // ---- 右键菜单接线（ImGui 自绘，结构对齐 1.x index.html #context-menu）----
    ui.menu_is_checked = [&](const std::string& id) -> bool {
        if (id == "vis-monitor") return ui.showMetrics;
        if (id == "vis-cpu") return ui.showCpu;
        if (id == "vis-ram") return ui.showRam;
        if (id == "vis-gpu") return ui.showGpu;
        if (id == "vis-net") return ui.showNet;
        if (id == "vis-self") return ui.showSelf;
        if (id == "vis-projects") return ui.showProjects;
        if (id == "flip") return renderer.isFlipped();
        if (id == "mini") return mini_mode;
        if (id == "autostart") return autostart_cache == 1;
        if (id.rfind("lang:", 0) == 0) return id.substr(5) == I18n::lang();
        // 设备声音管理：完全静音 / 按状态静音（键=当前活动角色）
        if (id == "sound-mute") return cfg.sound_mute;
        if (id.rfind("sound-muted:", 0) == 0)
            return cfg.state_audio_muted[cfg.active_character_id + ":" +
                                         id.substr(12)];
        return false;
    };

    ui.menu_hint = [&](const std::string& id) -> std::string {
        if (id == "hook-status") return hook_hint_cache;
        // 集成状态（主菜单“集成”右侧）：Hook 已安装即视为已集成
        if (id == "integration-status")
            return I18n::t(hook_hint_cache == "已安装" ? "menu.integrated"
                                                        : "menu.notIntegrated");
        if (id.rfind("assign:", 0) == 0) {
            auto [g, i] = state_machine.motionForState(id.substr(7));
            // 与播放/选择网格显示一致：GIF 形象用状态名，Live2D 用动作显示名
            //（原样返回 "组[序号]" 是动作原始名，未本地化）
            if (using_gif) return I18n::t(("state." + g).c_str());
            return I18n::motionName(g, i);
        }
        // charname:<charid> —— 角色编辑视图标题（角色显示名）
        if (id.rfind("charname:", 0) == 0) {
            const std::string cid = id.substr(9);
            for (const auto& c : cfg.custom_characters)
                if (c.id == cid) return c.name;
            return {};
        }
        // charfile:<charid>:<state> —— 编辑行右侧的当前动画文件名
        if (id.rfind("charfile:", 0) == 0) {
            const size_t p1 = id.find(':', 9);
            if (p1 == std::string::npos) return {};
            const std::string cid = id.substr(9, p1 - 9);
            const std::string state = id.substr(p1 + 1);
            for (const auto& c : cfg.custom_characters)
                if (c.id == cid) return gifFileFor(c, state);
            return {};
        }
        // charaudio:<charid>:<state> —— 角色编辑页当前绑定的音频文件名
        if (id.rfind("charaudio:", 0) == 0) {
            const size_t p1 = id.find(':', 10);
            if (p1 == std::string::npos) return {};
            const std::string cid = id.substr(10, p1 - 10);
            const std::string state = id.substr(p1 + 1);
            auto it = cfg.state_audio.find(cid);
            if (it == cfg.state_audio.end()) return {};
            auto f = it->second.find(state);
            return f == it->second.end() ? std::string() : f->second;
        }
        // stateaudio:<state> —— 当前活动角色绑定的音频文件名
        //（动作设定页 / 声音管理页；键可能是含冒号的模型 URL，
        //  故 action/hint 均不内嵌 key，处理时取 active_character_id）
        if (id.rfind("stateaudio:", 0) == 0) {
            auto it = cfg.state_audio.find(cfg.active_character_id);
            if (it == cfg.state_audio.end()) return {};
            auto f = it->second.find(id.substr(11));
            return f == it->second.end() ? std::string() : f->second;
        }
        return {};
    };

    // key: "models"=切换形象；"motions"=播放动作；"motions:<状态>"=动作设定
    ui.menu_collect = [&](const std::string& key) -> std::vector<UIRenderer::MenuEntry> {
        if (key == "models") {
            std::vector<UIRenderer::MenuEntry> out;
            // 1.x getCharacters 顺序：内置模型在前，自定义形象在后；
            // 缩略图：Live2D 用 1.x 缓存 ~/.dutyon/thumbnails/<名>.png（运行时
            // 生成，每次收集现查以拾取新生成的），GIF 形象用 sleeping 首帧
            for (const auto& e : model_entries)
                out.push_back({"model:" + e.key, e.name, e.key == current_model_key,
                               UserConfigStore::thumbnailFor(e.name)});
            for (const auto& c : cfg.custom_characters) {
                UIRenderer::MenuEntry me;
                me.id = "char:" + c.id;
                me.label = c.name;
                me.checked = using_gif && gif_char && gif_char->id == c.id;
                const std::string f = gifFileFor(c, "sleeping");
                if (!f.empty())
                    me.thumb = UserConfigStore::animationsDir() + "/" +f;
                out.push_back(std::move(me));
            }
            return out;
        }
        if (key == "motions" || key.rfind("motions:", 0) == 0) {
            if (motions_dirty) rebuild_motions();
            auto out = motions_cache;
            if (key.rfind("motions:", 0) == 0) {
                // 动作设定视图：勾选当前状态已绑定的动作
                auto [g, i] = state_machine.motionForState(key.substr(8));
                for (auto& e : out)
                    e.checked = (e.id == g + ":" + std::to_string(i));
            }
            return out;
        }
        // 自定义角色管理列表（编辑视图入口）
        if (key == "charmanage") {
            std::vector<UIRenderer::MenuEntry> out;
            for (const auto& c : cfg.custom_characters)
                out.push_back({"charedit:" + c.id, c.name, false, {}});
            return out;
        }
        return {};
    };

    auto sync_monitor_cfg = [&]() {
        cfg.monitor_enabled = ui.showMetrics;
        cfg.monitor_collapsed = ui.monitorCollapsed;
        cfg.monitor_show_cpu = ui.showCpu;
        cfg.monitor_show_ram = ui.showRam;
        cfg.monitor_show_gpu = ui.showGpu;
        cfg.monitor_show_net = ui.showNet;
        cfg.monitor_show_self = ui.showSelf;
        cfg.monitor_show_projects = ui.showProjects;
        UserConfigStore::saveMonitor(cfg);
#ifdef _WIN32
        backend.setMonitorActive(ui.showMetrics);  // 面板开关联动采样线程
#endif
    };

#ifdef _WIN32
    // 自定义角色动画文件选择器（模态；属主=桌宠窗口，防被置顶窗遮挡）
    const HWND pet_hwnd = static_cast<HWND>(window->nativeWinHandle());
    auto pick_animation_file = [&]() -> std::string {
        wchar_t buf[MAX_PATH] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = pet_hwnd;
        ofn.lpstrFilter =
            L"GIF/PNG/JPEG (*.gif;*.png;*.jpg;*.jpeg)\0*.gif;*.png;*.jpg;*.jpeg\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = L"选择动画 / 图片文件";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) return {};  // 取消/失败同按空处理
        const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0,
                                            nullptr, nullptr);
        if (len <= 1) return {};
        std::string out((size_t)len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), len, nullptr,
                            nullptr);
        return out;
    };

    // 把选中的源文件复制进 ~/.dutyon/animations/ 并挂到角色的某个状态。
    // 文件名带毫秒时间戳：替换后文件名必变 —— 设备端按文件名比对自动重下
    //（/api/character 只下发文件名，无内容哈希）。旧文件随之删除。
    auto install_char_file = [&](CustomCharacter& c, const std::string& state,
                                 const std::string& src) -> bool {
        namespace fs = std::filesystem;
        std::string ext = fs::path(src).extension().string();
        for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
        if (ext != ".gif" && ext != ".png" && ext != ".jpg" && ext != ".jpeg") {
            MessageBoxW(pet_hwnd, L"仅支持 GIF / PNG / JPG 文件",
                        L"Duty On", MB_OK | MB_ICONWARNING);
            return false;
        }
        const long long ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string fname =
            c.id + "_" + state + "_" + std::to_string(ms) + ext;
        const fs::path dst = fs::path(UserConfigStore::animationsDir()) / fname;
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(fs::path(src), dst,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            MessageBoxW(pet_hwnd, L"文件复制失败，请重试", L"Duty On",
                        MB_OK | MB_ICONWARNING);
            return false;
        }
        std::string& slot = state == "sleeping"   ? c.sleeping
                            : state == "working" ? c.working
                                                 : c.alert;
        if (!slot.empty() && slot != fname) {
            std::error_code ec2;  // 旧文件删除失败不阻塞流程
            fs::remove(fs::path(UserConfigStore::animationsDir()) / slot, ec2);
        }
        slot = fname;
        return true;
    };

    // 状态音频文件选择器（设备端状态切换提示音；格式 wav/mp3/ogg/flac/m4a）
    auto pick_audio_file = [&]() -> std::string {
        wchar_t buf[MAX_PATH] = L"";
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = pet_hwnd;
        ofn.lpstrFilter =
            L"音频 (*.wav;*.mp3;*.ogg;*.flac;*.m4a)\0*.wav;*.mp3;*.ogg;*.flac;*.m4a\0";
        ofn.lpstrFile = buf;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrTitle = L"选择状态音频文件";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&ofn)) return {};  // 取消/失败同按空处理
        const int len = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0,
                                            nullptr, nullptr);
        if (len <= 1) return {};
        std::string out((size_t)len - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), len, nullptr,
                            nullptr);
        return out;
    };

    // 把选中的音频复制进 ~/.dutyon/animations/ 并绑定到状态
    //（key = 角色 id 或模型 URL）。文件名 = audio_<key安全化>_<状态>_<ms>.ext：
    // 替换后文件名必变，设备端按文件名比对自动重下；旧文件随之删除。
    auto install_audio_file = [&](const std::string& key,
                                  const std::string& state,
                                  const std::string& src) -> bool {
        namespace fs = std::filesystem;
        std::string ext = fs::path(src).extension().string();
        for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
        if (ext != ".wav" && ext != ".mp3" && ext != ".ogg" &&
            ext != ".flac" && ext != ".m4a") {
            MessageBoxW(pet_hwnd,
                        L"仅支持 WAV / MP3 / OGG / FLAC / M4A 文件",
                        L"Duty On", MB_OK | MB_ICONWARNING);
            return false;
        }
        std::string safe;
        for (const char ch : key)
            safe += ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') ||
                     (ch >= 'A' && ch <= 'Z') || ch == '_')
                        ? ch
                        : '_';
        const long long ms = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string fname =
            "audio_" + safe + "_" + state + "_" + std::to_string(ms) + ext;
        const fs::path dst = fs::path(UserConfigStore::animationsDir()) / fname;
        std::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(fs::path(src), dst,
                      fs::copy_options::overwrite_existing, ec);
        if (ec) {
            MessageBoxW(pet_hwnd, L"文件复制失败，请重试", L"Duty On",
                        MB_OK | MB_ICONWARNING);
            return false;
        }
        auto kit = cfg.state_audio.find(key);
        if (kit != cfg.state_audio.end()) {
            auto fit = kit->second.find(state);
            if (fit != kit->second.end() && fit->second != fname) {
                std::error_code ec2;  // 旧文件删除失败不阻塞流程
                fs::remove(fs::path(UserConfigStore::animationsDir()) /
                               fit->second,
                           ec2);
            }
        }
        cfg.state_audio[key][state] = fname;
        return true;
    };

    // 状态音频试听（PC 端）：MCI 异步播放已绑定文件。用 W 版兼容非 ASCII
    // 路径；同一 alias 先停旧再开新，实现“重新试听”。MCI 不支持的格式
    //（部分 ogg/flac/m4a）open 失败即静默忽略。
    auto preview_audio_file = [](const std::string& utf8_path) {
        mciSendStringW(L"stop dutyonprev", nullptr, 0, nullptr);
        mciSendStringW(L"close dutyonprev", nullptr, 0, nullptr);
        const int n = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1,
                                          nullptr, 0);
        if (n <= 1) return;
        std::wstring wp((size_t)n - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, wp.data(), n);
        const std::wstring cmd = L"open \"" + wp + L"\" alias dutyonprev";
        if (mciSendStringW(cmd.c_str(), nullptr, 0, nullptr) != 0) return;
        mciSendStringW(L"play dutyonprev", nullptr, 0, nullptr);
    };

    // 系统默认音频文件名（未绑定自定义时试听播放；与设备端 SoundPlayer
    // 事件一致）：每个角色默认相同，即随包语音文件
    auto default_audio_file = [](const std::string& state) -> std::string {
        if (state == "working") return "mission_start.wav";
        if (state == "sleeping") return "mission_complete.wav";
        if (state == "alert") return "attention.wav";
        if (state == "welcome") return "welcome.wav";
        return std::string();
    };
    // 随包音频目录：发布 <exe>/assets/device/sounds；开发 仓库
    // frontend/assets/device/sounds。以 mission_start.wav 存在为识别标志
    auto find_sounds_dir = []() -> std::string {
        namespace fs = std::filesystem;
        char exe_path[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return std::string();
        const fs::path exe_dir = fs::path(exe_path).parent_path();
        const fs::path cands[] = {
            exe_dir / "assets" / "device" / "sounds",
            exe_dir / ".." / ".." / ".." / "frontend" / "assets" / "device" /
                "sounds",
        };
        std::error_code ec;
        for (const auto& c : cands) {
            const fs::path p = c.lexically_normal();
            if (fs::is_regular_file(p / "mission_start.wav", ec))
                return p.generic_string();
        }
        return std::string();
    };

    // 相框照片文件夹选择器（模态 shell 文件夹框；取消返回空）。路径以
    // UTF-8 回传（与 config.json / HTTP 服务一致）
    auto pick_frame_folder = [&]() -> std::string {
        BROWSEINFOW bi{};
        bi.hwndOwner = pet_hwnd;
        bi.lpszTitle = L"选择电子相框照片文件夹（仅 JPG / PNG）";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE |
                     BIF_EDITBOX | BIF_NONEWFOLDERBUTTON;
        std::string out;
        if (LPITEMIDLIST pidl = SHBrowseForFolderW(&bi)) {
            wchar_t path[MAX_PATH] = L"";
            if (SHGetPathFromIDListW(pidl, path)) {
                const int len = WideCharToMultiByte(CP_UTF8, 0, path, -1,
                                                    nullptr, 0, nullptr, nullptr);
                if (len > 1) {
                    out.resize((size_t)len - 1);
                    WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), len,
                                        nullptr, nullptr);
                }
            }
            CoTaskMemFree(pidl);
        }
        return out;
    };

    // 统计照片目录内可用照片数（只扫一次，上限 400 张：菜单提示用，
    // 真正常播的目录不会远少到这个数；上限避免大目录每帧卡渲染线程）
    auto count_frame_photos = [](const std::string& folder_utf8) -> int {
        namespace fs = std::filesystem;
        if (folder_utf8.empty()) return 0;
        const int n = MultiByteToWideChar(CP_UTF8, 0, folder_utf8.c_str(), -1,
                                         nullptr, 0);
        if (n <= 1) return 0;
        std::wstring wp((size_t)n - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, folder_utf8.c_str(), -1, wp.data(), n);
        std::error_code ec;
        int cnt = 0;
        for (const auto& e : fs::directory_iterator(fs::path(wp), ec)) {
            if (cnt >= 400) break;
            if (!e.is_regular_file(ec)) continue;
            std::string ext = e.path().extension().string();
            for (auto& ch : ext) ch = (char)tolower((unsigned char)ch);
            if (ext == ".jpg" || ext == ".jpeg" || ext == ".png") cnt++;
        }
        return cnt;
    };
#endif

    ui.menu_activate = [&](const std::string& id) {
        // ---- 显示隐藏（vis-*）----
        if (id.rfind("vis-", 0) == 0) {
            if (id == "vis-monitor") ui.showMetrics = !ui.showMetrics;
            else if (id == "vis-cpu") ui.showCpu = !ui.showCpu;
            else if (id == "vis-ram") ui.showRam = !ui.showRam;
            else if (id == "vis-gpu") ui.showGpu = !ui.showGpu;
            else if (id == "vis-net") ui.showNet = !ui.showNet;
            else if (id == "vis-self") ui.showSelf = !ui.showSelf;
            else if (id == "vis-projects") ui.showProjects = !ui.showProjects;
            sync_monitor_cfg();
        }
        // ---- 外观 ----
        else if (id == "flip") {
            renderer.setFlip(!renderer.isFlipped());
            gif.setFlip(renderer.isFlipped());
            UserConfigStore::saveFlip(renderer.isFlipped());
        } else if (id == "mini") {
            mini_mode = !mini_mode;
            UserConfigStore::saveMini(mini_mode);
        }
        // ---- 最小化：隐藏到系统托盘（托盘图标左键单击/双击唤回）----
        else if (id == "minimize") {
#ifdef _WIN32
            window->setVisible(false);
#endif
        }
        // ---- 语言 ----
        else if (id.rfind("lang:", 0) == 0) {
            const std::string code = id.substr(5);
            if (code != I18n::lang()) {
                I18n::setLang(code);
                ui.reloadFonts();       // ja/ko/zh-TW 字形范围不同，重建图集
                motions_dirty = true;   // 动作名翻译
                UserConfigStore::saveLanguage(code);
            }
        }
        // ---- 硬件显示端模式（菜单"设备模式"三选一）----
        else if (id.rfind("device-mode:", 0) == 0) {
            const std::string mode = id.substr(12);
            if (mode == "single" || mode == "multi" || mode == "frame") {
                cfg.device_mode = mode;
                UserConfigStore::saveDeviceMode(mode);
                // 立即生效（设备端下一次 /api/status 轮询 ≤2s 收到）
            }
        }
        // ---- 相框播放源（菜单「设备→相框播放」：动作轮播 / 指定文件夹）----
        else if (id.rfind("frame-source:", 0) == 0) {
#ifdef _WIN32
            const std::string src = id.substr(13);
            if (src == "motion") {
                cfg.frame_source = "motion";
                UserConfigStore::saveFrameSource("motion");
            } else if (src == "folder") {
                // 启用已选目录；未选过时先弹一次选择器（取消则保持原状）
                bool go = true;
                if (cfg.frame_folder.empty()) {
                    const std::string picked = pick_frame_folder();
                    if (picked.empty()) {
                        go = false;
                    } else {
                        cfg.frame_folder = picked;
                        UserConfigStore::saveFrameFolder(picked);
                    }
                }
                if (go) {
                    cfg.frame_source = "folder";
                    UserConfigStore::saveFrameSource("folder");
                }
            } else if (src == "pick") {
                const std::string picked = pick_frame_folder();
                if (!picked.empty()) {  // 取消：保持原状
                    cfg.frame_folder = picked;
                    UserConfigStore::saveFrameFolder(picked);
                    // 选完即切到照片源（否则用户得再点一下）
                    cfg.frame_source = "folder";
                    UserConfigStore::saveFrameSource("folder");
                }
            }
            // 设备端下一次 /api/status 轮询（≤2s）收到 frameSource
#endif
        }
        // ---- 显示文字颜色（菜单"时钟颜色"五选一）：作用于当前角色，
        // 存入 characterColors[activeCharacterId]；切换角色自动采用各自颜色 ----
        else if (id.rfind("clock-color:", 0) == 0) {
            const std::string color = id.substr(12);
            if (color == "amber" || color == "ice" || color == "white" ||
                color == "green" || color == "pink") {
                if (cfg.active_character_id.empty()) {
                    // 无当前角色：回退写全局默认色
                    cfg.clock_color = color;
                    UserConfigStore::saveClockColor(color);
                } else {
                    cfg.character_colors[cfg.active_character_id] = color;
                    UserConfigStore::saveCharacterColor(cfg.active_character_id,
                                                        color);
                }
                // 立即生效（设备端下一次 /api/status 轮询 ≤2s 收到）
            }
        }
        // ---- 硬件显示端亮度（菜单"设备→亮度"五档）----
        else if (id.rfind("device-brightness:", 0) == 0) {
            const int v = atoi(id.c_str() + 18);
            if (v >= 10 && v <= 100) {
                cfg.device_brightness = v;
                UserConfigStore::saveDeviceBrightness(v);
                // 设备端下一次 /api/status 轮询 ≤2s 收到
            }
        }
        // ---- 屏幕旋转（菜单「设备→屏幕旋转」0/90/180/270；经 /api/status 下发）----
        else if (id.rfind("device-rotate:", 0) == 0) {
            // 前缀 "device-rotate:" 共 14 字符，偏移 +14 才是角度数字；
            // 用 atoi（解析失败返回 0，不抛异常），避免 stoi 遇非法输入崩溃
            const int deg = atoi(id.c_str() + 14);
            if (deg == 0 || deg == 90 || deg == 180 || deg == 270) {
                cfg.screen_rotation = deg;
                UserConfigStore::saveScreenRotation(deg);
            }
        }
        // ---- 设备配对（Wi-Fi 配对码方案；菜单「设备→配对设备」直接弹窗）----
        else if (id == "pair-input") {
#ifdef _WIN32
            // 深色配对弹窗（见 platform/pair_code_dialog.h）：未配对=验证码
            // 风格 6 格输码；已配对=状态 + 解除配对 / 清除设备 Wi-Fi。
            // 旧方案在菜单里轮询 GetAsyncKeyState 采集物理键，桌宠窗口
            // WS_EX_NOACTIVATE 不抢焦点，实测漏键输不进去。
            PairDialog::Host host;
            host.confirm = [&](const std::string& code) {
                return backend.confirmPairing(code);
            };
            host.unpair = [&](const std::string& did) {
                backend.unpairDevice(did);
            };
            host.reset_wifi = [&](const std::string& did) {
                // 清设备端 Wi-Fi 凭据重进配网模式（token 保留，无需再配对）
                backend.requestDeviceResetWifi(did);
                MessageBoxW(
                    pet_hwnd,
                    L"已下发重新配网指令，设备将清除 Wi-Fi 并重启配网模式",
                    L"Duty On", MB_OK | MB_ICONINFORMATION);
            };
            host.paired = [&]() {
                std::vector<PairDialog::Device> out;
                for (const auto& did : backend.pairedDevices()) {
                    out.push_back({did, did.size() > 8 ? did.substr(0, 8)
                                                       : did});
                }
                return out;
            };
            PairDialog dlg;
            dlg.run(pet_hwnd, host);
#endif
        }
        // ---- 形象 / 动作 ----
        else if (id.rfind("model:", 0) == 0) {
            const std::string key = id.substr(6);
            for (const auto& e : model_entries) {
                if (e.key != key) continue;
                if (renderer.loadModelFile(e.dir, e.json)) {
                    using_gif = false;  // 切回 Live2D（1.x refreshActiveCharacter）
                    gif_char = nullptr;
                    gif.unload();
                    current_model_key = e.key;
                    cfg.active_character_id = e.key;
                    UserConfigStore::saveActiveCharacter(e.key);
                    apply_state_motions();  // 新模型的 stateMotions
                    motions_dirty = true;
                    auto [g, i] = state_machine.currentMotion();
                    renderer.setLoopMotion(g, i);
                }
                break;
            }
        } else if (id.rfind("char:", 0) == 0) {
            // char:<id> —— 切换到自定义 GIF 形象，按当前状态加载动画
            const std::string cid = id.substr(5);
            for (const auto& c : cfg.custom_characters) {
                if (c.id != cid) continue;
                using_gif = true;
                gif_char = &c;
                gif.setFlip(renderer.isFlipped());
                apply_state_motions();  // GIF：状态名即动作组
                auto [g, i] = state_machine.currentMotion();
                const std::string file = gifFileFor(c, g.empty() ? "sleeping" : g);
                if (file.empty() ||
                    !gif.load(UserConfigStore::animationsDir() + "/" +file)) {
                    using_gif = false;
                    gif_char = nullptr;
                    fprintf(stderr, "GIF load failed: %s\n", file.c_str());
                    break;
                }
                cfg.active_character_id = c.id;
                UserConfigStore::saveActiveCharacter(c.id);
                motions_dirty = true;
                printf("GIF character: %s (%s)\n", c.name.c_str(), c.id.c_str());
                break;
            }
        }
        // ---- 自定义角色：新建（选文件，三状态共用；逐状态可在编辑视图换）----
        else if (id == "charnew") {
#ifdef _WIN32
            const std::string src = pick_animation_file();
            if (!src.empty()) {
                CustomCharacter ch;
                const long long ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
                ch.id = "char_" + std::to_string(ms);
                ch.name = std::filesystem::path(src).stem().string();
                if (ch.name.empty()) ch.name = "自定义角色";
                bool okc = true;
                for (const char* st : {"sleeping", "working", "alert"})
                    okc = install_char_file(ch, st, src) && okc;
                if (okc) {
                    // push_back 可能搬移 vector 存储：先存活动角色 id，
                    // 之后重建指针防 gif_char 悬空
                    const std::string active_id =
                        (using_gif && gif_char) ? gif_char->id : std::string();
                    cfg.custom_characters.push_back(std::move(ch));
                    UserConfigStore::saveCustomCharacters(cfg);
                    if (!active_id.empty()) {
                        gif_char = nullptr;
                        for (const auto& cc : cfg.custom_characters)
                            if (cc.id == active_id) { gif_char = &cc; break; }
                    }
                }
            }
#endif
        }
        // ---- 自定义角色：更换某状态的动画文件（charset:<id>:<state>）----
        else if (id.rfind("charset:", 0) == 0) {
#ifdef _WIN32
            const size_t p1 = id.find(':', 8);
            if (p1 != std::string::npos) {
                const std::string cid = id.substr(8, p1 - 8);
                const std::string state = id.substr(p1 + 1);
                const std::string src = pick_animation_file();
                if (!src.empty()) {
                    for (auto& c : cfg.custom_characters) {
                        if (c.id != cid) continue;
                        if (install_char_file(c, state, src)) {
                            UserConfigStore::saveCustomCharacters(cfg);
                            // 正在显示该角色且被改的恰是当前状态 → 立即生效
                            if (using_gif && gif_char && gif_char->id == cid) {
                                auto [g, gi] = state_machine.currentMotion();
                                const std::string cur =
                                    g.empty() ? "sleeping" : g;
                                if (cur == state) {
                                    const std::string f = gifFileFor(c, state);
                                    if (!f.empty())
                                        gif.load(UserConfigStore::
                                                     animationsDir() +
                                                 "/" + f);
                                }
                            }
                        }
                        break;
                    }
                }
            }
#endif
        }
        // ---- 自定义角色：删除（chardelete:<id>；确认后连文件一起删）----
        else if (id.rfind("chardelete:", 0) == 0) {
#ifdef _WIN32
            const std::string cid = id.substr(11);
            for (size_t ci = 0; ci < cfg.custom_characters.size(); ci++) {
                const CustomCharacter& c = cfg.custom_characters[ci];
                if (c.id != cid) continue;
                // 确认框（角色名 UTF-8 → UTF-16）
                const int wlen = MultiByteToWideChar(
                    CP_UTF8, 0, c.name.c_str(), -1, nullptr, 0);
                std::wstring wname((size_t)wlen, L'\0');
                MultiByteToWideChar(CP_UTF8, 0, c.name.c_str(), -1,
                                    wname.data(), wlen);
                const std::wstring q = L"确定删除角色「" + wname +
                                       L"」吗？\r\n对应的动画文件会一并删除。";
                if (MessageBoxW(pet_hwnd, q.c_str(), L"Duty On",
                                MB_YESNO | MB_ICONQUESTION) != IDYES)
                    break;
                const bool was_active =
                    using_gif && gif_char && gif_char->id == cid;
                std::error_code ec;
                for (const char* st : {"sleeping", "working", "alert"}) {
                    const std::string f = gifFileFor(c, st);
                    if (!f.empty())
                        std::filesystem::remove(
                            std::filesystem::path(
                                UserConfigStore::animationsDir()) /
                                f,
                            ec);
                }
                cfg.custom_characters.erase(cfg.custom_characters.begin() +
                                            (ptrdiff_t)ci);
                UserConfigStore::saveCustomCharacters(cfg);
                if (was_active) {
                    // 删的是当前角色 → 切回第一个 Live2D 模型
                    using_gif = false;
                    gif_char = nullptr;
                    gif.unload();
                    if (!model_entries.empty()) {
                        const auto& e = model_entries.front();
                        if (renderer.loadModelFile(e.dir, e.json)) {
                            current_model_key = e.key;
                            cfg.active_character_id = e.key;
                            UserConfigStore::saveActiveCharacter(e.key);
                            apply_state_motions();
                            motions_dirty = true;
                            auto [g2, gi2] = state_machine.currentMotion();
                            renderer.setLoopMotion(g2, gi2);
                        }
                    } else {
                        cfg.active_character_id.clear();
                        UserConfigStore::saveActiveCharacter("");
                    }
                }
                break;
            }
#endif
        }
        // ---- 状态音频绑定：角色编辑页（charaudio:<id>:<state>）----
        else if (id.rfind("charaudio:", 0) == 0 ||
                 id.rfind("charaudioclear:", 0) == 0) {
#ifdef _WIN32
            const bool clear = id.rfind("charaudioclear:", 0) == 0;
            const size_t off = clear ? 15 : 10;
            const size_t p1 = id.find(':', off);
            if (p1 != std::string::npos) {
                const std::string cid = id.substr(off, p1 - off);
                const std::string state = id.substr(p1 + 1);
                for (const auto& c : cfg.custom_characters) {
                    if (c.id != cid) continue;
                    std::string fname;  // clear = 清除绑定并删文件
                    if (!clear) {
                        const std::string src = pick_audio_file();
                        if (src.empty() ||
                            !install_audio_file(cid, state, src))
                            break;
                        fname = cfg.state_audio[cid][state];
                    } else {
                        auto kit = cfg.state_audio.find(cid);
                        if (kit != cfg.state_audio.end()) {
                            auto fit = kit->second.find(state);
                            if (fit != kit->second.end()) {
                                std::error_code ec;
                                std::filesystem::remove(
                                    std::filesystem::path(
                                        UserConfigStore::animationsDir()) /
                                        fit->second,
                                    ec);
                            }
                        }
                    }
                    UserConfigStore::saveStateAudio(cid, state, fname);
                    break;
                }
            }
#endif
        }
        // ---- 状态音频绑定：动作设定页（stateaudio:<state>；键=活动角色，
        //      可能是含冒号的模型 URL，故不内嵌 key）----
        else if (id.rfind("stateaudio:", 0) == 0 ||
                 id.rfind("stateaudioclear:", 0) == 0) {
            const bool clear = id.rfind("stateaudioclear:", 0) == 0;
            const std::string state = id.substr(clear ? 16 : 11);
            const std::string key = cfg.active_character_id;
            if (!key.empty()) {
#ifdef _WIN32
                std::string fname;  // clear = 清除绑定并删文件
                if (!clear) {
                    const std::string src = pick_audio_file();
                    if (!src.empty() && install_audio_file(key, state, src))
                        fname = cfg.state_audio[key][state];
                } else {
                    auto kit = cfg.state_audio.find(key);
                    if (kit != cfg.state_audio.end()) {
                        auto fit = kit->second.find(state);
                        if (fit != kit->second.end()) {
                            std::error_code ec;
                            std::filesystem::remove(
                                std::filesystem::path(
                                    UserConfigStore::animationsDir()) /
                                    fit->second,
                                ec);
                        }
                    }
                }
                if (clear || !fname.empty())
                    UserConfigStore::saveStateAudio(key, state, fname);
#endif
            }
        }
        // ---- 状态音频试听：已绑定→播绑定文件；未绑定→播系统默认音频 ----
        else if (id.rfind("stateaudiopreview:", 0) == 0) {
#ifdef _WIN32
            const std::string state = id.substr(18);
            std::string path;
            auto kit = cfg.state_audio.find(cfg.active_character_id);
            if (kit != cfg.state_audio.end()) {
                auto fit = kit->second.find(state);
                if (fit != kit->second.end() && !fit->second.empty())
                    path = UserConfigStore::animationsDir() + "/" + fit->second;
            }
            if (path.empty()) {  // 未绑定 → 系统默认音频（随包语音）
                const std::string sdir = find_sounds_dir();
                const std::string df = default_audio_file(state);
                if (!sdir.empty() && !df.empty()) path = sdir + "/" + df;
            }
            if (!path.empty()) preview_audio_file(path);
#endif
        }
        // ---- 设备声音管理：完全静音 / 按状态静音（经 /api/status 下发）----
        else if (id == "sound-mute") {
            cfg.sound_mute = !cfg.sound_mute;
            UserConfigStore::saveSoundMute(cfg.sound_mute);
        } else if (id.rfind("sound-mute-state:", 0) == 0) {
            const std::string state = id.substr(17);
            const std::string mk = cfg.active_character_id + ":" + state;
            const bool muted = cfg.state_audio_muted.count(mk) != 0 &&
                               cfg.state_audio_muted[mk];
            cfg.state_audio_muted[mk] = !muted;
            UserConfigStore::saveStateAudioMuted(cfg.active_character_id,
                                                 state, !muted);
        } else if (id.rfind("motion:", 0) == 0 || id.rfind("preview:", 0) == 0) {
            // motion:<组>:<序号> 一次性播放；preview:<组>:<序号> 悬停预览
            const size_t off = id.find(':') + 1;
            const size_t p = id.find(':', off);
            if (p != std::string::npos) {
                const std::string group = id.substr(off, p - off);
                if (using_gif) {
                    // 1.x customAnimBackend.play：切到该状态的动画
                    if (gif_char) {
                        const std::string file = gifFileFor(*gif_char, group);
                        if (!file.empty())
                            gif.load(UserConfigStore::animationsDir() + "/" +file);
                    }
                } else {
                    renderer.playMotion(group, atoi(id.c_str() + p + 1));
                }
            }
        } else if (id.rfind("assign:", 0) == 0) {
            // assign:<状态>:<组>:<序号> —— 动作设定
            const size_t p1 = id.find(':', 7);
            const size_t p2 = id.find(':', p1 + 1);
            if (p1 != std::string::npos && p2 != std::string::npos) {
                const std::string state = id.substr(7, p1 - 7);
                const std::string group = id.substr(p1 + 1, p2 - p1 - 1);
                const int index = atoi(id.c_str() + p2 + 1);
                state_machine.setMotionFor(state, group, index);
                // GIF 形象映射固定（1.x selectAnimBackend），不写 stateMotions
                if (!using_gif) {
                    cfg.state_motions[current_model_key][state] = {group, index};
                    UserConfigStore::saveStateMotion(current_model_key, state, group,
                                                     index);
                }
                motions_dirty = true;
                // 若改的是当前状态，立即生效
                auto [g, i] = state_machine.currentMotion();
                if (!using_gif) renderer.setLoopMotion(g, i);
            }
        } else if (id == "preview-alert") {
            ui.previewAlert();  // 头顶 ! 特效 + 状态栏闪红 3s
        }
        // ---- 系统集成 ----
        else if (id == "open-models-dir") {
#ifdef _WIN32
            ShellExecuteA(nullptr, "open", UserConfigStore::userModelsDir().c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
#endif
        } else if (id == "install-hooks") {
#ifdef _WIN32
            // 不能在 UI 线程同步跑：HTTP（最长 15s）+ 模态弹窗会把渲染循环
            // 卡死，且无属主的 MessageBox 可能藏在别的窗口后面 —— 观感就是
            // “点了没反应/卡死”。后台线程执行；菜单缓存下次开菜单时由
            // on_menu_open 自动刷新。返回 JSON 是 InstallResult（"success"），
            // 旧代码查的 "installed" 字段不存在，装成了也报失败。
            static std::atomic<bool> installing{false};
            if (!installing.exchange(true)) {
                std::thread([&]() {
                    const std::string j = api.installHooks();
                    const wchar_t* msg =
                        j.empty() ? L"安装失败：无法连接后端服务"
                        : (j.find("\"success\":true") != std::string::npos ||
                           j.find("\"success\": true") != std::string::npos)
                            ? L"IDE 集成安装完成"
                            : L"IDE 集成安装失败，请重试";
                    MessageBoxW(nullptr, msg, L"Duty On", MB_OK | MB_ICONINFORMATION);
                    installing = false;
                }).detach();
            }
#endif
        } else if (id == "hook-status") {
#ifdef _WIN32
            // 同上：GET + 弹窗异步化，避免 UI 线程停摆
            static std::atomic<bool> querying{false};
            if (!querying.exchange(true)) {
                std::thread([&]() {
                    const std::string j = api.getHooks();
                    std::wstring msg = j.empty() ? L"无法连接后端服务"
                                                 : L"Hook 状态：\n" +
                                                   std::wstring(j.begin(), j.end());
                    MessageBoxW(nullptr, msg.c_str(), L"Duty On — Hook 状态",
                                MB_OK | MB_ICONINFORMATION);
                    querying = false;
                }).detach();
            }
#endif
        } else if (id == "autostart") {
            api.setAutostart(autostart_cache != 1);
            autostart_cache = api.getAutostart();
        } else if (id == "quit") {
            api.quitApp();  // 后端一起退出
            g_running = false;
        }
    };

    // 监控面板内部操作（↺ 恢复默认 / ▾ 收起）→ 持久化
    ui.on_monitor_action = [&](const std::string& action) {
        if (action == "reset") {
            ui.showMetrics = true;
            ui.monitorCollapsed = false;
            ui.showCpu = ui.showRam = ui.showGpu = true;
            ui.showNet = ui.showSelf = ui.showProjects = true;
        } else if (action == "collapse") {
            ui.monitorCollapsed = !ui.monitorCollapsed;
        }
        sync_monitor_cfg();
    };

    // ---- 窗口层事件接线 ----
    // 右键（角色区 / 托盘）：开/关菜单；菜单矩形内的右键不动作（同 1.x）
    window->on_context_menu = [&](int x, int y) {
        printf("[Menu] context menu requested at (%d, %d), open=%d\n",
               x, y, (int)ui.isMenuOpen());
        if (x >= 0 && ui.isMenuOpen() && ui.isPointInMenu((float)x, (float)y))
            return;
        if (ui.isMenuOpen())
            ui.closeMenu();
        else
            ui.openMenu();
    };
    // 菜单外点击关闭（同 1.x blur 关菜单）
    window->on_outside_click = [&ui]() { ui.closeMenu(); };

    // 边缘吸附：进入时关菜单（1.x maybeEnterEdgeDock 先 closeMenu，菜单会被
    // 吸附条遮住且窗口已收窄）；退出无需处理
    window->on_edge_dock_change = [&](bool docked) {
        if (docked && ui.isMenuOpen()) ui.closeMenu();
    };
    // 吸附条双击空白处 → 退出吸附恢复整窗（1.x dblclick leaveEdgeDock）
    ui.on_undock = [&]() { window->exitEdgeDock(); };

    // 鼠标/键盘/滚轮转发（窗口层 -> ImGui）
    window->mouse_button_cb = [&ui](int button, int action, int mods) {
        ui.forwardMouseButton(button, action, mods);
    };
    window->cursor_pos_cb = [&ui](double x, double y) {
        ui.forwardCursorPos(x, y);
    };
    window->scroll_cb = [&ui](double x, double y) { ui.forwardScroll(x, y); };
    window->key_cb = [&ui](int key, int scancode, int action, int mods) {
        ui.forwardKey(key, scancode, action, mods);
    };

#ifdef _WIN32
    // 角色画布当前左边（menu-left 模式下角色区右锚）
    auto canvas_x_px = [&]() -> int {
        const float base_w = mini_mode ? (float)mini_win_w : (float)win_w;
        const float margin = mini_mode ? 5.0f * ui_scale : 10.0f * ui_scale;
        return (int)(menu_left_active
                         ? (float)window->width() - base_w + margin
                         : margin);
    };
    // 拖拽区域 = 可点击内容区（模型 bounds / 状态栏 / 监控面板，同 1.x
    // wrapper+statusBar+monitorPanel 的 mousedown 拖拽把手）；
    // 菜单区留给菜单交互；吸附模式下整条吸附条都是拖拽把手。
    // 菜单打开时菜单区外的左键按下先关菜单（对齐 1.x DOM 点击关菜单）
    window->hit_test_drag = [&](int x, int y) {
        if (window->isEdgeDocked()) return true;
        if (ui.isMenuOpen()) {
            if (ui.isPointInMenu((float)x, (float)y)) return false;
            ui.closeMenu();
        }
        return ui.isPointClickable((float)x, (float)y);
    };
#endif

    // 项目行点击 -> 前置对应 IDE 窗口（对齐 1.x bringToFront）
    ui.on_project_click = [&pending_bring_to_front](const SessionInfo& sess) {
        pending_bring_to_front = sess.project_name;
    };

    // 调试：DUTYON_AUTO_MENU=1 时启动即打开菜单；DUTYON_MENU_VIEW=<view>
    // 直开指定子菜单视图（绕过鼠标模拟的不确定性）
    const char* auto_menu = getenv("DUTYON_AUTO_MENU");
    const char* menu_view = getenv("DUTYON_MENU_VIEW");
    if ((auto_menu && auto_menu[0] == '1') || (menu_view && menu_view[0])) {
        printf("[Menu] auto-open view=%s (debug)\n",
               menu_view ? menu_view : "main");
        if (menu_view && menu_view[0])
            ui.openMenuView(menu_view);
        else
            ui.openMenu();
    }

    printf("Entering main loop...\n");

    const auto frame_duration = std::chrono::milliseconds(1000 / FPS);
    auto last_frame = Clock::now();

    PetStatus current_status{};
    SysMetrics current_metrics{};
    bool has_metrics = false;
#ifndef _WIN32
    // 最近一次成功收到 PC 状态更新（HTTP 200 + token 有效）的时刻；开机置为
    // 远古表示"尚未连上"。dev_screen 用 (now - last_status_ok) 判定 PC 是否
    // 真在线——区别于"仅 UDP 发现但 token 失效/无响应"（会冻结在旧状态）。
    auto last_status_ok = Clock::now() - std::chrono::hours(1);
    // Wi-Fi 入网瞬间提示：渲染段在截止时刻前显示“Wi-Fi 连接成功”数秒
    bool wifi_was_online = false;
    Clock::time_point wifi_ok_until{};
#endif

#ifdef _WIN32
    // 配对连接成功边沿检测：设备首次带 token 上线（deviceOnline false→true）
    // 时自动隐藏桌宠窗口并弹托盘气泡。false 起始 = PC 重启后设备重连也会触发。
    bool device_was_online = false;
#endif

    // 缩略图后台生成（1.x generateMissingThumbnails 同策略）：启动 ~2s 后
    // 逐个为缺缓存缩略图的模型离屏渲染 128×128 透明 PNG，写入
    // ~/.dutyon/thumbnails/<名>.png（菜单读取）。主线程同 GL 上下文，
    // 节流 0.8s/个避免卡顿；已缓存的模型跳过（只跑一次）。
    struct ThumbGen {
        std::vector<ModelEntry> queue;
        size_t idx = 0;
        float elapsed = 0.f;
        float next_at = 0.f;
        bool queued = false;
    } thumb_gen;

    while (g_running) {
        auto now = Clock::now();
        float delta = std::chrono::duration<float>(now - last_frame).count();
        last_frame = now;

        // 缩略图生成推进（见上方 thumb_gen 声明）
        thumb_gen.elapsed += delta;
        if (!thumb_gen.queued && thumb_gen.elapsed >= 2.0f) {
            thumb_gen.queued = true;
            thumb_gen.next_at = thumb_gen.elapsed;
            for (const auto& e : model_entries)
                if (UserConfigStore::thumbnailFor(e.name).empty())
                    thumb_gen.queue.push_back(e);
        }
        if (thumb_gen.queued && thumb_gen.idx < thumb_gen.queue.size() &&
            thumb_gen.elapsed >= thumb_gen.next_at) {
            thumb_gen.next_at = thumb_gen.elapsed + 0.8f;
            const ModelEntry e = thumb_gen.queue[thumb_gen.idx++];
            if (UserConfigStore::thumbnailFor(e.name).empty()) {
                Live2DRenderer tmp;
                if (tmp.loadModelFile(e.dir, e.json)) {
                    tmp.update(0.033f);
                    if (tmp.captureThumbnailPng(
                            UserConfigStore::thumbnailPathFor(e.name), 128))
                        printf("[Thumb] generated: %s\n", e.name.c_str());
                }
            }
        }

        // 7. 窗口事件（PC: 拖拽/边缘吸附/右键菜单/托盘；返回 false = 退出）
        if (!window->pollEvents()) break;
#ifdef _WIN32
        // /api/quit 或菜单退出请求（HTTP 线程异步置位）
        if (backend.quitRequested()) {
            g_running = false;
            break;
        }
        // 配对连接成功：设备首次带 token 轮询上线 → 自动隐藏桌宠窗口，
        // 右下角托盘气泡提示“我在这里哟”（托盘图标左键可恢复显示）
        {
            const bool online_now = backend.deviceOnline();
            if (online_now && !device_was_online) {
                window->setVisible(false);
                window->showBalloon("Duty On 桌宠", "我在这里哟");
                // 通知设备端播放一次「欢迎」动作 + 专属音频（PC 已隐藏，
                // 桌宠实际在硬件屏上，欢迎在设备端呈现）
                backend.triggerWelcome();
                printf("[Pair] device connected -> hide window + tray balloon + welcome\n");
                // 自动检查程序版本：本机源码哈希 != 设备上报版本 则后台推送
                // 更新（哈希计算放工作线程，主循环零阻塞；未配置仓库静默跳过）
                const std::string repo = cfg.device_repo;
                if (!repo.empty() && !g_syncing.load()) {
                    const std::string dev_ver = backend.deviceVersion();
                    std::thread([repo, dev_ver]() {
                        const std::string src = computeSourceVersion(repo);
                        if (!src.empty() && src != dev_ver) {
                            printf("[Sync] version mismatch (device=%s src=%s)"
                                   " -> auto update\n",
                                   dev_ver.c_str(), src.c_str());
                            launchDeviceSync(repo, src);
                        } else {
                            printf("[Sync] version match (src=%s), skip\n",
                                   src.c_str());
                        }
                    }).detach();
                }
            }
            device_was_online = online_now;
        }
#endif

        // 8. 消费后台轮询结果（非阻塞读取缓存）
#ifndef _WIN32
        // Wi-Fi 状态机推进（内部 1s 节流）：无凭据 AP 配网 <-> 入网切换。
        wifi.poll();
        const WifiState wifi_state = wifi.state();
        // 入网瞬间记一个“Wi-Fi 连接成功”提示的截止时刻（渲染段显示数秒）
        {
            const bool online_now = (wifi_state == WifiState::Online);
            if (online_now && !wifi_was_online)
                wifi_ok_until = Clock::now() + std::chrono::seconds(5);
            wifi_was_online = online_now;
        }
        // 配对握手成功后工作线程产出 token：取走持久化（重启免再配）。
        if (std::string tok = api.takePairToken(); !tok.empty())
            identity.setToken(tok);
        // 仅入网(Online)后广播发现 PC：拿到 base url 喂 ApiClient；未入网/
        // 未发现则清空地址（ApiClient 自动暂停轮询）。发现回包的 paired 标记
        // PC 是否已认得本设备，记入 pc_paired（诊断用）。
        if (wifi_state == WifiState::Online) {
            bool disc_paired = false;
            const auto url = pc_discovery.poll(&disc_paired);
            if (disc_paired) pc_paired = true;
            api.setBaseUrl(url.value_or(""));
        } else {
            api.setBaseUrl("");
        }
        // 连接判定（关键）：只有"Wi-Fi 入网 + 确实在收到 PC 状态"才算已连接。
        // pc_online 以最近一次成功轮询时刻判定，5s 无新状态即视为断开——单靠
        // "UDP 发现 + 本地有 token"不够：token 可能已失效（PC 清除配对→401），
        // 若无条件进 Normal 会冻结在最后一帧（历史"卡在思考中且无声"根因）。
        const bool pc_online =
            (now - last_status_ok) < std::chrono::seconds(5);
        // paired 以 ApiClient 实时 token 为准：启动已用持久化 token 初始化，
        // 已配对设备重启后即为 true（免再配）；PC 侧 401 后 api 自动清 token
        // → false，屏幕回落到配对码引导重新配对（自愈）。
        const bool paired = api.paired();
        // 画面分派：
        //   AP 配网中        -> 配网引导（热点信息大字 + 分步说明）
        //   入网中           -> "正在连接 Wi-Fi"
        //   入网 + PC 在线   -> 正常任务画面（原机器自动连上后切到这里）
        //   入网 + 未连上 PC -> 顶部恒显配对码；已配对="正在等待连接"（原机器
        //                       回来自动切正常页），未配对="没有设备连接"（待输码）
        if (wifi_state == WifiState::ApProvisioning) {
            dev_screen = DevScreen::WifiProvision;
        } else if (wifi_state == WifiState::Joining) {
            dev_screen = DevScreen::JoiningWifi;
        } else if (wifi_state == WifiState::Online && pc_online) {
            dev_screen = DevScreen::Normal;
        } else if (wifi_state == WifiState::Online) {
            dev_screen = paired ? DevScreen::FindingPc : DevScreen::PairCode;
        } else {
            dev_screen = DevScreen::PairCode;  // 无射频/未入网：无从连接 PC
        }
        pc_ready = (dev_screen == DevScreen::Normal);
#endif
        if (auto status = api.takeStatus()) {
            current_status = std::move(*status);
#ifndef _WIN32
            // PC 下发的"重新配网"指令（换 WiFi 场景）：序号变化才执行（ack
            // 已由 ApiClient 发出）——清 Wi-Fi 凭据回配网模式，wifi 状态机
            // 自动把画面切回 WifiProvision；配对 token 保留，新网络入网后
            // 自动恢复连接，无需重新输配对码
            if (!current_status.reset_wifi_cmd.empty() &&
                current_status.reset_wifi_cmd != last_reset_cmd_id) {
                last_reset_cmd_id = current_status.reset_wifi_cmd;
                printf("[Main] reset-wifi cmd %s -> re-provision\n",
                       last_reset_cmd_id.c_str());
                wifi.resetToAp();
            }
            // 收到新状态 = PC 链路活着（HTTP 200 + token 有效）：刷新在线
            // 时刻，供上方 pc_online 判定（下一帧据此切回正常任务画面）
            last_status_ok = Clock::now();
            // 布局模式同步（single/multi/frame；断连后保持最近值）
            if (!current_status.device_mode.empty() &&
                current_status.device_mode != device_mode) {
                const std::string prev_mode = device_mode;
                device_mode = current_status.device_mode;
                printf("[Mode] device mode -> %s\n", device_mode.c_str());
                if (device_mode == "frame") {
                    frame_timer = 0.f;  // 进入相框模式立即从头轮播
                    frame_sig.clear();  // 强制下次轮播段重建并从头播放
                } else if (prev_mode == "frame") {
                    // 离开相框模式：相框期间状态机持续推进但动作被丢弃，
                    // onStatus 已无切换输出 → 按当前状态强制重放动作恢复联动
                    const auto [g, i] = state_machine.currentMotion();
                    if (!g.empty()) {
                        printf("[State] resume from frame -> %s[%d]\n",
                               g.c_str(), i);
                        if (using_gif) {
                            if (gif_char) {
                                const std::string f = gifFileFor(*gif_char, g);
                                if (!f.empty())
                                    gif.load(UserConfigStore::animationsDir() +
                                             "/" + f);
                            }
                        } else {
                            renderer.setLoopMotion(g, i);
                            dev_motion_group = g;
                            dev_motion_idx = i;
                        }
                    }
                }
            }
            // 相框播放源同步（motion/folder；旧版后端不下发时为空=动作轮播）
            {
                const std::string want_src =
                    current_status.frame_source.empty() ? std::string("motion")
                                                        : current_status.frame_source;
                if (want_src != frame_source) {
                    frame_source = want_src;
                    printf("[Mode] frame source -> %s\n", frame_source.c_str());
                }
            }
            // 照片播放启停：仅 frame 模式 + folder 源跑 PhotoPlayer（逐张
            // 流式，设备侧零落盘）。进入/离开都重置动作轮播计时，保证切回
            // motion 时从头轮播。
            const bool want_photo =
                (device_mode == "frame" && frame_source == "folder");
            if (want_photo && !photo_playing) {
                photo_player.start();
                photo_playing = true;
            } else if (!want_photo && photo_playing) {
                photo_player.stop();
                photo_playing = false;
                frame_timer = 0.f;
                frame_sig.clear();
            }
            // 时钟颜色同步（amber/ice/white/green/pink；断连后保持最近值）
            if (!current_status.clock_color.empty() &&
                current_status.clock_color != clock_color) {
                clock_color = current_status.clock_color;
                task_panel.setClockColor(clock_color);
                printf("[Mode] clock color -> %s\n", clock_color.c_str());
            }
            // 亮度同步（10-100）：优先写 sysfs 背光；当前屏无背光接口，
            // 由渲染循环末尾 renderDim 整屏压暗实现
            if (current_status.device_brightness > 0 &&
                current_status.device_brightness != device_brightness) {
                device_brightness = current_status.device_brightness;
                bool wrote_backlight = false;
                std::error_code bec;
                for (auto& e :
                     std::filesystem::directory_iterator("/sys/class/backlight",
                                                         bec)) {
                    int max_b = 0;
                    FILE* mf = fopen((e.path() / "max_brightness").c_str(), "r");
                    if (mf) {
                        if (fscanf(mf, "%d", &max_b) != 1) max_b = 0;
                        fclose(mf);
                    }
                    FILE* f = fopen((e.path() / "brightness").c_str(), "w");
                    if (!f || max_b <= 0) {
                        if (f) fclose(f);
                        continue;
                    }
                    fprintf(f, "%d", device_brightness * max_b / 100);
                    fclose(f);
                    wrote_backlight = true;
                }
                printf("[Mode] brightness -> %d (%s)\n", device_brightness,
                       wrote_backlight ? "backlight" : "software dim");
            }
            // 整屏旋转同步（0/90/180/270；PC 菜单"设备→屏幕旋转"下发）：
            // rotation!=0 时切换逻辑 FBO + 旋转合成；刷新布局尺寸（90/270
            // 交换）使角色/时钟/面板自适应新朝向，横屏竖屏皆可
            if (current_status.screen_rotation != screen_rotation) {
                screen_rotation = current_status.screen_rotation;
                window->setRotation(screen_rotation);
                WIN_W = window->width();
                WIN_H = window->height();
                MODEL_AREA_H = WIN_H / 2;
                // 持久化：作为下次开机的默认朝向（"与上次断开时一致"）
                UserConfigStore::saveScreenRotation(screen_rotation);
                printf("[Mode] screen rotation -> %d (%dx%d)\n",
                       screen_rotation, WIN_W, WIN_H);
            }
            // 左右翻转（镜像）同步：PC 菜单"左右翻转"下发，设备端同步翻转
            // Live2D/GIF 角色与 PC 一致（断连后保持最近值）。翻转在逻辑
            // 场景内进行，与整屏旋转合成正交、可叠加
            if (current_status.flip_horizontal != renderer.isFlipped()) {
                renderer.setFlip(current_status.flip_horizontal);
                gif.setFlip(current_status.flip_horizontal);
                printf("[Mode] flip horizontal -> %s\n",
                       current_status.flip_horizontal ? "on" : "off");
            }
            // 事件提示音：提醒（待确认）播 3 次，开始/结束各 1 次；边沿触发。
            // 对应状态绑定了自定义音频且未静音时，替代内置 beep
            if (snd_seen) {
                // 状态音频就绪判定：未完全静音、该状态未被单独静音、有绑定
                auto has_state_audio = [&](const std::string& st) {
                    if (current_status.sound_mute) return false;
                    for (const auto& m : current_status.sound_muted_states)
                        if (m == st) return false;
                    auto a = current_status.active_audio.find(st);
                    return a != current_status.active_audio.end() &&
                           !a->second.empty();
                };
                if (!snd_prev_confirm && current_status.has_confirmation)
                    sound_player.play(SoundPlayer::Event::Reminder);
                if (snd_prev_overall != current_status.overall_state) {
                    if (current_status.overall_state == "working") {
                        if (!has_state_audio("working"))
                            sound_player.play(SoundPlayer::Event::TaskStart);
                    } else if (snd_prev_overall == "working" &&
                               current_status.overall_state == "sleeping") {
                        if (!has_state_audio("sleeping"))
                            sound_player.play(SoundPlayer::Event::TaskEnd);
                    }
                }
            }
            // 首帧只定基准不发声（防开机误报）；但 overall 基准设为 sleeping，
            // 使"连接/重启时已有活跃任务"在下一帧补播开始提示音——避免服务
            // 重启落在 working 期间导致边沿被吞、全程无声
            if (!snd_seen) {
                snd_prev_overall = "sleeping";
                snd_prev_confirm = current_status.has_confirmation;
                snd_seen = true;
            } else {
                snd_prev_overall = current_status.overall_state;
                snd_prev_confirm = current_status.has_confirmation;
            }
            // 时钟同步：记录 PC 时间与本地单调钟基准，两次轮询间自行推进
            if (current_status.server_time > 0) {
                clock_epoch = current_status.server_time;
                clock_sync_tp = std::chrono::steady_clock::now();
            }
            // 形象与 PC 设定同步：/api/status 随快照下发 activeCharacter
            //（"char_xxx" = 自定义 GIF；否则 Live2D 模型 key）。
            // 覆盖四种切换：GIF→GIF（本地没有则从 PC 下载）、GIF→Live2D、
            // Live2D→GIF、Live2D→Live2D。
            {
                const std::string& ac = current_status.active_character;
                const std::string cur_key =
                    using_gif ? (gif_char ? gif_char->id : std::string())
                              : current_model_key;
                if (!ac.empty() && ac != cur_key) {
                    bool switched = false;
                    if (ac.rfind("char_", 0) == 0) {
                        // 目标是自定义 GIF 形象
                        const CustomCharacter* cc = nullptr;
                        for (const auto& c : cfg.custom_characters)
                            if (c.id == ac) { cc = &c; break; }
                        if (!cc) {
                            // 本地没有该角色：从 PC 拉定义 + 下载动画文件
                            //（阻塞主线程约 1-3s，仅首次发生，可接受）
                            CustomCharacter nc = api.fetchCharacter(ac);
                            if (!nc.id.empty()) {
                                bool ok = true;
                                const std::string files[] = {nc.sleeping,
                                                             nc.working, nc.alert};
                                for (const auto& f : files) {
                                    if (f.empty()) continue;
                                    const std::string dst =
                                        UserConfigStore::animationsDir() + "/" + f;
                                    if (!std::filesystem::exists(dst) &&
                                        !api.downloadAnimation(f, dst)) {
                                        ok = false;
                                        break;
                                    }
                                }
                                if (ok) {
                                    cfg.custom_characters.push_back(nc);
                                    UserConfigStore::saveCustomCharacters(cfg);
                                    cc = &cfg.custom_characters.back();
                                }
                            }
                        }
                        if (cc) {
                            // 切到目标状态的动画（状态不变时也会加载正确文件）
                            const std::string file = gifFileFor(
                                *cc, current_status.overall_state.empty()
                                         ? "sleeping"
                                         : current_status.overall_state);
                            if (!file.empty() &&
                                gif.load(UserConfigStore::animationsDir() + "/" +
                                         file)) {
                                using_gif = true;
                                gif_char = cc;
                                switched = true;
                                apply_state_motions();
                                printf("[Char] sync from PC: GIF %s (%s)\n",
                                       cc->name.c_str(), cc->id.c_str());
                            }
                        }
                    } else {
                        // 目标是 Live2D 模型；用户模型（PC HTTP 键）本地
                        // 没有时先从 PC 同步整套模型文件到 ~/.dutyon/live2d/
                        bool known = false;
                        for (const auto& e : model_entries)
                            if (e.key == ac) { known = true; break; }
                        if (!known && ac.rfind("http://", 0) == 0) {
                            // 失败节流：同一键 10s 内不重复整套拉取
                            //（下载阻塞渲染线程，文件多时卡顿明显）
                            static std::string dl_fail_key;
                            static auto dl_fail_at = Clock::now();
                            const bool throttled =
                                ac == dl_fail_key &&
                                Clock::now() - dl_fail_at <
                                    std::chrono::seconds(10);
                            if (!throttled) {
                                if (auto e = fetchUserModelFromPC(api, ac)) {
                                    model_entries.push_back(std::move(*e));
                                    printf("[Model] synced from PC: %s\n",
                                           ac.c_str());
                                } else {
                                    dl_fail_key = ac;
                                    dl_fail_at = Clock::now();
                                }
                            }
                        }
                        for (const auto& e : model_entries) {
                            if (e.key != ac) continue;
                            if (renderer.loadModelFile(e.dir, e.json)) {
                                using_gif = false;
                                gif_char = nullptr;
                                current_model_key = e.key;
                                apply_state_motions();
                                // 重放当前状态动作，避免切换后停在 idle
                                if (!dev_motion_group.empty())
                                    renderer.setLoopMotion(dev_motion_group,
                                                           dev_motion_idx);
                                switched = true;
                                printf("[Model] sync from PC: %s (%s)\n",
                                       e.name.c_str(), e.key.c_str());
                            }
                            break;
                        }
                    }
                    if (!switched) {
                        // 失败日志 10s 节流（下载尝试本身已有 10s 节流，
                        // 这里避免节流等待期每 0.5s 轮询都刷一条）
                        static std::string last_fail_key;
                        static auto last_fail_at = Clock::now();
                        if (ac != last_fail_key ||
                            Clock::now() - last_fail_at >=
                                std::chrono::seconds(10)) {
                            last_fail_key = ac;
                            last_fail_at = Clock::now();
                            fprintf(stderr, "[Char] sync from PC failed: %s\n",
                                    ac.c_str());
                        }
                    }
                }
            }
            // 状态动作覆盖同步（PC「动作设定」）：变化时重应用并重放当前
            // 状态动作，使设备与 PC 的待机/忙碌/提醒动作保持一致
            if (current_status.state_motions != pc_state_motions) {
                pc_state_motions = current_status.state_motions;
                apply_state_motions();
                if (device_mode != "frame") {
                    const auto [g, i] = state_machine.currentMotion();
                    if (!g.empty()) {
                        printf("[Motion] sync from PC: %s -> %s[%d]\n",
                               current_status.overall_state.c_str(), g.c_str(), i);
                        if (using_gif) {
                            if (gif_char) {
                                const std::string f = gifFileFor(*gif_char, g);
                                if (!f.empty())
                                    gif.load(UserConfigStore::animationsDir() +
                                             "/" + f);
                            }
                        } else {
                            renderer.setLoopMotion(g, i);
                            dev_motion_group = g;
                            dev_motion_idx = i;
                        }
                    }
                }
            }
            // 欢迎：PC 在设备连接边沿递增 welcomeSeq，序号增大即播一次欢迎
            // 动作（一次性）+ 专属音频（未绑定则用系统默认 welcome.wav）。
            // 基准 0：首次连上（序号 ≥ 1）即触发，之后每次重连再触发。
            if (current_status.welcome_seq > dev_welcome_prev) {
                dev_welcome_prev = current_status.welcome_seq;
                printf("[Welcome] play (seq=%lld)\n", dev_welcome_prev);
                if (!using_gif) {
                    const auto [g, i] = state_machine.motionForState("welcome");
                    if (!g.empty()) renderer.playMotion(g, i);
                }
                bool muted = current_status.sound_mute;
                for (const auto& m : current_status.sound_muted_states)
                    if (m == "welcome") muted = true;
                if (!muted) {
                    auto a = current_status.active_audio.find("welcome");
                    if (a != current_status.active_audio.end() &&
                        !a->second.empty()) {
                        const std::string dst =
                            UserConfigStore::animationsDir() + "/" + a->second;
                        if (!std::filesystem::exists(dst))
                            api.downloadAnimation(a->second, dst);
                        sound_player.playFile(dst);
                    } else {
                        sound_player.play(SoundPlayer::Event::Welcome);
                    }
                }
            }
#endif
#ifndef _WIN32
            // 相框模式不响应任务状态（只轮播动作，见主循环 frame 轮播段）
            if (device_mode == "frame") {
                state_machine.onStatus(current_status);  // 仍推进状态机计时
            } else
#endif
            {
                auto [group, idx] = state_machine.onStatus(current_status);
                if (!group.empty()) {
                    printf("[State] %s -> %s[%d]\n",
                           current_status.overall_state.c_str(), group.c_str(),
                           idx);
                    if (using_gif) {
                        // 1.x updateCustomAnimation：按状态切 GIF（带回退链）
                        if (gif_char) {
                            const std::string file = gifFileFor(*gif_char, group);
                            if (!file.empty())
                                gif.load(UserConfigStore::animationsDir() +
                                         "/" + file);
                        }
                    } else {
                        // 状态动作为循环动作（对齐 1.x playStateMotion）
                        renderer.setLoopMotion(group, idx);
#ifndef _WIN32
                        dev_motion_group = group;  // 记录供模型热切换后重放
                        dev_motion_idx = idx;
#endif
                    }
#ifndef _WIN32
                    // 状态音频：切换到的状态绑定了音频且未被静音 → 文件缺失
                    // 时从 PC 补下（首次 ~1s），随后后台线程播放。相框模式
                    // 不进入本分支（不响应任务状态，自然静音）
                    if (!current_status.sound_mute) {
                        bool st_muted = false;
                        for (const auto& m : current_status.sound_muted_states)
                            if (m == current_status.overall_state) {
                                st_muted = true;
                                break;
                            }
                        auto a = current_status.active_audio.find(
                            current_status.overall_state);
                        if (!st_muted && a != current_status.active_audio.end() &&
                            !a->second.empty()) {
                            const std::string dst =
                                UserConfigStore::animationsDir() + "/" +
                                a->second;
                            std::error_code aec;
                            if (!std::filesystem::exists(dst, aec) &&
                                !api.downloadAnimation(a->second, dst)) {
                                fprintf(stderr,
                                        "[Sound] download %s failed\n",
                                        a->second.c_str());
                            } else {
                                sound_player.playFile(dst);
                            }
                        }
                    }
#endif
                }
            }
        }
        if (auto m = api.takeMetrics()) {
            current_metrics = *m;
            has_metrics = true;
            ui.pushMetrics(current_metrics);  // 折线图历史（约 1.5s 一个样本）
        }

#ifdef _WIN32
        // 跨显示器拖动 / 系统缩放变化 → 重算联合缩放（约 0.5s 检测一次；
        // 变化时更新布局变量 + 字体图集，窗口尺寸随下方布局代码自适应）
        {
            static int mon_check = 0;
            if (++mon_check >= 15) {
                mon_check = 0;
                if (recomputeScale()) ui.setScale(ui_scale);
            }
        }

        // 9. 布局注入 + 窗口尺寸自适应（底边固定；菜单打开时向右/向左扩展）
        // 持续上报吸附条内容高（拖拽中的虚线预览框按它取高度）
        window->setDockBarHeightHint((int)ui.dockBarHeight(current_status));
        // 点击穿透区域上报（窗口层独立线程轮询消费，对齐 1.x
        // update_click_regions IPC；菜单打开时整窗强制可点 = force）
        {
            std::vector<IPlatformWindow::ClickRegion> cr;
            for (const auto& r : ui.clickRegions())
                cr.push_back({r.x, r.y, r.w, r.h});
            window->updateClickRegions(cr, ui.isMenuOpen());
        }
        // 菜单内容超出常规窗口高时，窗口向上增高的量（角色/状态栏同步
        // 下移，宠物屏幕位置不变）；仅在下方非吸附分支内计算，其余情况为 0
        float menu_top_offset = 0.0f;
        if (window->isEdgeDocked()) {
            // 边缘吸附条：40px 宽细条，几何由窗口层管理；按内容高度校正
            //（项目数变化时条随之伸缩，垂直中心保持不变）
            window->updateDockBarHeight((int)ui.dockBarHeight(current_status));
        } else {
            const float S = ui_scale;
            const float base_w_f = mini_mode ? (float)mini_win_w : (float)win_w;
            const float canvas_h_f =
                mini_mode ? (float)mini_model_area_h : (float)model_area_h;
            const float canvas_w_f =
                mini_mode ? (float)mini_canvas_w : (float)canvas_w;
            const float margin_x_f = mini_mode ? 5.0f * S : 10.0f * S;
            // 区块间距（角色画布↔状态栏↔监控面板统一）：4px（迷你 2px），
            // 三段视觉间隔一致（2px 用户反馈过紧，4px 为紧凑与呼吸感折中）
            const float gap = mini_mode ? 2.0f * S : 4.0f * S;
            const float extra = ui.menuExtraWidth();

            // 菜单展开方向：右侧工作区空间不足 → 向左展开（1.x menu-left）
            if (extra > 0.0f && !menu_left_active) {
                int wx = 0, wy = 0, wl = 0, wt = 0, wr = 0, wb = 0;
                window->windowPos(wx, wy);
                window->workArea(wl, wt, wr, wb);
                if (wr > wl && wx + (int)(base_w_f + extra) > wr)
                    menu_left_active = true;
            }
            ui.setMenuLeft(menu_left_active);

            // 面板锚定（menu-left 时角色区/面板贴右缘）
            const int win_cur_w = window->width();
            const float panel_x = menu_left_active
                ? (float)win_cur_w - base_w_f + margin_x_f
                : margin_x_f;
            ui.setLayout(canvas_h_f, panel_x, canvas_w_f, gap, mini_mode);

            // 高度：画布 + 间距 + 状态栏（+ 间距 + 监控面板）；
            // 1.x 迷你模式保留半宽状态栏、隐藏监控
            const float mon_h =
                (!mini_mode && ui.showMetrics) ? ui.monitorHeight() : 0.0f;
            const float base_h = canvas_h_f + gap + ui.statusBarHeight() +
                                 (mon_h > 0.0f ? gap + mon_h : 0.0f);
            // 菜单内容（切换形象等长视图）超出常规窗口高 -> 向上增高窗口容纳；
            // 角色视口/状态栏同步下移同量，宠物屏幕位置不变，顶部空间留给菜单
            float target_h = base_h;
            if (const float want = ui.menuDesiredHeight(); want > 0.0f) {
                float need = want + 8.0f * S;  // 内容 + 上下各 4px 边距
                int wl = 0, wt = 0, wr = 0, wb = 0;
                window->workArea(wl, wt, wr, wb);
                if (wb > wt && need > (float)(wb - wt))
                    need = (float)(wb - wt);  // 上限：工作区高，避免超出屏幕
                if (need > base_h) {
                    menu_top_offset = need - base_h;
                    target_h = need;
                }
            }
            ui.setContentTopOffset(menu_top_offset);
            const int target_w = (int)(base_w_f + extra);
            window->resizeKeepBottom(target_w, (int)target_h, menu_left_active);
            // 菜单已收起且窗口回到基础宽 → 退出 menu-left（展开与收回都保持右缘）
            if (extra == 0.0f && window->width() == (int)base_w_f)
                menu_left_active = false;
        }

        // 位置记忆二次校准：首帧布局把窗口高度从估算值（~350）校正为
        // 实际布局高度（底边固定、顶边随之上移）。用最终高度重放一次
        // 恢复：保存值合法时是幂等 no-op；顶边被推出屏外时夹回可见区
        {
            static int restore_replay = 0;
            if (has_restore && ++restore_replay == 5) {
                window->placeAt(restore_x, restore_bottom);
            }
        }
#endif

        // 10. 渲染（边缘吸附时只画吸附条：角色/状态栏/监控/菜单全部隐藏）
        glClearColor(0.f, 0.f, 0.f, 0.f);
        glClear(GL_COLOR_BUFFER_BIT);

        const bool edge_docked = window->isEdgeDocked();
        if (edge_docked) {
            ui.beginFrame();
            ui.renderDockBar(current_status);
            ui.endFrame();
        } else {
#ifdef _WIN32
            // 角色画布（对齐 1.x #canvas-wrapper；迷你 120x130）；
            // GL 视口原点在左下
            {
                GLFWwindow* glfw_win =
                    static_cast<GLFWwindow*>(window->nativeHandle());
                int fb_w = 0, fb_h = 0;
                glfwGetFramebufferSize(glfw_win, &fb_w, &fb_h);
                const float scale =
                    window->height() > 0
                        ? (float)fb_h / (float)window->height()
                        : 1.0f;
                const int cvs_w = mini_mode ? mini_canvas_w : canvas_w;
                const int cvs_h = mini_mode ? mini_model_area_h : model_area_h;
                const int margin_x = canvas_x_px();
                const int canvas_gl_h = static_cast<int>(cvs_h * scale);
                const int canvas_gl_w = static_cast<int>(cvs_w * scale);
                const int margin_gl_x = static_cast<int>(margin_x * scale);
                // 菜单增高窗口时角色整体下移 menu_top_offset（保持屏幕位置不变）
                const int top_off_gl = (int)(menu_top_offset * scale);
                renderer.setViewport(margin_gl_x, fb_h - canvas_gl_h - top_off_gl,
                                     canvas_gl_w, canvas_gl_h);
                gif.setViewport(margin_gl_x, fb_h - canvas_gl_h - top_off_gl,
                                canvas_gl_w, canvas_gl_h);
            }
#else
            // 设备端布局按朝向分两套：
            //   横屏（WIN_W>=WIN_H，rotation 0/180）= 左右布局：人偶占左列
            //     （满高），时钟/日期/任务列表在右列自上而下；
            //   竖屏（rotation 90/270）= 沿用上下布局：角色区 + 底部面板。
            const bool landscape = (WIN_W >= WIN_H);
            const int left_w = landscape ? WIN_H : WIN_W;   // 横屏左列（正方形）宽
            const int region_x = landscape ? left_w : 0;     // 信息区起点 x
            const int region_w = landscape ? (WIN_W - left_w) : WIN_W;
            float panel_h = 0.f;  // 竖屏 multi 面板区高度（横屏不用）
            renderer.setCenterV(true);
            gif.setCenterV(true);
            if (landscape) {
                // 左右布局：人偶占满高左列（列内居中），右列留给时钟/日期/任务
                renderer.setViewport(0, 0, left_w, WIN_H);
                gif.setViewport(0, 0, left_w, WIN_H);
            } else if (device_mode == "multi") {
                // 竖屏多任务：动态分屏（角色区 = 时钟下沿 ~ 面板顶，内容
                // 在该区域内垂直居中）。时钟区预留 ≈ 110px
                panel_h = TaskPanel::heightForSessions(
                    pc_ready ? (int)current_status.sessions.size() : 0);
                const int clock_reserve = 110;
                const int char_h = WIN_H - clock_reserve - (int)panel_h;
                renderer.setViewport(0, (int)panel_h, WIN_W, char_h);
                gif.setViewport(0, (int)panel_h, WIN_W, char_h);
            } else {
                // 竖屏单任务/相框：全屏 + 垂直居中
                renderer.setViewport(0, 0, WIN_W, WIN_H);
                gif.setViewport(0, 0, WIN_W, WIN_H);
            }
            // 相框模式播放：folder=照片逐张流式（PhotoPlayer）/ motion=动作
            // 轮播（现行）。无照片可显（PC 离线 / 首帧未就绪）时兜底跑动作
            // 轮播，避免黑屏。
            photo_showing = false;
            if (device_mode == "frame") {
                if (photo_playing) {
                    photo_player.update(delta);
                    photo_showing = photo_player.hasImage();
                }
                if (!photo_showing) {
                    const std::string sig =
                        using_gif ? (gif_char ? gif_char->id : std::string("?"))
                                  : current_model_key;
                    if (sig != frame_sig) {
                        frame_sig = sig;
                        frame_timer = 0.f;
                        play_frame_motion(0);
                    }
                    frame_timer += delta;
                    if (frame_timer >= 15.f) {
                        frame_timer = 0.f;
                        play_frame_motion(frame_idx + 1);
                    }
                }
            }
#endif

#ifdef _WIN32
            const bool draw_character = true;
#else
            // 设备端：仅"正常任务画面"渲染角色；配网/入网/配对引导画面
            //（WifiProvision/JoiningWifi/PairCode/FindingPc）不画 GIF/模型背景
            // ——开机初始化时角色动画压在配对码/配网引导下显得杂乱。update 照常
            // 推进（状态/包围盒保持新鲜），仅跳过绘制。相框照片显示时同样
            // 不画角色（照片满屏）。
            const bool draw_character =
                (dev_screen == DevScreen::Normal) && !photo_showing;
#endif
            if (using_gif) {
                gif.update(delta);
                if (draw_character) gif.render();
            } else {
                renderer.update(delta);
                if (draw_character) renderer.render();
            }
#ifndef _WIN32
            // 相框照片：满屏 cover 上屏（photo_showing 时角色已抑制）。
            // 主循环末尾的叠加层会重新 glViewport 回全屏，无需此处复位。
            if (photo_showing) photo_player.render(0, 0, WIN_W, WIN_H);
#endif
            // 头顶特效锚定：内容包围盒（视口坐标）→ 窗口客户区坐标。
            // GIF 用 72% 贴底适配后的实际绘制矩形（跟随缩放，而非整个画布区）
            {
                Rect cr = using_gif ? gif.contentRect()
                                    : renderer.contentRect();
#ifdef _WIN32
                cr.x += (float)canvas_x_px();
                // 角色视口下移了 menu_top_offset -> 头顶特效锚点同步下移
                cr.y += menu_top_offset;
#endif
                // Live2D=紧贴内容包围盒；GIF=整图框（含透明留白）
                ui.setModelRect(cr, !using_gif);
            }

            // 11. UI 叠加（迷你模式保留半宽状态栏 + 头顶特效，隐藏监控；
            //     设备端未就绪时不画状态栏，改画配网 QR / 配对码）
#ifdef _WIN32
            // 硬件显示端状态（菜单"设备模式"分组显示/隐藏 + 当前模式勾选）；
            // 颜色传当前角色的有效色（角色专属优先，否则全局），使颜色菜单勾选与之一致
            ui.setDeviceStatus(backend.deviceOnline(), cfg.device_mode,
                               cfg.effectiveColor(cfg.active_character_id),
                               cfg.device_brightness, cfg.screen_rotation);
            // 相框播放源（照片数每次开菜单才用得上：仅在菜单打开时扫目录）
            ui.setFrameSource(cfg.frame_source, cfg.frame_folder,
                              ui.isMenuOpen() ? count_frame_photos(cfg.frame_folder)
                                              : -1);
            ui.beginFrame();
            ui.renderStatus(current_status);
            if (!mini_mode && has_metrics) ui.renderMetrics(current_metrics);
            ui.renderHeadEffect(current_status);
            ui.renderMenu();
            ui.endFrame();
#else
            // 叠加层用全屏坐标系：恢复全屏视口（角色渲染用的半屏视口会影响
            // 后续绘制的 NDC->窗口映射，不复位面板会被压进上半屏）
            glViewport(0, 0, WIN_W, WIN_H);

            // ---- 配网 / 入网 / 配对 引导画面（非正常态，覆盖待机动画）----
            // 文字复用 task_panel：renderClock=数字卡通字体（配对码），
            // renderDate=主字体（含中文提示）。位置按 WIN_H 比例，横竖屏通用。
            if (dev_screen == DevScreen::WifiProvision) {
                // 配网引导：热点 SSID/密码大字（用户要抄的）+ portal 地址 QR。
                // （旧版 WIFI: 凭据串 QR 已废弃——多数扫码器不识别；这里的 QR
                // 只编码 URL，扫码直接打开配网页，是 captive portal 不自动弹
                // 时的保底入口。）横屏：左列热点信息+QR、右列步骤；竖屏单列。
                // 注意：本渲染系 y 轴向上（0=屏底），阅读顺序"上"= 大 y ——
                // 标题在最大 y，步骤自上而下 = y 递减。
                // 字距分支结束务必还原，勿影响时钟/任务页
                task_panel.setSpacing(1.0f);
                // 全页仅两种字号：em=凸显（标题/热点 SSID/密码）、nm=普通
                //（标签/步骤/脚注），层级清晰不琐碎
                const float em = landscape ? 46.f : 52.f;
                const float nm = landscape ? 28.f : 30.f;
                int hx, hw;             // 热点信息区起点 x / 宽
                int sx, sw;             // 步骤文本区起点 x / 宽
                float info_y[5];        // 标题/①/SSID/密码label/密码 的 y（文字顶）
                float s_top;            // 步骤首行 ② 的 y（步骤区最上）
                float foot_y;           // 底部下载指引 y（屏底小字）
                if (landscape) {
                    hx = 0;
                    hw = left_w;
                    sx = region_x;
                    sw = region_w;
                    s_top = 436.f;
                    foot_y = 44.f;
                    info_y[0] = 446.f;
                    info_y[1] = 404.f;
                    info_y[2] = 356.f;
                    info_y[3] = 288.f;
                    info_y[4] = 240.f;
                } else {
                    hx = 0;
                    hw = WIN_W;
                    sx = 0;
                    sw = WIN_W;
                    s_top = 440.f;
                    foot_y = 42.f;
                    info_y[0] = 764.f;
                    info_y[1] = 712.f;
                    info_y[2] = 662.f;
                    info_y[3] = 594.f;
                    info_y[4] = 544.f;
                }
                // ---- ① 热点信息（SSID/密码大字）----
                task_panel.renderDate("设备配网", info_y[0], em, WIN_W, WIN_H,
                                      hx, hw);
                task_panel.renderDate("① 手机连接设备热点", info_y[1], nm,
                                      WIN_W, WIN_H, hx, hw);
                task_panel.renderDate("「" + wifi.apSsid() + "」", info_y[2],
                                      em, WIN_W, WIN_H, hx, hw);
                task_panel.renderDate("热点密码", info_y[3], nm, WIN_W, WIN_H,
                                      hx, hw);
                task_panel.renderDate(wifi.apPass(), info_y[4], em, WIN_W,
                                      WIN_H, hx, hw);
                // ---- ② ~ ⑤ 步骤（自上而下正序：y 从 s_top 递减）----
                // 配网阶段用户往往还没装 PC 端 DutyOn，就地告知去哪下载；
                // renderDate 过宽自动缩字。
                //（QR 已移除：实测手机连热点后会被 MIUI 自动回切到家庭路由，
                //  扫码/浏览器路径全部失效，唯一可靠入口是 WiFi 设置页的
                //  captive portal 入口，QR 无作用）
                const std::string steps[] = {
                    "② 连接后自动弹出配网页",
                    "未弹出时浏览器打开 " + wifi.portalUrl(),
                    "③ 选 Wi-Fi 输密码提交",
                    "④ 联网后屏幕显示配对码",
                    "⑤ 电脑端右键宠物头像",
                    "    设备 → 配对设备 输码",
                };
                const int n = (int)(sizeof(steps) / sizeof(steps[0]));
                // 行距：普通字号 1.5 倍行高
                const float s_gap = nm * 1.5f;
                for (int i = 0; i < n; ++i)
                    task_panel.renderDate(steps[i], s_top - s_gap * (float)i,
                                          nm, WIN_W, WIN_H, sx, sw);
                // 底部脚注：软件下载指引（直接给仓库地址 —— 按名字在 Gitee/
                // GitHub 搜索实测搜不到本仓库，搜索式引导误导用户；Gitee 在前
                // 便于国内访问）
                task_panel.renderDate(
                    "下载 gitee.com/megrezsoft/duty-on",
                    foot_y, nm, WIN_W, WIN_H, sx, sw);
                task_panel.setSpacing(1.0f);  // 还原字距（勿影响其他画面）
            } else if (dev_screen == DevScreen::JoiningWifi) {
                task_panel.renderDate("正在连接 Wi-Fi…", (float)WIN_H * 0.60f,
                                      44.f, WIN_W, WIN_H);
            } else if (dev_screen == DevScreen::PairCode ||
                       dev_screen == DevScreen::FindingPc) {
                // 已入 Wi-Fi 但尚未连上 PC：顶部恒显 6 位配对码，底部状态区分
                //   未配对(PairCode)  ="没有设备连接" + 提示在 PC 端输入此码；
                //   已配对(FindingPc) ="正在等待连接" + 原机器上线后自动切正常页。
                // 已配对仍显示码：PC 若清除了配对，用户可据此直接重配（兜底）。
                const bool waiting = (dev_screen == DevScreen::FindingPc);
                const float code_top = (float)WIN_H * 0.70f;
                task_panel.renderDate("配对码", (float)WIN_H * 0.80f, 34.f,
                                      WIN_W, WIN_H);
                task_panel.renderClock(identity.pairCode(), code_top, 96.f,
                                       WIN_W, WIN_H);
                task_panel.renderDate(waiting ? "正在等待连接" : "没有设备连接",
                                      code_top - 132.f, 38.f, WIN_W, WIN_H);
                if (waiting) {
                    task_panel.renderDate("电脑端 DutyOn 启动后将自动连接",
                                          code_top - 190.f, 26.f, WIN_W, WIN_H);
                } else {
                    // 输码入口写明 PC 菜单路径，分两行防缩字过小
                    task_panel.renderDate("电脑端 DutyOn：右键宠物头像",
                                          code_top - 190.f, 26.f, WIN_W, WIN_H);
                    task_panel.renderDate("设备 → 配对设备 输入此配对码",
                                          code_top - 228.f, 26.f, WIN_W, WIN_H);
                }
                // Wi-Fi 刚入网数秒内顶部提示“连接成功”（此后由右上角 Wi-Fi
                // 信号条常绿表示已入网；是否连上 PC 由旁边的显示器图标表示）
                if (Clock::now() < wifi_ok_until)
                    task_panel.renderDate("✓ Wi-Fi 连接成功",
                                          (float)WIN_H * 0.90f, 30.f, WIN_W, WIN_H);
            }

            // ---- 时钟/日期/任务列表纵向位置（横屏右列 vs 竖屏上下）----
            const float clock_size =
                (device_mode == "multi") ? 56.f : 92.f;
            const float clock_lh = task_panel.clockLineHeight(clock_size);
            const float date_size = 30.f;
            const float date_lh = date_size * 1.35f;
            float clock_top, date_top, task_top;
            if (landscape) {
                // 横屏右列自上而下：时钟 → 日期 → 任务列表
                clock_top = (float)WIN_H - 40.f;
                date_top = clock_top - clock_lh - 14.f;
                task_top = date_top - date_lh - 18.f;
            } else {
                // 竖屏：时钟贴顶（multi 稍下沉避让角色头顶）；日期贴屏幕底；
                // 任务面板顶边 = 面板高度（卡片底贴屏幕底）
                clock_top = (float)WIN_H -
                            ((device_mode == "multi") ? 26.f : 22.f);
                date_top = date_lh + 14.f;
                task_top = panel_h;
            }

            // ---- 任务列表（multi；仅正常画面且已就绪）----
            if (dev_screen == DevScreen::Normal && device_mode == "multi" && pc_ready)
                task_panel.render(current_status, WIN_W, WIN_H, task_top,
                                  region_x, region_w);

            // ---- 时钟 + 日期文本（仅正常画面；优先 PC 下发时间，设备本地
            //      钟不可信，断连兜底本地时间）。相框照片满屏时隐藏，避免叠字
            if (dev_screen == DevScreen::Normal && !photo_showing) {
                char time_buf[16] = {};
                char date_buf[40] = {};
                static const char* kWeek[] = {"日", "一", "二", "三",
                                              "四", "五", "六"};
                time_t local_sec = 0;  // 本地日期用（含时区偏移的 epoch）
                if (clock_epoch > 0) {
                    const double t =
                        clock_epoch +
                        std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - clock_sync_tp)
                            .count() +
                        current_status.utc_offset_min * 60.0;
                    const long secs = ((long)t % 86400 + 86400) % 86400;
                    snprintf(time_buf, sizeof(time_buf), "%02ld:%02ld:%02ld",
                             secs / 3600, secs / 60 % 60, secs % 60);
                    local_sec = (time_t)t;
                } else {
                    local_sec = time(nullptr);
                    strftime(time_buf, sizeof(time_buf), "%H:%M:%S",
                             localtime(&local_sec));
                }
                // 日期（年月日+星期）：local_sec 已是含时区偏移的 epoch，
                // 用 gmtime 取其"本地"日历分量
                struct tm tmv = {};
                gmtime_r(&local_sec, &tmv);
                snprintf(date_buf, sizeof(date_buf),
                         "%d年%d月%d日 星期%s", tmv.tm_year + 1900,
                         tmv.tm_mon + 1, tmv.tm_mday, kWeek[tmv.tm_wday]);

                task_panel.renderClock(time_buf, clock_top, clock_size,
                                       WIN_W, WIN_H, region_x, region_w);
                // 日期：横屏右列恒显示（时钟下方）；竖屏仅单任务/相框
                //（竖屏 multi 底部是任务列表，会重叠）
                if (landscape || device_mode != "multi") {
                    task_panel.renderDate(date_buf, date_top, date_size,
                                          WIN_W, WIN_H, region_x, region_w);
                }
            }
            // 右上角连接状态图标（两段链路分开）：Wi-Fi 信号条=设备是否入网
            //（入网绿/未入网红），旁边显示器图标=是否已连上 PC（连上绿/未连灰）
            task_panel.renderNetStatus(wifi.state() == WifiState::Online,
                                       pc_ready, WIN_W, WIN_H);
            // 软件亮度：整屏压暗叠层（brightness<100 时生效）
            task_panel.renderDim(device_brightness, WIN_W, WIN_H);
#endif
        }

        // ---- 一次性 GL 诊断（第 90 帧左右，稳定后）----
        {
            static int diag_frame = 0;
            if (++diag_frame == 90) {
#ifdef _WIN32
                GLFWwindow* gw = static_cast<GLFWwindow*>(window->nativeHandle());
                int fw = 0, fh = 0;
                glfwGetFramebufferSize(gw, &fw, &fh);
#else
                // 设备端无 GLFW：全屏 framebuffer，尺寸即窗口尺寸
                const int fw = window->width(), fh = window->height();
#endif
                printf("[GLDiag] renderer=%s\n", glGetString(GL_RENDERER));
                int tex2d_en = 0;
#ifdef _WIN32
                // GLES 下 GL_TEXTURE_2D 不是 glIsEnabled 的合法枚举（误报 0x500）
                tex2d_en = glIsEnabled(GL_TEXTURE_2D);
#endif
                printf("[GLDiag] fb=%dx%d scissor=%d blend=%d depth=%d cull=%d tex2d=%d\n",
                       fw, fh,
                       glIsEnabled(GL_SCISSOR_TEST), glIsEnabled(GL_BLEND),
                       glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_CULL_FACE),
                       tex2d_en);
                GLint sci[4] = {0, 0, 0, 0};
                glGetIntegerv(GL_SCISSOR_BOX, sci);
                printf("[GLDiag] scissorBox=(%d,%d,%d,%d)\n", sci[0], sci[1], sci[2],
                       sci[3]);
                printf("[GLDiag] glError=0x%x\n", glGetError());
                // 读回帧缓冲采样点（画布中心/画布上部/面板中心/窗口左上角）
                struct P { int x, y; const char* tag; };
                const P pts[] = {{fw / 2, fh - fh / 5, "canvas-upper"},
                                 {fw / 2, fh / 2, "canvas-mid"},
                                 {fw / 2, fh / 20, "panel"},
                                 {5, fh - 5, "corner"}};
                for (const auto& pt : pts) {
                    GLubyte px[4] = {0, 0, 0, 0};
                    glReadPixels(pt.x, pt.y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    printf("[GLDiag] %-13s (%4d,%4d) RGBA=(%3d,%3d,%3d,%3d)\n", pt.tag,
                           pt.x, pt.y, px[0], px[1], px[2], px[3]);
                }
#ifndef _WIN32
                // 时钟区诊断：扫顶部 120px 高的中央条，统计非透明/青色像素
                {
                    int cyan = 0, dark = 0, opaque = 0, total = 0;
                    for (int y = fh - 120; y < fh; y += 6)
                        for (int x = fw / 2 - 100; x < fw / 2 + 100; x += 6) {
                            GLubyte px[4] = {0, 0, 0, 0};
                            glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
                            total++;
                            if (px[3] > 20) opaque++;
                            if (px[2] > 150 && px[1] > 100) cyan++;      // 青蓝
                            if (px[0] < 40 && px[1] < 40 && px[2] < 60) dark++;  // 深背板
                        }
                    printf("[GLDiag] clock-area: total=%d opaque=%d cyan=%d dark=%d\n",
                           total, opaque, cyan, dark);
                }
#endif
            }
        }

        // ---- 调试自动化钩子（环境变量触发；正常发布无副作用）----
        //   DUTYON_SNAPSHOT=<前缀>  第 90 帧起每 120 帧存一帧 BMP（含 alpha）
        //   DUTYON_MENU=<view>      第 60 帧打开指定菜单视图（models/main/...）
        //   DUTYON_DOCK=<0|1>       第 60 帧进入左/右缘吸附
        //   DUTYON_MEM=1            第 150 帧打印内存构成诊断
        {
            static const char* snap_env = getenv("DUTYON_SNAPSHOT");
            static const char* menu_env = getenv("DUTYON_MENU");
            static const char* dock_env = getenv("DUTYON_DOCK");
            static const char* mem_env = getenv("DUTYON_MEM");
            static int dbg_frame = 0;
            ++dbg_frame;
#ifdef _WIN32
            if (mem_env && *mem_env && dbg_frame == 150) dutyon::DumpMemoryComposition();
#endif
            if (menu_env && *menu_env && dbg_frame == 60) {
                printf("[Debug] open menu view: %s\n", menu_env);
                ui.openMenuView(menu_env);
            }
            if (dock_env && *dock_env && dbg_frame == 60) {
                printf("[Debug] enter edge dock: %s\n", dock_env);
                window->debugEnterDock(atoi(dock_env));
            }
            if (snap_env && *snap_env && dbg_frame >= 90 &&
                (dbg_frame - 90) % 120 == 0) {
#ifdef _WIN32
                GLFWwindow* gw = static_cast<GLFWwindow*>(window->nativeHandle());
                int fw = 0, fh = 0;
                glfwGetFramebufferSize(gw, &fw, &fh);
#else
                // 旋转激活时：重新合成到 FB0 并按物理尺寸回读（捕捉上屏
                // 旋转后的实际内容）；直出时物理 = 逻辑
                int fw, fh;
                if (window->logicalActive()) {
                    window->presentComposite();
                    fw = window->physWidth();
                    fh = window->physHeight();
                } else {
                    fw = window->width();
                    fh = window->height();
                }
#endif
                std::vector<unsigned char> px((size_t)fw * fh * 4);
                glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                // 32 位 BMP：BGRA、行序自底向上（与 GL 原点一致），保留 alpha
                char path[512];
                snprintf(path, sizeof(path), "%s.%d.bmp", snap_env, dbg_frame);
                FILE* f = fopen(path, "wb");
                if (f) {
                    const unsigned header_size = 14 + 40;
                    const unsigned data_size = (unsigned)(fw * fh * 4);
                    unsigned char fh14[14] = {'B', 'M', 0};
                    *(unsigned*)&fh14[2] = header_size + data_size;
                    *(unsigned*)&fh14[10] = header_size;
                    unsigned char ih40[40] = {0};
                    *(int*)&ih40[0] = 40;
                    *(int*)&ih40[4] = fw;
                    *(int*)&ih40[8] = fh;
                    *(short*)&ih40[12] = 1;
                    *(short*)&ih40[14] = 32;
                    fwrite(fh14, 1, 14, f);
                    fwrite(ih40, 1, 40, f);
                    std::vector<unsigned char> row((size_t)fw * 4);
                    for (int y = 0; y < fh; y++) {
                        for (int x = 0; x < fw; x++) {
                            row[x * 4 + 0] = px[((size_t)y * fw + x) * 4 + 2];
                            row[x * 4 + 1] = px[((size_t)y * fw + x) * 4 + 1];
                            row[x * 4 + 2] = px[((size_t)y * fw + x) * 4 + 0];
                            row[x * 4 + 3] = px[((size_t)y * fw + x) * 4 + 3];
                        }
                        fwrite(row.data(), 1, row.size(), f);
                    }
                    fclose(f);
                    printf("[Debug] snapshot -> %s (%dx%d)\n", path, fw, fh);
                }
            }
        }

        // ---- 位置记忆（1.x Moved 事件持久化的轮询版）----
        // 每秒检查：位置变化且非吸附态（吸附位置不记忆，同 1.x）、菜单
        // 收起（menu-left 模式左缘临时左移，非稳定值）才写盘。锚=底边：
        // 所有运行期尺寸变化都保持底边不动，任意时刻读到的底边都有效
#ifdef _WIN32
        {
            static int pos_tick = 0;
            static bool have_last = false;
            static int last_x = 0, last_b = 0;
            if (++pos_tick >= 30) {
                pos_tick = 0;
                if (!window->isEdgeDocked() && !ui.isMenuOpen()) {
                    int wx = 0, wy = 0;
                    window->windowPos(wx, wy);
                    const int bottom = wy + window->height();
                    if (!have_last || wx != last_x || bottom != last_b) {
                        have_last = true;
                        last_x = wx;
                        last_b = bottom;
                        UserConfigStore::saveWindowPos(wx, bottom);
                    }
                }
            }
        }
#endif

        window->swapBuffers();

        // 项目行点击处理（帧外执行，同步 HTTP 不卡 UI 帧）
        if (!pending_bring_to_front.empty()) {
            api.bringToFront(pending_bring_to_front);
            pending_bring_to_front.clear();
        }

        // 12. 帧率控制
        auto elapsed = Clock::now() - now;
        if (elapsed < frame_duration) {
            std::this_thread::sleep_for(frame_duration - elapsed);
        }
    }

    // 退出前最终保存位置（兜住最后一秒内的移动；吸附态不保存，同 1.x）
#ifdef _WIN32
    if (!window->isEdgeDocked() && !ui.isMenuOpen()) {
        int wx = 0, wy = 0;
        window->windowPos(wx, wy);
        UserConfigStore::saveWindowPos(wx, wy + window->height());
    }
#endif

    printf("Shutting down...\n");
#ifdef _WIN32
    backend.stop();
#else
    // 相框照片：显式停后台线程 + 删纹理（GL 上下文尚当前；栈析构再调一次无感）
    photo_player.stop();
#endif
    ui.shutdown();
    Live2DRenderer::frameworkDispose();
    window->shutdown();
    delete window;
    return 0;
}
