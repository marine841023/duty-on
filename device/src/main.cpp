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
#include <thread>

// GL 函数声明：PC 用 GLEW（桌面 OpenGL），设备用 GLES3
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <GL/glew.h>
#include <GLFW/glfw3.h>  // glfwGetFramebufferSize（视口 DPI 换算）
#else
#include <GLES3/gl3.h>
#endif

#include <algorithm>
#include <filesystem>
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
    const char* api_url = kApiBaseUrl;
    const int WIN_W = kDisplayWidth;
    const int MODEL_AREA_H = kDisplayHeight;
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
        // 低分屏下限 0.85（不严格按比例缩放）：1080p@100% 纯比例只有
        // 0.75，文字被压到 8~11px，中文即使 FreeType hinting 也难以辨认
        // （用户反馈"还是不够清晰"）。0.85 时四档字号 13/12/11/10px，
        // 主字号达到 13px 灰度 AA 光滑渲染阈值；窗口/角色稍大可接受。
        if (f < 0.85f) f = 0.85f;
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
#endif

    std::vector<ModelEntry> model_entries = UserConfigStore::listModels(builtin_roots);
    std::string current_model_key;  // 1.x activeCharacterId（Live2D 模型唯一键）
    {
        // 自定义形象优先（1.x getCharacters：active 匹配 custom id）
        for (const auto& c : cfg.custom_characters) {
            if (c.id == cfg.active_character_id && !c.id.empty()) {
                using_gif = true;
                gif_char = &c;
                const std::string file =
                    gifFileFor(c, "sleeping");  // 初始状态 sleeping
                if (!file.empty() &&
                    gif.load(UserConfigStore::animationsDir() + "\\" + file)) {
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
    ApiClient api(api_url);
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
    };
    apply_state_motions();

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
        return false;
    };

    ui.menu_hint = [&](const std::string& id) -> std::string {
        if (id == "hook-status") return hook_hint_cache;
        if (id.rfind("assign:", 0) == 0) {
            auto [g, i] = state_machine.motionForState(id.substr(7));
            return g + "[" + std::to_string(i) + "]";
        }
        return {};
    };

    // key: "models"=切换形象；"motions"=播放动作；"motions:<状态>"=动作设定
    ui.menu_collect = [&](const std::string& key) -> std::vector<UIRenderer::MenuEntry> {
        if (key == "models") {
            std::vector<UIRenderer::MenuEntry> out;
            // 1.x getCharacters 顺序：内置模型在前，自定义形象在后；
            // 缩略图：Live2D 用 1.x 缓存（~/.dutyon/thumbnails/<名>.png），
            // GIF 形象用 sleeping 动画首帧（stb 解码）
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
                    me.thumb = UserConfigStore::animationsDir() + "\\" + f;
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
                    !gif.load(UserConfigStore::animationsDir() + "\\" + file)) {
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
                            gif.load(UserConfigStore::animationsDir() + "\\" + file);
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
            std::string j = api.installHooks();
            const wchar_t* msg = j.empty()
                ? L"安装失败：无法连接后端服务"
                : (j.find("\"installed\":true") != std::string::npos ||
                   j.find("\"installed\": true") != std::string::npos)
                    ? L"IDE 集成安装完成"
                    : L"IDE 集成安装失败，请重试";
            MessageBoxW(nullptr, msg, L"Duty On", MB_OK | MB_ICONINFORMATION);
            refresh_menu_caches();
#endif
        } else if (id == "hook-status") {
#ifdef _WIN32
            std::string j = api.getHooks();
            std::wstring msg = j.empty() ? L"无法连接后端服务"
                                         : L"Hook 状态：\n" +
                                           std::wstring(j.begin(), j.end());
            MessageBoxW(nullptr, msg.c_str(), L"Duty On — Hook 状态",
                        MB_OK | MB_ICONINFORMATION);
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

    while (g_running) {
        auto now = Clock::now();
        float delta = std::chrono::duration<float>(now - last_frame).count();
        last_frame = now;

        // 7. 窗口事件（PC: 拖拽/边缘吸附/右键菜单/托盘；返回 false = 退出）
        if (!window->pollEvents()) break;
#ifdef _WIN32
        // /api/quit 或菜单退出请求（HTTP 线程异步置位）
        if (backend.quitRequested()) {
            g_running = false;
            break;
        }
#endif

        // 8. 消费后台轮询结果（非阻塞读取缓存）
        if (auto status = api.takeStatus()) {
            current_status = std::move(*status);
            auto [group, idx] = state_machine.onStatus(current_status);
            if (!group.empty()) {
                printf("[State] %s -> %s[%d]\n",
                       current_status.overall_state.c_str(), group.c_str(), idx);
                if (using_gif) {
                    // 1.x updateCustomAnimation：按状态切 GIF（带回退链）
                    if (gif_char) {
                        const std::string file = gifFileFor(*gif_char, group);
                        if (!file.empty())
                            gif.load(UserConfigStore::animationsDir() + "\\" + file);
                    }
                } else {
                    // 状态动作为循环动作（对齐 1.x playStateMotion）
                    renderer.setLoopMotion(group, idx);
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
            float target_h = canvas_h_f + gap + ui.statusBarHeight();
            if (mon_h > 0.0f) target_h += gap + mon_h;
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
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
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
                renderer.setViewport(margin_gl_x, fb_h - canvas_gl_h,
                                     canvas_gl_w, canvas_gl_h);
                gif.setViewport(margin_gl_x, fb_h - canvas_gl_h,
                                canvas_gl_w, canvas_gl_h);
            }
#else
            // 设备端：整屏渲染
            renderer.setViewport(0, 0, WIN_W, MODEL_AREA_H);
#endif

            if (using_gif) {
                gif.update(delta);
                gif.render();
            } else {
                renderer.update(delta);
                renderer.render();
            }

            // 头顶特效锚定：内容包围盒（视口坐标）→ 窗口客户区坐标。
            // GIF 用 72% 贴底适配后的实际绘制矩形（跟随缩放，而非整个画布区）
            {
                Rect cr = using_gif ? gif.contentRect()
                                    : renderer.contentRect();
#ifdef _WIN32
                cr.x += (float)canvas_x_px();
#endif
                // Live2D=紧贴内容包围盒；GIF=整图框（含透明留白）
                ui.setModelRect(cr, !using_gif);
            }

            // 11. UI 叠加（迷你模式保留半宽状态栏 + 头顶特效，隐藏监控）
            ui.beginFrame();
            ui.renderStatus(current_status);
            if (!mini_mode && has_metrics) ui.renderMetrics(current_metrics);
            ui.renderHeadEffect(current_status);
            ui.renderMenu();
            ui.endFrame();
        }

        // ---- 一次性 GL 诊断（第 90 帧左右，稳定后）----
        {
            static int diag_frame = 0;
            if (++diag_frame == 90) {
                GLFWwindow* gw = static_cast<GLFWwindow*>(window->nativeHandle());
                int fw = 0, fh = 0;
                glfwGetFramebufferSize(gw, &fw, &fh);
                printf("[GLDiag] renderer=%s\n", glGetString(GL_RENDERER));
                printf("[GLDiag] fb=%dx%d scissor=%d blend=%d depth=%d cull=%d tex2d=%d\n",
                       fw, fh,
                       glIsEnabled(GL_SCISSOR_TEST), glIsEnabled(GL_BLEND),
                       glIsEnabled(GL_DEPTH_TEST), glIsEnabled(GL_CULL_FACE),
                       glIsEnabled(GL_TEXTURE_2D));
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
                GLFWwindow* gw = static_cast<GLFWwindow*>(window->nativeHandle());
                int fw = 0, fh = 0;
                glfwGetFramebufferSize(gw, &fw, &fh);
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
#endif
    ui.shutdown();
    Live2DRenderer::frameworkDispose();
    window->shutdown();
    delete window;
    return 0;
}
