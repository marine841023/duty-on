// EGL + OpenGL ES 上下文（DRM/GBM 直渲，无 X11/Wayland）。
//
// 直出模式（2026-09-24）：废除「离屏 FBO + 旋转 quad blit」方案——该方案
// 下 Panfrost 上场景渲染输出不可见（GL 程序/纹理/绘制均验证正常，但内容
// 不进 FBO，花数小时未能定位，不为此堵功能交付）。业务直接渲染到 GBM
// surface，帧经 drmModeSetCrtc / pageflip 上屏。
//
// surface 尺寸跟随实际选中的 DRM 模式：fb 尺寸必须 = mode 尺寸，sun4i
// 驱动才会接受 setCrtc（竖 fb 上横 mode 会被拒）。initDrm 优先匹配期望
// 逻辑尺寸（480x800）的模式，无匹配时取首个模式。调用方以 init 后的
// width()/height()（= mode 尺寸）作为业务逻辑尺寸自适应布局。
//
// pageflip 兜底：H616 内核的 sun4i DRM 在部分定制 EDID 时序下
// drmModePageFlip 恒返回 EBUSY，异步翻页永远不完成；翻页失败时退回
// drmModeSetCrtc 同步上屏（setCrtc 路径已实测可用）。

#include "render/gles_context.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <poll.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>   // EGL_PLATFORM_GBM_MESA 等扩展枚举（egl.h 不含 eglext.h）
#include <GLES3/gl3.h>
#include <drm_fourcc.h>
#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <fcntl.h>
#include <unistd.h>

namespace dutyon {

struct DrmState {
    int fd = -1;
    uint32_t connector_id = 0;
    uint32_t crtc_id = 0;
    drmModeModeInfo mode{};
    bool mode_set = false;
    gbm_device* dev = nullptr;
    gbm_surface* surf = nullptr;
    gbm_bo* shown_bo = nullptr;
    std::map<gbm_bo*, uint32_t> fb_cache;
};

namespace {

// 打开 /dev/dri/cardX 并挑选已连接输出（优先匹配目标分辨率的模式）
bool initDrm(DrmState& d, int width, int height) {
    const char* cards[] = {"/dev/dri/card0", "/dev/dri/card1"};
    for (const char* path : cards) {
        const int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drmModeRes* res = drmModeGetResources(fd);
        if (!res) { close(fd); continue; }
        bool found = false;
        for (int i = 0; i < res->count_connectors && !found; ++i) {
            drmModeConnector* conn = drmModeGetConnector(fd, res->connectors[i]);
            if (!conn) continue;
            if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0 &&
                conn->encoder_id) {
                drmModeEncoder* enc = drmModeGetEncoder(fd, conn->encoder_id);
                if (enc && enc->crtc_id) {
                    int pick = 0;
                    for (int m = 0; m < conn->count_modes; ++m) {
                        if (conn->modes[m].hdisplay == width &&
                            conn->modes[m].vdisplay == height) {
                            pick = m;
                            break;
                        }
                    }
                    d.fd = fd;
                    d.connector_id = conn->connector_id;
                    d.crtc_id = enc->crtc_id;
                    d.mode = conn->modes[pick];
                    printf("[GlesContext] DRM %s: mode %dx%d@%d\n", path,
                           d.mode.hdisplay, d.mode.vdisplay, d.mode.vrefresh);
                    found = true;
                }
                if (enc) drmModeFreeEncoder(enc);
            }
            drmModeFreeConnector(conn);
        }
        drmModeFreeResources(res);
        if (found) return true;
        close(fd);
    }
    return false;
}

// GBM 缓冲 → DRM framebuffer id（同一 bo 复用，翻页零拷贝注册）
uint32_t fbIdForBo(DrmState& d, gbm_bo* bo, int width, int height) {
    const auto it = d.fb_cache.find(bo);
    if (it != d.fb_cache.end()) return it->second;
    const uint32_t handle = gbm_bo_get_handle(bo).u32;
    const uint32_t stride = gbm_bo_get_stride(bo);
    uint32_t fb = 0;
    if (drmModeAddFB(d.fd, width, height, 24, 32, stride, handle, &fb) != 0) {
        return 0;
    }
    d.fb_cache[bo] = fb;
    return fb;
}

// 等 pageflip 完成（vblank），100ms 超时兜底（防内核事件丢失挂死）
void waitForFlip(int fd) {
    static bool flipped = false;
    flipped = false;
    drmEventContext ev = {};
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = [](int, unsigned, unsigned, unsigned, void*) {
        flipped = true;
    };
    struct pollfd pfd = {fd, POLLIN, 0};
    if (poll(&pfd, 1, 100) > 0) {
        drmHandleEvent(fd, &ev);
    }
}

} // namespace

