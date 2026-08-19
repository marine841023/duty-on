// Duty On 2.0 — 全平台原生入口
//
// PC (Windows):  GLFW 透明窗口 + OpenGL 3.3 + ImGui 监控面板
// ARM Linux:     EGL/GLES2 framebuffer 直渲 + headless 占位 UI
//
// 两种形态共用同一套：API 轮询 -> 状态机 -> Live2D 渲染 -> UI 叠加。
// 浏览器/WebView 已彻底退出。

#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

// GL 函数声明：PC 用 GLEW（桌面 OpenGL），设备用 GLES3
#ifdef _WIN32
#include <GL/glew.h>
#else
#include <GLES3/gl3.h>
#endif

#include "config.h"
#include "api/client.h"
#include "platform/window.h"
#include "render/live2d_renderer.h"
#include "state/machine.h"
#include "ui/ui_renderer.h"

using namespace dutyon;
using Clock = std::chrono::steady_clock;

static volatile bool g_running = true;

void signalHandler(int) {
    g_running = false;
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

#ifdef _WIN32
    const char* platform = "Windows (GLFW/OpenGL)";
    const char* api_url = "http://127.0.0.1:17521";
    constexpr int WIN_W = 480, WIN_H = 640;
    constexpr int FPS = 30;
#else
    const char* platform = "ARM Linux (EGL/GLES2)";
    const char* api_url = kApiBaseUrl;
    constexpr int WIN_W = kDisplayWidth, WIN_H = kDisplayHeight;
    constexpr int FPS = kTargetFps;
#endif

    printf("Duty On 2.0 — %s\n", platform);
    printf("API: %s | Window: %dx%d @ %dfps\n", api_url, WIN_W, WIN_H, FPS);

    // 1. 平台窗口（PC: GLFW 透明窗口 / 设备: EGL framebuffer）
    IPlatformWindow* window = createPlatformWindow();
    if (!window->init(WIN_W, WIN_H)) {
        fprintf(stderr, "Window init failed\n");
        delete window;
        return 1;
    }

    // 2. Cubism Framework（进程级一次）
    if (!Live2DRenderer::frameworkInit()) {
        fprintf(stderr, "Cubism Framework init failed\n");
        window->shutdown();
        delete window;
        return 1;
    }

    // 3. Live2D 模型
    Live2DRenderer renderer;
    renderer.setViewport(0, 0, WIN_W, WIN_H);
#ifdef _WIN32
    const char* model_dir = "models";   // 相对路径，开发时方便
#else
    const char* model_dir = kModelDir;
#endif
    if (!renderer.loadModel(model_dir, kDefaultModel)) {
        fprintf(stderr, "Model load failed (%s/%s) — 继续运行（无角色）\n",
                model_dir, kDefaultModel);
    }

    // 4. UI 叠加层（PC: ImGui / 设备: 占位）
    UIRenderer ui;
#ifdef _WIN32
    ui.init(static_cast<GLFWwindow*>(window->nativeHandle()));
#else
    ui.init(WIN_W, WIN_H);
#endif

    // 5. API 客户端 + 状态机
    ApiClient api(api_url);
    StateMachine state_machine;

    auto [init_group, init_idx] = state_machine.currentMotion();
    renderer.playMotion(init_group, init_idx);

    printf("Entering main loop...\n");

    const auto frame_duration = std::chrono::milliseconds(1000 / FPS);
    auto last_status_poll = Clock::now() - std::chrono::milliseconds(kPollIntervalMs);
    auto last_metrics_poll = Clock::now();
    auto last_frame = Clock::now();

    PetStatus current_status{};
    SysMetrics current_metrics{};
    bool has_metrics = false;

    while (g_running) {
        auto now = Clock::now();
        float delta = std::chrono::duration<float>(now - last_frame).count();
        last_frame = now;

        // 6. 窗口事件（PC: 拖拽/右键菜单/托盘；返回 false = 退出）
        if (!window->pollEvents()) break;

        // 7. 状态轮询（~2Hz）
        if (now - last_status_poll >= std::chrono::milliseconds(kPollIntervalMs)) {
            last_status_poll = now;
            if (auto status = api.poll()) {
                current_status = *status;
                auto [group, idx] = state_machine.onStatus(*status);
                if (!group.empty()) {
                    printf("[State] %s -> %s[%d]\n",
                           status->overall_state.c_str(), group.c_str(), idx);
                    renderer.playMotion(group, idx);
                }
            }
        }

        // 8. 监控数据轮询（~0.7Hz，与 Rust 采样 1.5s 错开）
        if (ui.showMetrics &&
            now - last_metrics_poll >= std::chrono::milliseconds(1500)) {
            last_metrics_poll = now;
            if (auto m = api.pollMetrics()) {
                current_metrics = *m;
                has_metrics = true;
            }
        }

        // 9. 渲染
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);  // 全透明背景
        glClear(GL_COLOR_BUFFER_BIT);

        renderer.update(delta);
        renderer.render();

        // 10. UI 叠加
        ui.beginFrame();
        ui.renderStatus(current_status);
        if (has_metrics) ui.renderMetrics(current_metrics);
        ui.endFrame();

        window->swapBuffers();

        // 11. 帧率控制
        auto elapsed = Clock::now() - now;
        if (elapsed < frame_duration) {
            std::this_thread::sleep_for(frame_duration - elapsed);
        }
    }

    printf("Shutting down...\n");
    ui.shutdown();
    Live2DRenderer::frameworkDispose();
    window->shutdown();
    delete window;
    return 0;
}
