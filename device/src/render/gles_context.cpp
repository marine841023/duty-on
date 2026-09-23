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
#include <cstdlib>
#include <map>

#include "config.h"

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
    log_w_ = width;
    log_h_ = height;
    if (!setupRotation()) return false;
    // 旋转 90/270：物理面板宽高 = 逻辑高宽互换（竖屏画面横放扫描输出）
    const bool swap_wh = (rotation_ == 90 || rotation_ == 270);
    phys_w_ = swap_wh ? log_h_ : log_w_;
    phys_h_ = swap_wh ? log_w_ : log_h_;

    drm_ = std::make_unique<DrmState>();
    if (!initDrm(*drm_, phys_w_, phys_h_)) {
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

    // GBM 表面（扫描输出 + 渲染两用；物理分辨率；ARGB 不行退化 XRGB）
    drm_->surf = gbm_surface_create(
        drm_->dev, phys_w_, phys_h_, GBM_FORMAT_ARGB8888,
        GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!drm_->surf) {
        drm_->surf = gbm_surface_create(
            drm_->dev, phys_w_, phys_h_, GBM_FORMAT_XRGB8888,
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

    // 离屏逻辑目标（竖屏 480x800）+ 旋转合成管线；bindLogicalTarget 绑定
    // FBO 并置逻辑视口/混合态，此后所有渲染都画进 FBO，swapBuffers 时旋贴上屏
    if (!createCompositeProgram()) return false;
    if (!createLogicalTarget()) return false;
    bindLogicalTarget();

    printf("[GlesContext] context ready (logical %dx%d -> physical %dx%d, rotate %d)\n",
           log_w_, log_h_, phys_w_, phys_h_, rotation_);
    return true;
}

// 解析旋转角：环境变量 DUTYON_ROTATE（0/90/180/270）覆盖 config.h 默认值，
// 无需重编译即可调显示器竖放朝向
bool GlesContext::setupRotation() {
    int r = kRotationDeg;
    const char* env = getenv("DUTYON_ROTATE");
    if (env && *env) {
        const int v = atoi(env);
        if (v == 0 || v == 90 || v == 180 || v == 270)
            r = v;
        else
            fprintf(stderr, "[GlesContext] invalid DUTYON_ROTATE=%s, using %d\n",
                    env, kRotationDeg);
    }
    rotation_ = r;
    return true;
}

// 离屏逻辑目标：RGBA 颜色纹理 + FBO（逻辑分辨率，所有渲染画到这里）
bool GlesContext::createLogicalTarget() {
    glGenTextures(1, &fbo_tex_);
    glBindTexture(GL_TEXTURE_2D, fbo_tex_);
    // GL_RGBA（无尺寸）：ES2/ES3 上下文均可作颜色可渲染附件
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, log_w_, log_h_, 0, GL_RGBA,
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
        return false;
    }
    return true;
}

// 合成管线：把逻辑纹理按旋转角贴满物理屏的着色程序 + 全屏 quad。
// 用 GLSL ES 1.00 语法（attribute/varying/texture2D），ES2/ES3 上下文均可编译。
bool GlesContext::createCompositeProgram() {
    static const char* kVS =
        "attribute vec2 aPos;\n"
        "attribute vec2 aUV;\n"
        "varying vec2 vUV;\n"
        "void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }\n";
    static const char* kFS =
        "precision mediump float;\n"
        "varying vec2 vUV;\n"
        "uniform sampler2D uTex;\n"
        // 不透明输出：扫描输出无 alpha，逻辑透明区取黑底
        "void main(){ gl_FragColor = vec4(texture2D(uTex, vUV).rgb, 1.0); }\n";

    auto compile = [](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            GLsizei n = 0;
            glGetShaderInfoLog(s, sizeof(log), &n, log);
            fprintf(stderr, "[GlesContext] composite shader fail: %s\n", log);
        }
        return s;
    };
    const GLuint vs = compile(GL_VERTEX_SHADER, kVS);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFS);
    comp_prog_ = glCreateProgram();
    glAttachShader(comp_prog_, vs);
    glAttachShader(comp_prog_, fs);
    glBindAttribLocation(comp_prog_, 0, "aPos");
    glBindAttribLocation(comp_prog_, 1, "aUV");
    glLinkProgram(comp_prog_);
    GLint ok = 0;
    glGetProgramiv(comp_prog_, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        fprintf(stderr, "[GlesContext] composite program link fail\n");
        return false;
    }

    // 全屏 quad（NDC 位置固定 BL,BR,TR,TL），UV 按旋转角排布（逆时针）
    const float pos[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    float uv[4][2];
    switch (rotation_) {
        case 90:
            uv[0][0] = 0; uv[0][1] = 1; uv[1][0] = 0; uv[1][1] = 0;
            uv[2][0] = 1; uv[2][1] = 0; uv[3][0] = 1; uv[3][1] = 1;
            break;
        case 180:
            uv[0][0] = 1; uv[0][1] = 1; uv[1][0] = 0; uv[1][1] = 1;
            uv[2][0] = 0; uv[2][1] = 0; uv[3][0] = 1; uv[3][1] = 0;
            break;
        case 270:  // 逆时针 270 = 顺时针 90
            uv[0][0] = 1; uv[0][1] = 0; uv[1][0] = 1; uv[1][1] = 1;
            uv[2][0] = 0; uv[2][1] = 1; uv[3][0] = 0; uv[3][1] = 0;
            break;
        default:   // 0
            uv[0][0] = 0; uv[0][1] = 0; uv[1][0] = 1; uv[1][1] = 0;
            uv[2][0] = 1; uv[2][1] = 1; uv[3][0] = 0; uv[3][1] = 1;
            break;
    }
    float verts[16];
    for (int i = 0; i < 4; ++i) {
        verts[i * 4 + 0] = pos[i][0];
        verts[i * 4 + 1] = pos[i][1];
        verts[i * 4 + 2] = uv[i][0];
        verts[i * 4 + 3] = uv[i][1];
    }
    glGenVertexArrays(1, &comp_vao_);
    glGenBuffers(1, &comp_vbo_);
    glBindVertexArray(comp_vao_);
    glBindBuffer(GL_ARRAY_BUFFER, comp_vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (void*)(2 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

// 绑定逻辑 FBO + 视口 + 混合态（每帧起点、以及合成翻页后复位）
void GlesContext::bindLogicalTarget() {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, log_w_, log_h_);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_SCISSOR_TEST);
}

// 把逻辑 FBO 旋转合成到默认帧缓冲（物理分辨率）：不透明贴图铺满整屏。
// 渲染方遗留的 program/VAO/texture/scissor/blend/depth 均在此重置，不影响下帧。
void GlesContext::compositeToScreen() {
    if (!comp_prog_ || !fbo_tex_ || !comp_vao_) return;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, phys_w_, phys_h_);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);  // 不透明合成
    glUseProgram(comp_prog_);
    glBindVertexArray(comp_vao_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, fbo_tex_);
    glUniform1i(glGetUniformLocation(comp_prog_, "uTex"), 0);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glBindVertexArray(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void GlesContext::swapBuffers() {
    if (display_ == EGL_NO_DISPLAY || surface_ == EGL_NO_SURFACE) return;

    // 先把逻辑 FBO（竖屏）旋转合成到默认帧缓冲（物理横屏），再翻页
    compositeToScreen();
    eglSwapBuffers(display_, surface_);

    if (drm_ && drm_->surf) {
        gbm_bo* bo = gbm_surface_lock_front_buffer(drm_->surf);
        if (bo) {
            const uint32_t fb = fbIdForBo(*drm_, bo, phys_w_, phys_h_);
            if (!fb) {
                gbm_surface_release_buffer(drm_->surf, bo);
            } else {
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
        }
    }

    // 复位逻辑目标（绑定 FBO + 视口 + 混合态），供下一帧渲染
    bindLogicalTarget();
}

void GlesContext::destroy() {
    // GL 对象须在上下文仍 current 时删除
    if (display_ != EGL_NO_DISPLAY && context_ != EGL_NO_CONTEXT) {
        eglMakeCurrent(display_, surface_, surface_, context_);
        if (fbo_) { glDeleteFramebuffers(1, &fbo_); fbo_ = 0; }
        if (fbo_tex_) { glDeleteTextures(1, &fbo_tex_); fbo_tex_ = 0; }
        if (comp_vbo_) { glDeleteBuffers(1, &comp_vbo_); comp_vbo_ = 0; }
        if (comp_vao_) { glDeleteVertexArrays(1, &comp_vao_); comp_vao_ = 0; }
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
