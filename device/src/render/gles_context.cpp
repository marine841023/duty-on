#include "render/gles_context.h"
#include <cstdio>
#include <cstdlib>

namespace dutyon {

GlesContext::GlesContext() = default;

GlesContext::~GlesContext() { destroy(); }

bool GlesContext::init(int width, int height) {
    width_ = width;
    height_ = height;

    // 使用默认显示（framebuffer/DRM，无 X11）
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY) {
        fprintf(stderr, "[GlesContext] eglGetDisplay failed\n");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(display_, &major, &minor)) {
        fprintf(stderr, "[GlesContext] eglInitialize failed\n");
        return false;
    }
    printf("[GlesContext] EGL %d.%d initialized\n", major, minor);

    // 配置：RGB888，OpenGL ES 3.0
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE
    };

    EGLConfig config;
    EGLint num_configs;
    if (!eglChooseConfig(display_, config_attribs, &config, 1, &num_configs) || num_configs == 0) {
        fprintf(stderr, "[GlesContext] eglChooseConfig failed\n");
        return false;
    }

    // 创建上下文（ES 3.0）
    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, context_attribs);
    if (context_ == EGL_NO_CONTEXT) {
        // 回退 ES 2.0
        context_attribs[1] = 2;
        context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, context_attribs);
        if (context_ == EGL_NO_CONTEXT) {
            fprintf(stderr, "[GlesContext] eglCreateContext failed\n");
            return false;
        }
        printf("[GlesContext] fallback to OpenGL ES 2.0\n");
    }

    // 使用默认 framebuffer surface（全屏）
    surface_ = eglCreateWindowSurface(display_, config, (EGLNativeWindowType)0, nullptr);
    if (surface_ == EGL_NO_SURFACE) {
        fprintf(stderr, "[GlesContext] eglCreateWindowSurface failed\n");
        return false;
    }

    if (!eglMakeCurrent(display_, surface_, surface_, context_)) {
        fprintf(stderr, "[GlesContext] eglMakeCurrent failed\n");
        return false;
    }

    glViewport(0, 0, width_, height_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    printf("[GlesContext] OpenGL ES context ready (%dx%d)\n", width_, height_);
    return true;
}

void GlesContext::swapBuffers() {
    if (display_ != EGL_NO_DISPLAY && surface_ != EGL_NO_SURFACE) {
        eglSwapBuffers(display_, surface_);
    }
}

void GlesContext::destroy() {
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        eglTerminate(display_);
    }
    display_ = EGL_NO_DISPLAY;
    surface_ = EGL_NO_SURFACE;
    context_ = EGL_NO_CONTEXT;
}

} // namespace dutyon
