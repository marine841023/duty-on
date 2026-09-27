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
// 渲染与旋转（2026-09-25 起）：rotation 0 = 直出 GBM surface（零合成开销）；
// rotation 90/180/270 = 场景先渲染到逻辑尺寸离屏 FBO，swapBuffers 时按纯旋转
// UV（已修正，不再镜像）合成满物理屏。surface/fb 尺寸必须 = 实际选中的 DRM
// mode 尺寸（sun4i 驱动才接受 setCrtc）。调用方以 init/setRotation 后的
// width()/height()（= 逻辑尺寸，90/270 与物理交换）自适应布局：横屏逻辑
// 用左右布局、竖屏逻辑用上下布局
class GlesContext {
public:
    GlesContext();
    ~GlesContext();

    // width/height = 期望逻辑尺寸（用于优先匹配对应 DRM 模式）；
    // 实际逻辑尺寸 = 选中的 mode 尺寸（无匹配模式时取首个模式）
    bool init(int width, int height);
    void swapBuffers();
    void destroy();

    // 整屏旋转（度：0/90/180/270，PC 菜单「设备→屏幕旋转」经 /api/status
    // 下发）。0 = 直出（不建 FBO，零额外开销）；非 0 = 场景先渲染到逻辑
    // 尺寸离屏 FBO，swapBuffers 时旋转合成满物理屏再翻页。90/270 时逻辑
    // 尺寸 = 物理尺寸交换（width()/height() 返回逻辑尺寸供布局自适应）
    void setRotation(int deg);
    int rotation() const { return rotation_; }

    // 物理（mode/默认帧缓冲）尺寸：rotation 0 = 逻辑尺寸；90/270 与逻辑交换
    int physWidth() const { return phys_w_; }
    int physHeight() const { return phys_h_; }
    bool logicalActive() const { return logical_active_; }
    // 快照调试用：旋转激活时把逻辑 FBO 重新合成到 FB0（不上屏），
    // 以便 glReadPixels 读取物理屏旋转后的实际内容
    void presentComposite() {
        if (logical_active_) compositeToScreen();
    }

    bool isValid() const { return display_ != EGL_NO_DISPLAY; }

    int width() const { return width_; }
    int height() const { return height_; }

private:
    bool createLogicalTarget();
    void destroyLogicalTarget();
    bool createCompositeProgram();
    void bindLogicalTarget();
    void compositeToScreen();

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLSurface surface_ = EGL_NO_SURFACE;
    EGLContext context_ = EGL_NO_CONTEXT;
    int width_ = 0;   // 逻辑宽（rotation 0 = mode 宽；90/270 = mode 高）
    int height_ = 0;  // 逻辑高
    int rotation_ = 0;
    int phys_w_ = 0;  // 物理（mode）宽
    int phys_h_ = 0;  // 物理（mode）高
    unsigned int fbo_ = 0;         // 逻辑离屏 FBO
    unsigned int fbo_tex_ = 0;     // FBO 颜色纹理（逻辑尺寸）
    unsigned int comp_prog_ = 0;   // 旋转合成着色器
    unsigned int comp_vbo_ = 0;    // 全屏 quad（pos+uv）
    bool logical_active_ = false;  // 当前帧是否渲染进逻辑 FBO
    std::unique_ptr<DrmState> drm_;
};

} // namespace dutyon
