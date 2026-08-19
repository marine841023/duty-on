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

    // 1. 初始化 OpenGL ES 上下文（无窗口系统）
    GlesContext gles;
    if (!gles.init(kDisplayWidth, kDisplayHeight)) {
        fprintf(stderr, "Failed to initialize GLES context\n");
        return 1;
    }

    // 2. 加载 Live2D 模型
    Live2DRenderer renderer;
    if (!renderer.loadModel(kModelDir, kDefaultModel)) {
        fprintf(stderr, "Failed to load Live2D model\n");
        // 不退出，允许无模型运行（仅状态灯模式）
    }

    // 3. 初始化 API 客户端和状态机
    ApiClient api(kApiBaseUrl);
    StateMachine state_machine;

    // 初始状态
    renderer.playMotion(state_machine.currentIdleGroup());

    printf("Entering main loop...\n");

    const auto frame_duration = std::chrono::milliseconds(1000 / kTargetFps);
    auto last_poll = Clock::now() - std::chrono::milliseconds(kPollIntervalMs);
    auto last_frame = Clock::now();

    while (g_running) {
        auto now = Clock::now();
        float delta = std::chrono::duration<float>(now - last_frame).count();
        last_frame = now;

        // 4. 定时轮询桌面端状态
        if (now - last_poll >= std::chrono::milliseconds(kPollIntervalMs)) {
            last_poll = now;
            if (auto status = api.poll()) {
                auto motion = state_machine.onStatus(*status);
                if (!motion.empty()) {
                    printf("[State] %s -> %s\n",
                           status->state.c_str(), motion.c_str());
                    renderer.playMotion(motion);
                }
            }
        }

        // 5. 渲染
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        renderer.update(delta);
        renderer.render();

        gles.swapBuffers();

        // 6. 帧率控制
        auto elapsed = Clock::now() - now;
        if (elapsed < frame_duration) {
            std::this_thread::sleep_for(frame_duration - elapsed);
        }
    }

    printf("Shutting down...\n");
    return 0;
}
