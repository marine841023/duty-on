// Windows 平台窗口：GLFW 创建透明无边框窗口，Win32 API 补充
// 穿透/托盘/顶层/边缘吸附。完全脱离 WebView —— 2.0 的 PC 端窗口方案。
//
// 透明原理：GLFW_TRANSPARENT_FRAMEBUFFER + DWM 合成，alpha=0 的像素
// 直接透出桌面。点击穿透用 WS_EX_TRANSPARENT | WS_EX_LAYERED 切换。
//
// 右键菜单不在此层实现（1.x 的菜单是独立窗口 + HTML/CSS 样式），
// 窗口层只在右键/托盘右键时触发 on_context_menu 回调，菜单本体由
// UIRenderer 用 ImGui 自绘。

#ifdef _WIN32

#include "platform/window.h"

#ifndef GLFW_EXPOSE_NATIVE_WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace dutyon {

// 托盘图标自定义消息
static constexpr UINT WM_TRAYICON = WM_APP + 1;
static constexpr UINT TRAY_ID = 1001;

class Win32Window : public IPlatformWindow {
public:
    Win32Window() = default;
    ~Win32Window() override { shutdown(); }

    bool init(int width, int height) override {
        width_ = width;
        height_ = height;

        if (!glfwInit()) {
            fprintf(stderr, "[Win32Window] glfwInit failed\n");
            return false;
        }

        // 透明 + 无边框 + 不抢焦点 + OpenGL 兼容配置文件
        // 必须显式请求 COMPAT_PROFILE：Cubism SDK 的 OpenGL 渲染器使用
        // client-side 顶点数组（默认 VAO），Core Profile 下 glDrawElements
        // 会得到 GL_INVALID_OPERATION，模型一个像素都画不出来
        // （诊断证据：ANY_PROFILE+3.3 时驱动实际返回 Core Profile）
        glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_COMPAT_PROFILE);
        glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_FALSE);

        window_ = glfwCreateWindow(width, height, "DutyOn", nullptr, nullptr);
        if (!window_) {
            fprintf(stderr, "[Win32Window] glfwCreateWindow failed\n");
            glfwTerminate();
            return false;
        }

        glfwMakeContextCurrent(window_);
        glfwSwapInterval(1);  // vsync

        // Cubism Framework 的 OpenGL 渲染器依赖 GLEW 加载 GL 2.x+ 函数指针，
        // 必须在上下文 current 之后初始化（GLEW_STATIC 链接，无 DLL）。
        glewExperimental = GL_TRUE;
        if (glewInit() != GLEW_OK) {
            fprintf(stderr, "[Win32Window] glewInit failed\n");
            return false;
        }
        glGetError();  // 吞掉 GLEW 初始化时产生的无害 GL_INVALID_ENUM

        hwnd_ = glfwGetWin32Window(window_);
        setupWin32Styles();
        setupTrayIcon();
        setupCallbacks();

        // 点击穿透轮询线程（对齐 1.x click_through.rs 的独立线程：固定
        // 15ms 节拍，不受渲染帧率波动/卡顿影响，消除快速点击的竞态）
        click_thread_run_ = true;
        click_thread_ = std::thread(&Win32Window::clickThroughLoop, this);

        // 默认放到右下角
        positionBottomRight();

        printf("[Win32Window] %dx%d transparent window created\n", width, height);
        return true;
    }

    bool pollEvents() override {
        if (glfwWindowShouldClose(window_)) return false;
        glfwPollEvents();
        handleDrag();
        pollOutsideClick();
        return !quit_requested_;
    }

    void swapBuffers() override {
        glfwSwapBuffers(window_);
    }

    void setClickThrough(bool through) override {
        if (through == click_through_) return;
        click_through_ = through;
        printf("[ClickThrough] %s\n", through ? "ON (穿透)" : "OFF (可交互)");

        LONG_PTR ex = GetWindowLongPtr(hwnd_, GWL_EXSTYLE);
        if (through) {
            // 穿透：TRANSPARENT 必须搭配 LAYERED；LAYERED 一旦加上必须
            // 调 SetLayeredWindowAttributes 初始化，否则 DWM 不合成窗口
            ex |= WS_EX_TRANSPARENT | WS_EX_LAYERED;
            SetWindowLongPtr(hwnd_, GWL_EXSTYLE, ex);
            SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        } else {
            ex &= ~(WS_EX_TRANSPARENT | WS_EX_LAYERED);
            SetWindowLongPtr(hwnd_, GWL_EXSTYLE, ex);
        }
        // 让样式修改立即生效
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    bool isClickThrough() const override { return click_through_; }

    // ---- 边缘吸附（1.x edge-dock）----
    bool isEdgeDocked() const override { return dock_active_; }

    void updateDockBarHeight(int content_h) override {
        if (dock_active_ && content_h != height_) applyDockBarHeight(content_h);
    }

    void setDockBarHeightHint(int content_h) override {
        dock_bar_h_hint_ = content_h;
    }

    void exitEdgeDock() override { exitDockInternal(); }

    void debugEnterDock(int edge) override {
        if (dock_active_ || !hwnd_) return;
        RECT rc;
        if (!GetWindowRect(hwnd_, &rc)) return;
        dock_edge_ = edge == 0 ? 0 : 1;
        pre_dock_rect_ = rc;
        dock_cy_ = (rc.top + rc.bottom) / 2;
        dock_active_ = true;
        applyDockBarHeight(height_);
        printf("[EdgeDock][debug] enter %s edge\n",
               dock_edge_ == 0 ? "left" : "right");
        if (on_edge_dock_change) on_edge_dock_change(true);
    }

    void shutdown() override {
        if (click_thread_run_.exchange(false)) {
            if (click_thread_.joinable()) click_thread_.join();
        }
        if (tray_added_) {
            NOTIFYICONDATAA nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd_;
            nid.uID = TRAY_ID;
            Shell_NotifyIconA(NIM_DELETE, &nid);
            tray_added_ = false;
        }
        if (ghost_) {
            DestroyWindow(ghost_);
            ghost_ = nullptr;
        }
        if (window_) {
            glfwDestroyWindow(window_);
            window_ = nullptr;
        }
        glfwTerminate();
    }

    int width() const override { return width_; }
    int height() const override { return height_; }

    void* nativeHandle() override { return window_; }  // GLFWwindow*

    HWND hwnd() const { return hwnd_; }
    void* nativeWinHandle() const override { return hwnd_; }

    void resizeKeepBottom(int new_w, int new_h, bool keep_right) override {
        if (!hwnd_) return;
        if (new_w == width_ && new_h == height_) return;
        RECT rc;
        GetWindowRect(hwnd_, &rc);
        // 底边固定：y = 旧底边 - 新高度（内容向上生长，同 1.x）；
        // 宽度：默认左边固定向右生长（菜单展开）；keep_right=true 保持右缘
        // 向左生长（menu-left 模式，角色区贴右缘不跳）
        const int x = keep_right ? rc.right - new_w : rc.left;
        SetWindowPos(hwnd_, HWND_TOPMOST, x, rc.bottom - new_h,
                     new_w, new_h, SWP_NOACTIVATE);
        width_ = new_w;
        height_ = new_h;
    }

    void windowPos(int& x, int& y) const override {
        RECT rc;
        if (hwnd_ && GetWindowRect(hwnd_, &rc)) {
            x = rc.left;
            y = rc.top;
        } else {
            x = 0;
            y = 0;
        }
    }

    void placeAt(int x, int bottom_y) override {
        if (!hwnd_) return;
        // 有效性：恢复矩形中心必须落在某台显示器上（显示器拔掉/布局
        // 改动后旧坐标可能在屏幕外——放弃恢复，沿用默认右下角，同 1.x
        // is_position_on_screen 语义）
        const POINT ctr = {x + width_ / 2, bottom_y - height_ / 2};
        HMONITOR mon = MonitorFromPoint(ctr, MONITOR_DEFAULTTONULL);
        if (!mon) return;
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfo(mon, &mi)) return;
        // 完整可见地夹进该显示器工作区（贴边拖到半屏外的情形拉回可见）
        int left = x;
        int top = bottom_y - height_;
        const RECT& wa = mi.rcWork;
        if (left < wa.left) left = wa.left;
        if (left + width_ > wa.right) left = wa.right - width_;
        if (top < wa.top) top = wa.top;
        if (top + height_ > wa.bottom) top = wa.bottom - height_;
        SetWindowPos(hwnd_, HWND_TOPMOST, left, top, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE);
        printf("[Win32Window] restore position: left=%d bottom=%d\n", left,
               top + height_);
    }

    void monitorSize(int& w, int& h) const override {
        w = 0;
        h = 0;
        if (!hwnd_) return;
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(mon, &mi)) {
            w = mi.rcMonitor.right - mi.rcMonitor.left;
            h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        }
    }

    void workArea(int& left, int& top, int& right, int& bottom) const override {
        HMONITOR mon = hwnd_ ? MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST)
                             : nullptr;
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfo(mon, &mi)) {
            left = mi.rcWork.left;
            top = mi.rcWork.top;
            right = mi.rcWork.right;
            bottom = mi.rcWork.bottom;
        } else {
            RECT wa;
            SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
            left = wa.left;
            top = wa.top;
            right = wa.right;
            bottom = wa.bottom;
        }
    }

