#pragma once

#include <functional>
#include <string>
#include <vector>

namespace dutyon {

// 平台窗口抽象：Windows (GLFW+Win32) 和 ARM Linux (EGL/fbdev) 各自实现
// 统一的接口，main.cpp 按平台选择实现。
class IPlatformWindow {
public:
    virtual ~IPlatformWindow() = default;

    // 创建窗口和 GL 上下文
    virtual bool init(int width, int height) = 0;

    // 处理事件（拖拽、托盘），返回 false 表示应退出
    virtual bool pollEvents() = 0;

    // 交换缓冲
    virtual void swapBuffers() = 0;

    // 点击穿透开关（true = 鼠标穿透到下层窗口）
    virtual void setClickThrough(bool through) = 0;

    virtual bool isClickThrough() const = 0;

    virtual void shutdown() = 0;

    // 窗口尺寸
    virtual int width() const = 0;
    virtual int height() const = 0;

    // 平台原生句柄（PC 端返回 GLFWwindow*，设备端返回 nullptr）
    virtual void* nativeHandle() { return nullptr; }

    // Win32 HWND（仅 Windows 端实现；诊断/Win32 专用）
    virtual void* nativeWinHandle() const { return nullptr; }

    // 调整窗口尺寸并保持底边不动（内容向上生长，同 1.x 面板展开行为）。
    // keep_right=true 时保持右缘不动（宽度变化向左生长：菜单 menu-left 模式，
    // 展开与收回都保持角色区贴右缘不跳）
    virtual void resizeKeepBottom(int new_w, int new_h, bool keep_right = false) {
        (void)new_w; (void)new_h; (void)keep_right;
    }

    // 窗口当前屏幕位置（物理像素；设备端不实现）
    virtual void windowPos(int& x, int& y) const { x = 0; y = 0; }
    // 位置记忆恢复：把窗口放到左缘 x、底边 bottom_y（物理像素）。
    // 底边是 2.0 的稳定锚点——所有运行期尺寸变化（菜单展开/监控面板
    // 出现）都保持底边不动。落点不在任何显示器上时忽略（沿用默认位置）
    virtual void placeAt(int x, int bottom_y) { (void)x; (void)bottom_y; }
    // 窗口当前所在监视器的完整物理分辨率（整个屏幕，非工作区）；
    // 取不到时输出 0
    virtual void monitorSize(int& w, int& h) const { w = 0; h = 0; }
    // 窗口所在监视器的工作区（物理像素；设备端不实现）
    virtual void workArea(int& left, int& top, int& right, int& bottom) const {
        left = top = right = bottom = 0;
    }

    // ---- 右键菜单：窗口层只负责捕获触发时机，菜单本体由 ImGui 自绘 ----
    // 右键角色区 / 托盘图标右键时触发（主程序里接 ui.openMenu() 切换）。
    // 参数为点击的客户区坐标；托盘触发传 (-1,-1)（无客户区坐标）
    std::function<void(int, int)> on_context_menu;
    // 菜单外点击关闭：鼠标左键按下且位于窗口矩形之外时触发
    // （GLFW 收不到窗口外的事件，轮询 GetAsyncKeyState 兜底，同 1.x blur 关菜单）
    std::function<void()> on_outside_click;

    // ---- 边缘吸附（1.x edge-dock，Win32 实现；设备端空实现）----
    // 拖拽结束越过屏幕左/右缘超 20% 窗宽时进入：窗口变成 40px 宽的
    // "红绿灯"细条（高度由主循环按内容喂给 updateDockBarHeight）。
    // 进入/退出时回调 on_edge_dock_change，主循环切换渲染内容。
    virtual bool isEdgeDocked() const { return false; }
    virtual void updateDockBarHeight(int content_h_physical) {
        (void)content_h_physical;
    }
    // 吸附条内容高度提示（物理像素；非吸附时也持续上报——
    // 拖拽中的虚线预览框按它取高度，对齐 1.x computeDockBarHeight）
    virtual void setDockBarHeightHint(int content_h_physical) {
        (void)content_h_physical;
    }
    // 主动退出（双击吸附条空白处）：恢复吸附前尺寸，角色中心对齐光标
    virtual void exitEdgeDock() {}
    // 测试自动化：绕过拖拽直接进入吸附态（edge: 0=左缘 1=右缘）
    virtual void debugEnterDock(int) {}
    std::function<void(bool)> on_edge_dock_change;

    // ---- GLFW 事件转发（窗口层 -> ImGui 等叠加 UI）----
    std::function<void(int, int, int)> mouse_button_cb;   // button, action, mods
    std::function<void(double, double)> cursor_pos_cb;    // x, y
    std::function<void(int, int, int, int)> key_cb;       // key, scancode, action, mods
    std::function<void(double, double)> scroll_cb;        // xoffset, yoffset

    // 可拖拽区域测试（窗口客户区坐标）；返回 true = 该点按住左键可拖动窗口。
    // 主程序用它把拖拽限制在角色区（面板区留给 UI 交互，同 1.x）；空 = 整窗可拖
    std::function<bool(int, int)> hit_test_drag;

    // ---- 动态点击穿透（1.x setupClickThrough：透明区域事件穿透到桌面）----
    // 可点击区域快照（客户区物理像素）：窗口层用独立线程轮询光标位置，
    // 落在任一区域内 → 正常接收鼠标事件，其余透明区域 → WS_EX_TRANSPARENT
    // 穿透到桌面。对齐 1.x click_through.rs：主循环每帧上报区域 + 菜单
    // 打开时 force_clickable（整窗可交互），拖拽/吸附期间窗口层自行强制
    struct ClickRegion {
        float x = 0, y = 0, w = 0, h = 0;
    };
    virtual void updateClickRegions(const std::vector<ClickRegion>& regions,
                                    bool force_clickable) {
        (void)regions;
        (void)force_clickable;
    }
};

// 按平台创建窗口实例（工厂函数，各平台 .cpp 实现）
IPlatformWindow* createPlatformWindow();

} // namespace dutyon
