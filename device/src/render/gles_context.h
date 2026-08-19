#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>

namespace dutyon {

// 初始化 EGL + OpenGL ES 上下文（直接 DRM/KMS 或 framebuffer）
// 不依赖 X11/Wayland，最小化系统开销
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
};

} // namespace dutyon
