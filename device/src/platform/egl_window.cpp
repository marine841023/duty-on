// ARM Linux 平台窗口：EGL + framebuffer 直渲（无 X11/Wayland）。
// 包装现有的 GlesContext 为 IPlatformWindow 接口。
// 嵌入式设备无窗口系统概念：无穿透/托盘/拖拽，事件循环恒为运行态。

#ifndef _WIN32

#include "platform/window.h"
#include "../render/gles_context.h"

namespace dutyon {

class EglWindow : public IPlatformWindow {
public:
    EglWindow() = default;
    ~EglWindow() override { shutdown(); }

    bool init(int width, int height) override {
        width_ = width;
        height_ = height;
        return ctx_.init(width, height);
    }

    bool pollEvents() override {
        return true;  // 无窗口事件；退出由 main 的 signal handler 控制
    }

    void swapBuffers() override { ctx_.swapBuffers(); }

    void setClickThrough(bool) override {}
    bool isClickThrough() const override { return false; }

    void shutdown() override { ctx_.destroy(); }

    int width() const override { return width_; }
    int height() const override { return height_; }

private:
    GlesContext ctx_;
    int width_ = 0, height_ = 0;
};

IPlatformWindow* createPlatformWindow() {
    return new EglWindow();
}

} // namespace dutyon

#endif // !_WIN32
