#pragma once

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <memory>

namespace dutyon {

struct DrmState;  // DRM/GBM 直渲状态（实现细节在 gles_context.cpp）

// 初始化 EGL + OpenGL ES 上下文（DRM/GBM 直渲，无 X11/Wayland）
// 打开 /dev/dri/cardX → GBM 表面 → EGL(GBM 平台)，首帧经
// drmModeSetCrtc 上屏，之后 pageflip 等 vblank。
//
// init 传入的 width/height 是【逻辑分辨率】（竖屏 480x800：所有布局/投影按此
// 绘制到离屏 FBO）。物理面板只支持横屏模式，故内部按 kRotationDeg（可被环境
// 变量 DUTYON_ROTATE 覆盖）确定物理分辨率：旋转 90/270 时物理宽高 =
// (height,width)。每帧 swapBuffers 把逻辑 FBO 旋转合成到物理默认帧缓冲再翻页——
// 显示器物理竖放即得到正立竖屏画面。
class GlesContext {
public:
    GlesContext();
    ~GlesContext();

    bool init(int width, int height);   // width/height = 逻辑分辨率
    void swapBuffers();
    void destroy();

    bool isValid() const { return display_ != EGL_NO_DISPLAY; }

private:
    bool setupRotation();            // 解析旋转角（env DUTYON_ROTATE 覆盖 config）
    bool createLogicalTarget();      // 离屏 FBO + 颜色纹理（逻辑分辨率）
    bool createCompositeProgram();   // 旋转贴图 shader + 全屏 quad VAO
    void bindLogicalTarget();        // 绑定 FBO + 逻辑视口（帧起点/合成后复位）
    void compositeToScreen();        // FBO 旋转合成到默认帧缓冲（物理分辨率）

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    int log_w_ = 0, log_h_ = 0;           // 逻辑分辨率（FBO 尺寸）
    int phys_w_ = 0, phys_h_ = 0;         // 物理分辨率（GBM 表面 / 默认帧缓冲）
    int rotation_ = 0;                    // 逻辑→物理旋转角（度，逆时针）
    GLuint fbo_ = 0, fbo_tex_ = 0;        // 离屏逻辑渲染目标
    GLuint comp_prog_ = 0;                // 旋转合成着色程序
    GLuint comp_vao_ = 0, comp_vbo_ = 0;  // 全屏 quad
    std::unique_ptr<DrmState> drm_;
};

} // namespace dutyon
