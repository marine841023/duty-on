#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <memory>

namespace dutyon {

struct DrmState;  // DRM/GBM 直渲状态（实现细节在 gles_context.cpp）

// 初始化 EGL + OpenGL ES 上下文（DRM/GBM 直渲，无 X11/Wayland）
// 打开 /dev/dri/cardX → GBM 表面 → EGL(GBM 平台)，首帧经
// drmModeSetCrtc 上屏，之后 pageflip 等 vblank
class GlesContext {
public:
    GlesContext();
    ~GlesContext();

    bool init(int width, int height);
    void swapBuffers();
    void destroy();

    bool isValid() const { return display_ != EGL_NO_DISPLAY; }

private:
    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    int width_ = 0;
    int height_ = 0;
    std::unique_ptr<DrmState> drm_;
};

} // namespace dutyon
