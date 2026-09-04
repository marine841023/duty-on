// ARM Linux 显示后端：DRM/GBM 直渲（无 X11）。
// 流程：打开 /dev/dri/cardX → 找已连接 connector 与 CRTC → GBM 设备/表面
// → EGL(GBM 平台) ES3 上下文；首帧经 drmModeSetCrtc 上屏，之后
// drmModePageFlip 等 vblank 翻页（自带帧节流）。

#include "render/gles_context.h"

#include <EGL/eglext.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cstdio>
#include <map>

namespace dutyon {

// DRM/GBM 直渲状态（与 EGL 表面绑定；头文件仅前置声明）
struct DrmState {
    int fd = -1;
    gbm_device* dev = nullptr;
    gbm_surface* surf = nullptr;
    uint32_t crtc_id = 0;
    uint32_t connector_id = 0;
    drmModeModeInfo mode = {};
    bool mode_set = false;
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
    width_ = width;
    height_ = height;

    drm_ = std::make_unique<DrmState>();
    if (!initDrm(*drm_, width, height)) {
        fprintf(stderr, "[GlesContext] no connected DRM output\n");
        return false;
    }

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

    // GBM 表面（扫描输出 + 渲染两用；ARGB 不行退化 XRGB）
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

    printf("[GlesContext] OpenGL ES context ready (%dx%d)\n", width_, height_);
    return true;
}

void GlesContext::swapBuffers() {
    if (display_ == EGL_NO_DISPLAY || surface_ == EGL_NO_SURFACE) return;
    eglSwapBuffers(display_, surface_);
    if (!drm_ || !drm_->surf) return;

    gbm_bo* bo = gbm_surface_lock_front_buffer(drm_->surf);
    if (!bo) return;
    const uint32_t fb = fbIdForBo(*drm_, bo, width_, height_);
    if (!fb) {
        gbm_surface_release_buffer(drm_->surf, bo);
        return;
    }

    if (!drm_->mode_set) {
        // 首帧：直接点亮 CRTC
        if (drmModeSetCrtc(drm_->fd, drm_->crtc_id, fb, 0, 0,
                           &drm_->connector_id, 1, &drm_->mode) != 0) {
            fprintf(stderr, "[GlesContext] drmModeSetCrtc failed\n");
        }
        drm_->mode_set = true;
    } else if (drmModePageFlip(drm_->fd, drm_->crtc_id, fb,
                               DRM_MODE_PAGE_FLIP_EVENT, nullptr) == 0) {
        waitForFlip(drm_->fd);
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
}

void GlesContext::destroy() {
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