GlesContext::GlesContext() = default;

GlesContext::~GlesContext() { destroy(); }

bool GlesContext::init(int width, int height) {
    drm_ = std::make_unique<DrmState>();
    if (!initDrm(*drm_, width, height)) {
        fprintf(stderr, "[GlesContext] no connected DRM output\n");
        return false;
    }

    // 逻辑尺寸 = 实际选中的 mode 尺寸（fb 必须与 mode 一致，否则 setCrtc
    // 被拒黑屏）。竖屏设备选到 480x800，横屏设备/定制 EDID 选到 800x480
    width_ = drm_->mode.hdisplay;
    height_ = drm_->mode.vdisplay;

    drm_->dev = gbm_create_device(drm_->fd);
    if (!drm_->dev) {
        fprintf(stderr, "[GlesContext] gbm_create_device failed\n");
        return false;
    }

    display_ = eglGetPlatformDisplay(EGL_PLATFORM_GBM_MESA, drm_->dev, nullptr);
    if (display_ == EGL_NO_DISPLAY) {
        fprintf(stderr, "[GlesContext] eglGetPlatformDisplay(GBM) failed\n");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(display_, &major, &minor)) {
        fprintf(stderr, "[GlesContext] eglInitialize failed\n");
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    printf("[GlesContext] EGL %d.%d initialized (GBM/DRM)\n", major, minor);

    // 配置：RGB888，OpenGL ES 3.0（alpha 不行则退化为不要求）
    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_NONE
    };
    EGLConfig config = nullptr;
    EGLint num_configs = 0;
    if (!eglChooseConfig(display_, config_attribs, &config, 1, &num_configs) ||
        num_configs == 0) {
        config_attribs[9] = 0;  // ALPHA_SIZE=0（XRGB 平面）
        if (!eglChooseConfig(display_, config_attribs, &config, 1, &num_configs) ||
            num_configs == 0) {
            fprintf(stderr, "[GlesContext] eglChooseConfig failed\n");
            return false;
        }
    }

    // 创建上下文（ES 3.0，回退 2.0）
    EGLint context_attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, context_attribs);
    if (context_ == EGL_NO_CONTEXT) {
        context_attribs[1] = 2;
        context_ = eglCreateContext(display_, config, EGL_NO_CONTEXT, context_attribs);
        if (context_ == EGL_NO_CONTEXT) {
            fprintf(stderr, "[GlesContext] eglCreateContext failed\n");
            return false;
        }
        printf("[GlesContext] fallback to OpenGL ES 2.0\n");
    }

    // GBM 表面（扫描输出 + 渲染两用；ARGB 不行退化 XRGB）；尺寸 = mode 尺寸
    drm_->surf = gbm_surface_create(
        drm_->dev, width_, height_, GBM_FORMAT_ARGB8888,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!drm_->surf) {
        drm_->surf = gbm_surface_create(
            drm_->dev, width_, height_, GBM_FORMAT_XRGB8888,
            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    }
    if (!drm_->surf) {
        fprintf(stderr, "[GlesContext] gbm_surface_create failed\n");
        return false;
    }

    surface_ = eglCreateWindowSurface(display_, config,
                                      (EGLNativeWindowType)drm_->surf, nullptr);
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

    // 物理尺寸 = mode 尺寸；rotation 0 时逻辑 = 物理（直出）
    phys_w_ = width_;
    phys_h_ = height_;

    printf("[GlesContext] OpenGL ES context ready (%dx%d, direct)\n", width_,
           height_);
    return true;
}

