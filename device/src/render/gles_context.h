#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <memory>

namespace dutyon {

struct DrmState;  // DRM/GBM 直渲状态（实现细节在 gles_context.cpp）

// 初始化 EGL + OpenGL ES 上下文（DRM/GBM 直渲，无 X11/Wayland）
// 打开 /dev/dri/cardX → GBM 表面 → EGL(GBM 平台)，首帧经
// drmModeSetCrtc 上屏，之后 pageflip 等 vblank（pageflip 失败回退
// setCrtc：H616 内核在部分定制 EDID 时序下 pageflip 恒 EBUSY）
//
// 直出模式（2026-09-24 起，废除离屏 FBO + 旋转 blit 方案——该方案下
// 场景渲染输出在 Panfrost 上不可见）：surface 尺寸跟随实际选中的 DRM
// 模式（fb 尺寸必须 = mode 尺寸，sun4i 驱动才会 setCrtc 成功）。调用方
// 以 init 后的 width()/height() 作为业务逻辑尺寸自适应布局（竖屏设备
// 480x800、横屏 800x480）
class GlesContext {
public:
    GlesContext();
    ~GlesContext();

    // width/height = 期望逻辑尺寸（用于优先匹配对应 DRM 模式）；
    // 实际逻辑尺寸 = 选中的 mode 尺寸（无匹配模式时取首个模式）
    bool init(int width, int height);
    void swapBuffers();
    void destroy();

    bool isValid() const { return display_ != EGL_NO_DISPLAY; }

    int width() const { return width_; }
    int height() const { return height_; }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    int width_ = 0;   // 逻辑宽（= 选中的 mode 宽）
    int height_ = 0;  // 逻辑高（= 选中的 mode 高）
    std::unique_ptr<DrmState> drm_;
};

} // namespace dutyon
