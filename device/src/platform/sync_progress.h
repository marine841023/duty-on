#pragma once
// ---------------------------------------------------------------------------
// 设备程序同步进度窗口（仅 Windows PC 端）—— header-only，被 main.cpp 的
// 自动更新工作线程使用。独立于宠物 GLFW 窗口：WS_EX_TOPMOST 置顶弹出。
//
// 线程约定：create/update/pump/destroy 必须都在同一个工作线程调用（窗口
// 归属该线程，pump() 自持非阻塞消息泵），无跨线程 marshaling。
//
// 用法：
//   SyncProgressDialog dlg;
//   dlg.create();
//   while (proc_running) { dlg.pump(); dlg.update(pct, L"阶段名 45%"); }
//   dlg.update(100, L"更新完成"); Sleep(2000); dlg.destroy();
// ---------------------------------------------------------------------------
#ifdef _WIN32

#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

namespace dutyon {

class SyncProgressDialog {
public:
    ~SyncProgressDialog() { destroy(); }

    // 创建置顶进度窗口（居中，约 400x150）。成功返回 true。
    bool create(const wchar_t* title = L"Duty On \u2014 \u8bbe\u5907\u7a0b\u5e8f\u66f4\u65b0") {
        if (hwnd_) return true;
        HINSTANCE hi = GetModuleHandleW(nullptr);
        static bool cls_ready = false;
        static const wchar_t* kCls = L"DutyOnSyncProgress";
        if (!cls_ready) {
            INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS};
            InitCommonControlsEx(&icc);
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &SyncProgressDialog::wndProc;
            wc.hInstance = hi;
            // IDC_ARROW = MAKEINTRESOURCEA(32512)（非 UNICODE 构建）；资源 ID 只是
            // 低 16 位整数，A/W 同值，转 LPCWSTR 以匹配显式 W 版 API。
            wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = kCls;
            RegisterClassExW(&wc);
            cls_ready = true;
        }
        const int w = 400, h = 150;
        const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kCls, title,
                                WS_POPUP | WS_BORDER, x, y, w, h,
                                nullptr, nullptr, hi, nullptr);
        if (!hwnd_) return false;

        const int mx = 20;                 // 左右边距
        const int cw = w - mx * 2 - 4;     // 控件宽（预留边框）
        HFONT f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        hwnd_title_ = CreateWindowExW(0, L"STATIC", title,
                                      WS_CHILD | WS_VISIBLE | SS_CENTER,
                                      mx, 16, cw, 22, hwnd_, nullptr, hi, nullptr);
        hwnd_stage_ = CreateWindowExW(0, L"STATIC",
                                      L"\u51c6\u5907\u4e2d\u2026",
                                      WS_CHILD | WS_VISIBLE | SS_CENTER,
                                      mx, 48, cw, 40, hwnd_, nullptr, hi, nullptr);
        hwnd_bar_ = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                                    WS_CHILD | WS_VISIBLE,
                                    mx, 100, cw, 20, hwnd_, nullptr, hi, nullptr);
        SendMessageW(hwnd_bar_, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(hwnd_bar_, PBM_SETPOS, 0, 0);
        if (f) {
            SendMessageW(hwnd_title_, WM_SETFONT, (WPARAM)f, TRUE);
            SendMessageW(hwnd_stage_, WM_SETFONT, (WPARAM)f, TRUE);
        }
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        return true;
    }

    // 更新进度条位置（0-100）与阶段文字（stage 可为空表示只改进度）
    void update(int pct, const wchar_t* stage) {
        if (!hwnd_) return;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        if (stage) SetWindowTextW(hwnd_stage_, stage);
        SendMessageW(hwnd_bar_, PBM_SETPOS, (WPARAM)pct, 0);
    }

    // 非阻塞抽取并派发本线程消息（在等待子进程的轮询里周期调用）
    void pump() {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    void destroy() {
        if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
        hwnd_title_ = hwnd_stage_ = hwnd_bar_ = nullptr;
    }

private:
    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        return DefWindowProcW(h, m, w, l);
    }

    HWND hwnd_ = nullptr;
    HWND hwnd_title_ = nullptr;
    HWND hwnd_stage_ = nullptr;
    HWND hwnd_bar_ = nullptr;
};

} // namespace dutyon

#endif // _WIN32