// ---------------------------------------------------------------------------
// 整屏旋转：逻辑 FBO + 旋转合成（rotation != 0 才启用）
// ---------------------------------------------------------------------------
bool GlesContext::createCompositeProgram() {
    auto compile = [](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512] = {};
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            fprintf(stderr, "[GlesContext] composite shader fail: %s\n", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    };
    const char* kVS =
        "attribute vec2 a_pos;\n"
        "attribute vec2 a_uv;\n"
        "varying vec2 v_uv;\n"
        "void main(){ gl_Position = vec4(a_pos, 0.0, 1.0); v_uv = a_uv; }\n";
    const char* kFS =
        "precision mediump float;\n"
        "varying vec2 v_uv;\n"
        "uniform sampler2D u_tex;\n"
        "void main(){ gl_FragColor = vec4(texture2D(u_tex, v_uv).rgb, 1.0); }\n";
    const GLuint vs = compile(GL_VERTEX_SHADER, kVS);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) return false;
    comp_prog_ = glCreateProgram();
    glAttachShader(comp_prog_, vs);
    glAttachShader(comp_prog_, fs);
    glLinkProgram(comp_prog_);
    GLint ok = 0;
    glGetProgramiv(comp_prog_, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        fprintf(stderr, "[GlesContext] composite program link fail\n");
        glDeleteProgram(comp_prog_);
        comp_prog_ = 0;
        return false;
    }
    return true;
}

bool GlesContext::createLogicalTarget() {
    destroyLogicalTarget();
    glGenTextures(1, &fbo_tex_);
    glBindTexture(GL_TEXTURE_2D, fbo_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width_, height_, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           fbo_tex_, 0);
    const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[GlesContext] logical FBO incomplete: 0x%x\n", st);
        destroyLogicalTarget();
        return false;
    }
    return true;
}

void GlesContext::destroyLogicalTarget() {
    if (fbo_) { glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
    if (fbo_tex_) { glDeleteTextures(1, &fbo_tex_); fbo_tex_ = 0; }
    logical_active_ = false;
}

void GlesContext::bindLogicalTarget() {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, width_, height_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

// 把逻辑 FBO 旋转合成到默认帧缓冲（物理分辨率）：不透明贴图铺满整屏。
// uv 按旋转角选取，使逻辑画面顺时针旋转 rotation_ 后上屏。
void GlesContext::compositeToScreen() {
    if (!comp_prog_ || !fbo_tex_) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, phys_w_, phys_h_);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(comp_prog_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fbo_tex_);
    glUniform1i(glGetUniformLocation(comp_prog_, "u_tex"), 0);
    // 全屏 quad（NDC：TL TR BL BR，TRIANGLE_STRIP）
    static const float pos[8] = {-1, 1, 1, 1, -1, -1, 1, -1};
    // uv 顺序对应 pos 的 TL/TR/BL/BR；纹理 v=1 = 逻辑画面视觉顶部。
    // 下列映射为纯旋转（行列式 +1，无镜像）：屏幕某角采样「旋转后应落到
    // 该角的逻辑角」。旧表把 180 映成左右镜像、identity 映成上下翻转，
    // 是本次「人物/时间/任务全反」的根因。
    float uv[8];
    switch (rotation_) {
        case 90:  { const float v[8] = {0, 0, 0, 1, 1, 0, 1, 1}; memcpy(uv, v, sizeof(v)); break; }
        case 180: { const float v[8] = {1, 0, 0, 0, 1, 1, 0, 1}; memcpy(uv, v, sizeof(v)); break; }
        case 270: { const float v[8] = {1, 1, 1, 0, 0, 1, 0, 0}; memcpy(uv, v, sizeof(v)); break; }
        default:  { const float v[8] = {0, 1, 1, 1, 0, 0, 1, 0}; memcpy(uv, v, sizeof(v)); break; }
    }
    const GLint lp = glGetAttribLocation(comp_prog_, "a_pos");
    const GLint lu = glGetAttribLocation(comp_prog_, "a_uv");
    glEnableVertexAttribArray(lp);
    glEnableVertexAttribArray(lu);
    glVertexAttribPointer(lp, 2, GL_FLOAT, GL_FALSE, 0, pos);
    glVertexAttribPointer(lu, 2, GL_FLOAT, GL_FALSE, 0, uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(lp);
    glDisableVertexAttribArray(lu);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}

void GlesContext::setRotation(int deg) {
    deg = ((deg % 360) + 360) % 360;
    if (deg != 0 && deg != 90 && deg != 180 && deg != 270) return;
    if (deg == rotation_) return;
    rotation_ = deg;
    const bool swap = (deg == 90 || deg == 270);
    width_ = swap ? phys_h_ : phys_w_;
    height_ = swap ? phys_w_ : phys_h_;
    if (deg == 0) {
        destroyLogicalTarget();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width_, height_);
    } else {
        if (!comp_prog_ && !createCompositeProgram()) {
            rotation_ = 0; width_ = phys_w_; height_ = phys_h_;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, width_, height_);
            return;
        }
        if (!createLogicalTarget()) {
            // FBO 不可用：回退直出（宁可不旋转也不黑屏）
            fprintf(stderr, "[GlesContext] rotation fallback to direct\n");
            rotation_ = 0; width_ = phys_w_; height_ = phys_h_;
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, width_, height_);
            return;
        }
        logical_active_ = true;
        bindLogicalTarget();
    }
    printf("[GlesContext] rotation %d (logical %dx%d -> physical %dx%d)\n",
           rotation_, width_, height_, phys_w_, phys_h_);
}

