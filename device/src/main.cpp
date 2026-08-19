#include <chrono>
#include <csignal>
#include <cstdio>
#include <thread>

#include "config.h"
#include "api/client.h"
#include "render/gles_context.h"
#include "render/live2d_renderer.h"
#include "state/machine.h"

using namespace dutyon;
using Clock = std::chrono::steady_clock;

static volatile bool g_running = true;

void signalHandler(int) {
    g_running = false;
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    printf("Duty On Device v2.0.0 (Native Live2D)\n");
    printf("API: %s | Display: %dx%d @ %dfps\n",
           kApiBaseUrl, kDisplayWidth, kDisplayHeight, kTargetFps);

    // 1. 初始化 OpenGL ES 上下文（无窗口系统，直渲 framebuffer）
    GlesContext gles;
    if (!gles.init(kDisplayWidth, kDisplayHeight)) {
        fprintf(stderr, "Failed to initialize GLES context\n");
        return 1;
    }

    // 2. 初始化 Cubism Framework（进程级一次）
    if (!Live2DRenderer::frameworkInit()) {
        fprintf(stderr, "Failed to initialize Cubism Framework\n");
        return 1;
    }

    // 3. 加载 Live2D 模型
    Live2DRenderer renderer;
    renderer.setViewport(0, 0, kDisplayWidth, kDisplayHeight);
    if (!renderer.loadModel(kModelDir, kDefaultModel)) {
        fprintf(stderr, "Failed to load Live2D model %s/%s\n",
                kModelDir, kDefaultModel);
        // 不退出：允许无模型运行（仅日志状态输出）
    }

    // 4. API 客户端 + 状态机
    ApiClient api(kApiBaseUrl);
    StateMachine state_machine;

    auto [init_group, init_idx] = state_machine.currentMotion();
    renderer.playMotion(init_group, init_idx);

    printf("Entering main loop...\n");

    const auto frame_duration = std::chrono::milliseconds(1000 / kTargetFps);
    auto last_poll = Clock::now() - std::chrono::milliseconds(kPollIntervalMs);
    auto last_frame = Clock::now();

    while (g_running) {
        auto now = Clock::now();
        float delta = std::chrono::duration<float>(now - last_frame).count();
        last_frame = now;

        // 5. 定时轮询桌面端状态
        if (now - last_poll >= std::chrono::milliseconds(kPollIntervalMs)) {
            last_poll = now;
            if (auto status = api.poll()) {
                auto [group, idx] = state_machine.onStatus(*status);
                if (!group.empty()) {
                    printf("[State] %s -> %s[%d]\n",
                           status->overall_state.c_str(), group.c_str(), idx);
                    renderer.playMotion(group, idx);
                }
            }
        }

        // 6. 渲染
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        renderer.update(delta);
        renderer.render();

        gles.swapBuffers();

        // 7. 帧率控制
        auto elapsed = Clock::now() - now;
        if (elapsed < frame_duration) {
            std::this_thread::sleep_for(frame_duration - elapsed);
        }
    }

    printf("Shutting down...\n");
    Live2DRenderer::frameworkDispose();
    return 0;
}
