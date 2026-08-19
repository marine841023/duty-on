// Windows 平台窗口：GLFW 创建透明无边框窗口，Win32 API 补充
// 穿透/托盘/顶层。完全脱离 WebView —— 2.0 的 PC 端窗口方案。
//
// 透明原理：GLFW_TRANSPARENT_FRAMEBUFFER + DWM 合成，alpha=0 的像素
// 直接透出桌面。点击穿透用 WS_EX_TRANSPARENT | WS_EX_LAYERED 切换。

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

#include <cstdio>

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

        // 透明 + 无边框 + 不抢焦点 + OpenGL 3.3 Core
        glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, GLFW_TRUE);
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

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

        // 默认放到右下角
        positionBottomRight();

        printf("[Win32Window] %dx%d transparent window created\n", width, height);
        return true;
    }

    bool pollEvents() override {
        if (glfwWindowShouldClose(window_)) return false;
        glfwPollEvents();
        handleDrag();
        return !quit_requested_;
    }

    void swapBuffers() override {
        glfwSwapBuffers(window_);
    }

    void setClickThrough(bool through) override {
        if (through == click_through_) return;
        click_through_ = through;

        LONG_PTR ex = GetWindowLongPtr(hwnd_, GWL_EXSTYLE);
        if (through) {
            ex |= WS_EX_TRANSPARENT | WS_EX_LAYERED;
        } else {
            ex &= ~WS_EX_TRANSPARENT;
        }
        SetWindowLongPtr(hwnd_, GWL_EXSTYLE, ex);
        // 让样式修改立即生效
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    bool isClickThrough() const override { return click_through_; }

    void shutdown() override {
        if (tray_added_) {
            NOTIFYICONDATAA nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd_;
            nid.uID = TRAY_ID;
            Shell_NotifyIconA(NIM_DELETE, &nid);
            tray_added_ = false;
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

private:
    // ---- Win32 样式：顶层、工具窗口（不出现在任务栏）、不激活 ----
    void setupWin32Styles() {
        LONG_PTR ex = GetWindowLongPtr(hwnd_, GWL_EXSTYLE);
        ex |= WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED;
        ex &= ~WS_EX_APPWINDOW;
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

        glfwSetMouseButtonCallback(window_, [](GLFWwindow* w, int button, int action, int) {
            auto* self = static_cast<Win32Window*>(glfwGetWindowUserPointer(w));
            if (button == GLFW_MOUSE_BUTTON_LEFT) {
                self->dragging_ = (action == GLFW_PRESS);
                if (self->dragging_) {
                    POINT pt;
                    GetCursorPos(&pt);
                    RECT rc;
                    GetWindowRect(self->hwnd_, &rc);
                    self->drag_offset_x_ = pt.x - rc.left;
                    self->drag_offset_y_ = pt.y - rc.top;
                }
            }
            if (button == GLFW_MOUSE_BUTTON_RIGHT && action == GLFW_PRESS) {
                self->showContextMenu();
            }
        });

        // 拦截托盘消息需要子类化 WndProc
        old_wndproc_ = reinterpret_cast<WNDPROC>(
            SetWindowLongPtr(hwnd_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndProcThunk)));
        SetWindowLongPtr(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }

    static LRESULT CALLBACK wndProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Win32Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (msg == WM_TRAYICON && self) {
            if (LOWORD(lp) == WM_RBUTTONUP) {
                self->showContextMenu();
                return 0;
            }
        }
        if (self && self->old_wndproc_) {
            return CallWindowProc(self->old_wndproc_, hwnd, msg, wp, lp);
        }
        return DefWindowProc(hwnd, msg, wp, lp);
    }

    // ---- 拖拽移动 ----
    void handleDrag() {
        if (!dragging_ || click_through_) return;
        POINT pt;
        GetCursorPos(&pt);
        SetWindowPos(hwnd_, HWND_TOPMOST,
                     pt.x - drag_offset_x_, pt.y - drag_offset_y_,
                     0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // ---- 右键菜单（原生 Win32 弹出菜单）----
    void showContextMenu() {
        HMENU menu = CreatePopupMenu();
        AppendMenuA(menu, MF_STRING, 1, click_through_ ? "取消穿透" : "点击穿透");
        AppendMenuA(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuA(menu, MF_STRING, 2, "退出");

        POINT pt;
        GetCursorPos(&pt);
        // TPM_RETURNCMD: 直接返回选中项而不发消息，省去消息路由
        SetForegroundWindow(hwnd_);
        int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                 pt.x, pt.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);

        switch (cmd) {
            case 1: setClickThrough(!click_through_); break;
            case 2: quit_requested_ = true; break;
            default: break;
        }
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
    bool click_through_ = false;
    bool quit_requested_ = false;
    bool tray_added_ = false;
    bool dragging_ = false;
    int drag_offset_x_ = 0, drag_offset_y_ = 0;
};

IPlatformWindow* createPlatformWindow() {
    return new Win32Window();
}

} // namespace dutyon

#endif // _WIN32