void GlesContext::swapBuffers() {
    if (display_ == EGL_NO_DISPLAY || surface_ == EGL_NO_SURFACE) return;
    // 旋转合成：场景渲染在逻辑 FBO，先旋转贴满默认帧缓冲（物理屏）再翻页
    if (logical_active_) compositeToScreen();
    eglSwapBuffers(display_, surface_);
    if (!drm_ || !drm_->surf) return;

    gbm_bo* bo = gbm_surface_lock_front_buffer(drm_->surf);
    if (!bo) return;
    // fb 尺寸必须用物理（mode）尺寸：GBM surface 按 phys_w_/phys_h_ 创建，
    // 旋转时 width_/height_ 已是逻辑尺寸（不可用于 fb 注册，否则与 bo 不匹配）
    const uint32_t fb = fbIdForBo(*drm_, bo, phys_w_, phys_h_);
    if (!fb) {
        gbm_surface_release_buffer(drm_->surf, bo);
        return;
    }

    if (!drm_->mode_set) {
        // 首帧：直接点亮 CRTC
        if (drmModeSetCrtc(drm_->fd, drm_->crtc_id, fb, 0, 0,
                           &drm_->connector_id, 1, &drm_->mode) != 0) {
            fprintf(stderr, "[GlesContext] drmModeSetCrtc failed\n");
        } else {
            printf("[GlesContext] setCrtc ok (fb %u %dx%d)\n", fb, phys_w_,
                   phys_h_);
        }
        drm_->mode_set = true;
    } else if (drmModePageFlip(drm_->fd, drm_->crtc_id, fb,
                               DRM_MODE_PAGE_FLIP_EVENT, nullptr) == 0) {
        waitForFlip(drm_->fd);
    } else if (drmModeSetCrtc(drm_->fd, drm_->crtc_id, fb, 0, 0,
                              &drm_->connector_id, 1, &drm_->mode) != 0) {
        // H616 内核定制 EDID 时序下 pageflip 恒 EBUSY：同步 setCrtc 兜底。
        // 静默失败会表现为"程序在跑、声音正常、屏幕停在旧内容"，必须留证
        static int crtc_fail_logged = 0;
        if (crtc_fail_logged < 3) {
            fprintf(stderr, "[GlesContext] setCrtc fallback failed (errno %d)\n",
                    errno);
            ++crtc_fail_logged;
        }
        return;  // fb 未上屏：不释放 shown_bo（下一帧重试同一路径）
    }

    // 翻页完成后释放上一帧（含其 fb 注册）
    if (drm_->shown_bo) {
        const auto it = drm_->fb_cache.find(drm_->shown_bo);
        if (it != drm_->fb_cache.end()) {
            drmModeRmFB(drm_->fd, it->second);
            drm_->fb_cache.erase(it);
        }
        gbm_surface_release_buffer(drm_->surf, drm_->shown_bo);
    }
    drm_->shown_bo = bo;
    // 复位逻辑目标（绑 FBO + 视口 + 混合态），供下一帧场景渲染
    if (logical_active_) bindLogicalTarget();
}

void GlesContext::destroy() {
    if (context_ != EGL_NO_CONTEXT) {
        destroyLogicalTarget();
        if (comp_prog_) { glDeleteProgram(comp_prog_); comp_prog_ = 0; }
    }
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        eglTerminate(display_);
    }
    if (drm_) {
        for (auto& kv : drm_->fb_cache) drmModeRmFB(drm_->fd, kv.second);
        drm_->fb_cache.clear();
        if (drm_->surf) gbm_surface_destroy(drm_->surf);
        if (drm_->dev) gbm_device_destroy(drm_->dev);
        if (drm_->fd >= 0) close(drm_->fd);
        drm_.reset();
    }
    display_ = EGL_NO_DISPLAY;
    surface_ = EGL_NO_SURFACE;
    context_ = EGL_NO_CONTEXT;
}

} // namespace dutyon