private:
    // ---- Win32 样式：顶层、工具窗口（不出现在任务栏）、不激活 ----
    // 注意：不能在这里加 WS_EX_LAYERED —— 未调用 SetLayeredWindowAttributes/
    // UpdateLayeredWindow 的 layered 窗口不会被 DWM 合成到屏幕（GL 帧缓冲
    // 里画了内容也看不到）。GLFW_TRANSPARENT_FRAMEBUFFER 的逐像素透明由
    // 帧缓冲 alpha 通道 + DWM 完成，无需 LAYERED。点击穿透时才动态加
    // （见 setClickThrough，加的时候会同步补 SLWA 保持窗口可见）。
    void setupWin32Styles() {
        LONG_PTR ex = GetWindowLongPtr(hwnd_, GWL_EXSTYLE);
        ex |= WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
        ex &= ~(WS_EX_APPWINDOW | WS_EX_LAYERED);
        SetWindowLongPtr(hwnd_, GWL_EXSTYLE, ex);
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    // ---- 系统托盘 ----
    void setupTrayIcon() {
        NOTIFYICONDATAA nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = TRAY_ID;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = LoadIconA(nullptr, IDI_APPLICATION);  // TODO: 自定义图标
        strcpy_s(nid.szTip, "Duty On 桌宠");
        tray_added_ = Shell_NotifyIconA(NIM_ADD, &nid) != FALSE;
    }

    // ---- GLFW 回调 ----
    void setupCallbacks() {
        glfwSetWindowUserPointer(window_, this);

        glfwSetMouseButtonCallback(window_, [](GLFWwindow* w, int button, int action, int mods) {
            auto* self = static_cast<Win32Window*>(glfwGetWindowUserPointer(w));
            if (!self) return;
            // 先喂给叠加 UI（ImGui 项目行点击等）
            if (self->mouse_button_cb) self->mouse_button_cb(button, action, mods);
            if (button == GLFW_MOUSE_BUTTON_LEFT) {
                // 仅角色区可拖拽窗口（面板区留给 UI 交互，同 1.x；
                // 吸附模式下整条都是拖拽把手）
                if (action == GLFW_PRESS) {
                    double cx = 0, cy = 0;
                    glfwGetCursorPos(w, &cx, &cy);
                    const bool drag =
                        !self->hit_test_drag || self->hit_test_drag((int)cx, (int)cy);
                    if (drag) {
                        POINT pt;
                        GetCursorPos(&pt);
                        RECT rc;
                        GetWindowRect(self->hwnd_, &rc);
                        self->drag_offset_x_ = pt.x - rc.left;
                        self->drag_offset_y_ = pt.y - rc.top;
                        self->drag_start_x_ = pt.x;
                        self->drag_start_y_ = pt.y;
                        self->dragging_ = true;
                    }
                } else if (action == GLFW_RELEASE && self->dragging_) {
                    self->dragging_ = false;
                    // 拖拽结束：越过屏幕左/右缘超 20% 窗宽 → 边缘吸附
                    //（1.x detect_edge_dock / enter_edge_dock）
                    self->hideGhostPreview();  // 预览框只在拖拽中出现
                    self->tryEnterEdgeDock();
                } else if (action == GLFW_RELEASE) {
                    self->dragging_ = false;
                    self->hideGhostPreview();
                }
            }
            if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
                // 右键 -> 打开/关闭自绘菜单（菜单矩形内的右键由主程序忽略）
                if (self->on_context_menu) {
                    double cx = 0, cy = 0;
                    glfwGetCursorPos(w, &cx, &cy);
                    self->on_context_menu((int)cx, (int)cy);
                }
            }
        });

        glfwSetCursorPosCallback(window_, [](GLFWwindow* w, double x, double y) {
            auto* self = static_cast<Win32Window*>(glfwGetWindowUserPointer(w));
            if (self && self->cursor_pos_cb) self->cursor_pos_cb(x, y);
        });

        glfwSetScrollCallback(window_, [](GLFWwindow* w, double x, double y) {
            auto* self = static_cast<Win32Window*>(glfwGetWindowUserPointer(w));
            if (self && self->scroll_cb) self->scroll_cb(x, y);
        });

        glfwSetKeyCallback(window_, [](GLFWwindow* w, int key, int scancode,
                                       int action, int mods) {
            auto* self = static_cast<Win32Window*>(glfwGetWindowUserPointer(w));
            if (self && self->key_cb) self->key_cb(key, scancode, action, mods);
        });

        // 拦截托盘消息需要子类化 WndProc
        old_wndproc_ = reinterpret_cast<WNDPROC>(
            SetWindowLongPtr(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndProcThunk)));
        SetWindowLongPtr(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }

    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (msg == WM_TRAYICON && self) {
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK) {
                if (self->on_context_menu) self->on_context_menu(-1, -1);
                return 0;
            }
        }
        if (self && self->old_wndproc_) {
            return CallWindowProc(self->old_wndproc_, hwnd, msg, wp, lp);
        }
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    // ---- 拖拽移动 + 边缘吸附（1.x 拖拽无位置吸附，越过边缘释放才停靠）----
    void handleDrag() {
        if (!dragging_ || click_through_) return;
        POINT pt;
        GetCursorPos(&pt);
        // 吸附条被拖离边缘：恢复整窗，拖拽无缝继续（1.x leaveEdgeDock）
        if (dock_active_ &&
            (abs(pt.x - drag_start_x_) + abs(pt.y - drag_start_y_)) > 4) {
            exitDockInternal();
        }
        const int x = pt.x - drag_offset_x_;
        const int y = pt.y - drag_offset_y_;
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        // 拖拽中的停靠位置虚线预览（1.x updateDockPreview ghost）：
        // 越过边缘阈值时在将停靠的位置显示虚线框，松手才真正吸附
        if (!dock_active_) {
            const int cand = snapCandidate();
            if (cand >= 0) updateGhostPreview(cand);
            else hideGhostPreview();
        }
    }

    // snap 候选：0=左缘 1=右缘 -1=无（判定与 1.x detect_edge_dock 一致：
    // 窗口越过屏幕左/右缘超 20% 窗宽）
    int snapCandidate() const {
        RECT rc;
        if (!GetWindowRect(hwnd_, &rc)) return -1;
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfo(mon, &mi)) return -1;
        const int w = rc.right - rc.left;
        const long long threshold = (long long)((double)w * 0.2);
        const long long cross_l = (long long)mi.rcMonitor.left - rc.left;
        const long long cross_r = (long long)rc.right - (long long)mi.rcMonitor.right;
        if (cross_l <= threshold && cross_r <= threshold) return -1;
        return cross_l >= cross_r ? 0 : 1;
    }

    // ---- 边缘吸附（1.x edge-dock：拖拽释放时窗口越过屏幕左/右缘
    // 超 20% 窗宽才停靠；变成 40px 宽细条，高度由主循环按内容校正）----
    void tryEnterEdgeDock() {
        if (dock_active_) return;
        const int cand = snapCandidate();
        if (cand < 0) return;
        RECT rc;
        if (!GetWindowRect(hwnd_, &rc)) return;
        dock_active_ = true;
        dock_edge_ = cand;                        // 0=左缘 1=右缘
        pre_dock_rect_ = rc;
        dock_cy_ = (rc.top + rc.bottom) / 2;      // 条垂直居中于释放位置
        applyDockBarHeight(height_);              // 下一帧主循环按内容校正
        hideGhostPreview();                       // 预览框完成使命
        printf("[EdgeDock] enter %s edge\n", dock_edge_ == 0 ? "left" : "right");
        if (on_edge_dock_change) on_edge_dock_change(true);
    }

    // 停靠条几何：40px 宽 × content_h 高，贴边、垂直中心不变、钳制屏内
    void applyDockBarHeight(int h) {
        const float sc = contentScale();
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfo(mon, &mi)) return;
        const int w = (int)(40.0f * sc);  // 1.x EDGE_DOCK_THICKNESS
        const int min_h = (int)(80.0f * sc);
        const int max_h =
            (mi.rcMonitor.bottom - mi.rcMonitor.top) - (int)(16.0f * sc);
        if (h < min_h) h = min_h;
        if (max_h >= min_h && h > max_h) h = max_h;
        const int margin = (int)(8.0f * sc);
        const int x = dock_edge_ == 0 ? mi.rcMonitor.left
                                      : mi.rcMonitor.right - w;
        int y = dock_cy_ - h / 2;
        if (y < mi.rcMonitor.top + margin) y = mi.rcMonitor.top + margin;
        if (y > mi.rcMonitor.bottom - margin - h)
            y = mi.rcMonitor.bottom - margin - h;
        if (y < mi.rcMonitor.top + margin) y = mi.rcMonitor.top + margin;
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
        width_ = w;
        height_ = h;
    }

    // 退出吸附：恢复吸附前尺寸，角色画布中心对齐光标（拖离/双击通用；
    // 拖拽锚点同步移动到角色中心，拖拽无缝继续，同 1.x exit_edge_dock）
    void exitDockInternal() {
        if (!dock_active_) return;
        dock_active_ = false;
        const float sc = contentScale();
        const int w = pre_dock_rect_.right - pre_dock_rect_.left;
        const int h = pre_dock_rect_.bottom - pre_dock_rect_.top;
        const int cx_off = w / 2;
        const int cy_off = w < (int)(200.0f * sc) ? (int)(65.0f * sc)
                                                  : (int)(130.0f * sc);
        POINT pt;
        int x, y;
        if (GetCursorPos(&pt)) {
            x = pt.x - cx_off;
            y = pt.y - cy_off;
        } else {
            x = pre_dock_rect_.left;
            y = pre_dock_rect_.top;
        }
        HMONITOR mon = MonitorFromPoint({x + w / 2, y + h / 2},
                                        MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(mon, &mi)) {
            if (x < mi.rcMonitor.left) x = mi.rcMonitor.left;
            if (y < mi.rcMonitor.top) y = mi.rcMonitor.top;
            if (x + w > mi.rcMonitor.right) x = mi.rcMonitor.right - w;
            if (y + h > mi.rcMonitor.bottom) y = mi.rcMonitor.bottom - h;
        }
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
        width_ = w;
        height_ = h;
        drag_offset_x_ = cx_off;
        drag_offset_y_ = cy_off;
        printf("[EdgeDock] exit\n");
        if (on_edge_dock_change) on_edge_dock_change(false);
    }

    float contentScale() const {
        float xs = 1.0f, ys = 1.0f;
        if (window_) glfwGetWindowContentScale(window_, &xs, &ys);
        return xs >= 0.5f ? xs : 1.0f;
    }

    // ---- 停靠位置虚线预览框（1.x updateDockPreview 的 ghost 窗口）----
    // 独立 Win32 分层窗口：黑色 color-key 全透明 + 白色虚线圆角边框，
    // 不抢焦点、鼠标事件穿透。只在拖拽越过边缘阈值时短暂显示
    static LRESULT CALLBACK GhostWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            // 背景 = color key（黑）→ 完全透明
            HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(hdc, &rc, bg);
            DeleteObject(bg);
            // 蓝色虚线圆角边框（2px，PS_DASH；主题蓝 #6496FF）
            HPEN pen = CreatePen(PS_DASH, 2, RGB(0x64, 0x96, 0xFF));
            HPEN old_pen = (HPEN)SelectObject(hdc, pen);
            HGDIOBJ old_br = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, rc.left + 2, rc.top + 2, rc.right - 2, rc.bottom - 2,
                      14, 14);
            SelectObject(hdc, old_pen);
            SelectObject(hdc, old_br);
            DeleteObject(pen);
            EndPaint(h, &ps);
            return 0;
        }
        return DefWindowProc(h, msg, wp, lp);
    }

    void ensureGhost() {
        if (ghost_) return;
        WNDCLASSA wc = {};
        wc.lpfnWndProc = GhostWndProc;
        wc.hInstance = GetModuleHandleA(nullptr);
        wc.lpszClassName = "DutyOnDockGhost";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassA(&wc);
        ghost_ = CreateWindowExA(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            "DutyOnDockGhost", "", WS_POPUP, 0, 0, 100, 100, nullptr, nullptr,
            GetModuleHandleA(nullptr), nullptr);
        if (ghost_)
            SetLayeredWindowAttributes(ghost_, RGB(0, 0, 0), 0, LWA_COLORKEY);
    }

    // 在将停靠的位置显示预览框（几何 = 停靠条最终几何：40px 宽贴边，
    // 高度取主循环上报的吸附条内容高，垂直中心跟随窗口中心）
    void updateGhostPreview(int edge) {
        ensureGhost();
        if (!ghost_) return;
        const float sc = contentScale();
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (!GetMonitorInfo(mon, &mi)) return;
        const int w = (int)(40.0f * sc);
        const int min_h = (int)(80.0f * sc);
        const int max_h =
            (mi.rcMonitor.bottom - mi.rcMonitor.top) - (int)(16.0f * sc);
        int h = dock_bar_h_hint_ > 0 ? dock_bar_h_hint_ : min_h;
        if (h < min_h) h = min_h;
        if (max_h >= min_h && h > max_h) h = max_h;
        const int margin = (int)(8.0f * sc);
        RECT rc;
        if (!GetWindowRect(hwnd_, &rc)) return;
        const int x = edge == 0 ? mi.rcMonitor.left : mi.rcMonitor.right - w;
        int y = (rc.top + rc.bottom) / 2 - h / 2;
        if (y < mi.rcMonitor.top + margin) y = mi.rcMonitor.top + margin;
        if (y > mi.rcMonitor.bottom - margin - h)
            y = mi.rcMonitor.bottom - margin - h;
        if (y < mi.rcMonitor.top + margin) y = mi.rcMonitor.top + margin;
        SetWindowPos(ghost_, HWND_TOPMOST, x, y, w, h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void hideGhostPreview() {
        if (ghost_ && IsWindowVisible(ghost_)) ShowWindow(ghost_, SW_HIDE);
    }

    // ---- 菜单外点击关闭（GLFW 收不到窗口外事件，轮询兜底）----
    void pollOutsideClick() {
        if (!on_outside_click) return;
        if (!(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;
        POINT pt;
        GetCursorPos(&pt);
        RECT rc;
        GetWindowRect(hwnd_, &rc);
        if (pt.x < rc.left || pt.x >= rc.right || pt.y < rc.top || pt.y >= rc.bottom)
            on_outside_click();
    }

    // ---- 动态点击穿透（1.x click_through.rs 架构）----
    // 独立线程固定 15ms 轮询 GetCursorPos（不受渲染帧率波动影响）；
    // 区域快照由主循环每帧通过 updateClickRegions 上报（互斥锁保护）。
    // 判定：窗口外 / 拖拽 / 吸附 / force（菜单打开）→ 可交互；窗口内
    // 且命中任一区域 → 可交互；其余 → WS_EX_TRANSPARENT 穿透
    void clickThroughLoop() {
        while (click_thread_run_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
            if (!hwnd_) continue;
            if (dragging_.load(std::memory_order_relaxed) ||
                dock_active_.load(std::memory_order_relaxed)) {
                setClickThrough(false);
                continue;
            }
            std::vector<ClickRegion> regions;
            bool force = false;
            {
                std::lock_guard<std::mutex> lk(click_mu_);
                regions = click_regions_;
                force = force_clickable_;
            }
            POINT pt;
            if (!GetCursorPos(&pt)) continue;
            RECT rc;
            if (!GetWindowRect(hwnd_, &rc)) continue;
            if (pt.x < rc.left || pt.x >= rc.right || pt.y < rc.top ||
                pt.y >= rc.bottom) {
                setClickThrough(false);   // 窗口外：保持可交互（1.x 同语义）
                continue;
            }
            if (force) {
                setClickThrough(false);
                continue;
            }
            POINT cl = pt;                // 屏幕物理 → 客户区物理（DPI 感知下同单位）
            ScreenToClient(hwnd_, &cl);
            bool hit = false;
            for (const auto& r : regions) {
                if ((float)cl.x >= r.x && (float)cl.x < r.x + r.w &&
                    (float)cl.y >= r.y && (float)cl.y < r.y + r.h) {
                    hit = true;
                    break;
                }
            }
            setClickThrough(!hit);
        }
    }

    void updateClickRegions(const std::vector<ClickRegion>& regions,
                            bool force_clickable) override {
        std::lock_guard<std::mutex> lk(click_mu_);
        click_regions_ = regions;
        force_clickable_ = force_clickable;
    }

    void positionBottomRight() {
        RECT work;
        SystemParametersInfo(SPI_GETWORKAREA, 0, &work, 0);
        int x = work.right - width_ - 40;
        int y = work.bottom - height_ - 40;
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }

    GLFWwindow* window_ = nullptr;
    HWND hwnd_ = nullptr;
    WNDPROC old_wndproc_ = nullptr;
    int width_ = 0, height_ = 0;
    std::atomic<bool> click_through_{false};
    bool quit_requested_ = false;
    bool tray_added_ = false;
    std::atomic<bool> dragging_{false};
    int drag_offset_x_ = 0, drag_offset_y_ = 0;
    int drag_start_x_ = 0, drag_start_y_ = 0;  // 拖拽起点（屏幕坐标）

    // 点击穿透轮询线程 + 区域快照（主循环每帧上报，锁保护跨线程读写）
    std::thread click_thread_;
    std::atomic<bool> click_thread_run_{false};
    std::mutex click_mu_;
    std::vector<ClickRegion> click_regions_;
    bool force_clickable_ = false;

    // 边缘吸附状态（轮询线程读取 → atomic）
    std::atomic<bool> dock_active_{false};
    int dock_edge_ = 0;       // 0=左缘 1=右缘
    RECT pre_dock_rect_{};    // 吸附前窗口矩形
    int dock_cy_ = 0;         // 停靠条垂直中心（屏幕坐标）
    int dock_bar_h_hint_ = 0; // 吸附条内容高（主循环上报，ghost 预览用）
    HWND ghost_ = nullptr;    // 拖拽停靠位置虚线预览框
};

IPlatformWindow* createPlatformWindow() {
    return new Win32Window();
}

} // namespace dutyon

#endif // _WIN32
