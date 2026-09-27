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
        if (!ctx_.init(width, height)) return false;
        // 逻辑尺寸跟随实际选中的 DRM 模式（480x800 / 800x480 自适应）
        width_ = ctx_.width();
        height_ = ctx_.height();
        return true;
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

    // 整屏旋转：转发 GlesContext，并同步逻辑尺寸（90/270 交换）供布局自适应
    void setRotation(int deg) override {
        ctx_.setRotation(deg);
        width_ = ctx_.width();
        height_ = ctx_.height();
    }

    int physWidth() const override { return ctx_.physWidth(); }
    int physHeight() const override { return ctx_.physHeight(); }
    bool logicalActive() const override { return ctx_.logicalActive(); }
    void presentComposite() override { ctx_.presentComposite(); }

private:
    GlesContext ctx_;
    int width_ = 0, height_ = 0;
};

IPlatformWindow* createPlatformWindow() {
    return new EglWindow();
}

} // namespace dutyon

#endif // !_WIN32
