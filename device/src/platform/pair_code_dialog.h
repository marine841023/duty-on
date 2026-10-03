#pragma once
// ---------------------------------------------------------------------------
// 配对弹窗（仅 Windows PC 端）—— 深色卡片风，双状态：
//   未配对：验证码（OTP）风格 6 格输码 + [确定][取消]
//   已配对：「✓ 已配对成功」+ 设备列表 + 每台 [解除配对][清除 Wi-Fi] + [关闭]
//
// 视觉对齐程序整体深色风格（背景 #1A1D24 / 输入框 #12151A / 主按钮
// #4C8DFF / 次按钮 #2A2F37，标题栏 DWM 深色），不再用系统灰底。
//
// 交互（输码态，对齐常见验证码控件）：
//   数字键填入当前格自动跳下一格（重输覆盖）；Backspace 空格回退上一格；
//   ←/→ 移动；Enter=确定；Esc/关闭=取消；粘贴取剪贴板第一个数字；
//   未填满点“确定”：蜂鸣并定位第一个空格。
//
// 动作与数据经 Host 回调注入（main.cpp 用 lambda 组装），确认成功后
// 弹窗内部直接切到已配对态。线程约定：仅 UI 线程，run() 自持模态泵。
// ---------------------------------------------------------------------------
#ifdef _WIN32

#include <windows.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM
#include <dwmapi.h>

#include <functional>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

namespace dutyon {

class PairDialog {
public:
    struct Device {
        std::string id, label;
    };

    // 宿主动作集（全部可选；为空时对应按钮不生效）
    struct Host {
        std::function<bool(const std::string&)> confirm;     // 输码 → 是否成功
        std::function<void(const std::string&)> unpair;      // 解除配对
        std::function<void(const std::string&)> reset_wifi;  // 清除设备 Wi-Fi
        std::function<std::vector<Device>()> paired;         // 当前已配对列表
    };

    // 模态运行：点「配对设备」直接调用；关闭窗口才返回
    void run(HWND owner, const Host& host) {
        host_ = &host;
        devices_ = host.paired ? host.paired() : std::vector<Device>();
        mode_ = devices_.empty() ? Mode::kInput : Mode::kPaired;
        if (!create(owner)) return;
        MSG m;
        while (!done_ && GetMessageW(&m, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &m)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        destroy();
    }

private:
    static constexpr int kBoxes = 6;
    static constexpr int kEditIdBase = 100;  // 输入框 ID: 100~105
    static constexpr int kBtnUnpairBase = 200;  // 已配对设备行按钮 ID: 200+2i/201+2i

    enum class Mode { kInput, kPaired };

    struct BoxCtx {  // 子类引用数据（须活到控件销毁）
        PairDialog* dlg;
        int idx;
    };

    // ---- 深色配色（与 ImGui 菜单/portal 一致）----
    static constexpr COLORREF kBg = RGB(0x1A, 0x1D, 0x24);
    static constexpr COLORREF kEditBg = RGB(0x12, 0x15, 0x1A);
    static constexpr COLORREF kEditBorder = RGB(0x33, 0x38, 0x3F);
    static constexpr COLORREF kText = RGB(0xE8, 0xEA, 0xED);
    static constexpr COLORREF kTextDim = RGB(0x9A, 0xA0, 0xA6);
    static constexpr COLORREF kGreen = RGB(0x34, 0xC7, 0x59);
    static constexpr COLORREF kPrimary = RGB(0x4C, 0x8D, 0xFF);
    static constexpr COLORREF kPrimaryDown = RGB(0x3A, 0x76, 0xE0);
    static constexpr COLORREF kBtnBg = RGB(0x2A, 0x2F, 0x37);
    static constexpr COLORREF kBtnBorder = RGB(0x3A, 0x41, 0x50);
    static constexpr COLORREF kBtnDown = RGB(0x23, 0x27, 0x2E);
    static constexpr COLORREF kAccent = RGB(0x8A, 0xB4, 0xFF);

    bool create(HWND owner) {
        HINSTANCE hi = GetModuleHandleW(nullptr);
        static const wchar_t* kCls = L"DutyOnPairDialog";
        static bool cls_ready = false;
        if (!cls_ready) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &PairDialog::wndProc;
            wc.hInstance = hi;
            // IDC_ARROW = MAKEINTRESOURCEA(32512)（非 UNICODE 构建）；资源 ID
            // 只是低 16 位整数，A/W 同值，转 LPCWSTR 以匹配显式 W 版 API
            wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
            wc.hbrBackground = nullptr;  // 深色自绘（WM_ERASEBKGND）
            wc.lpszClassName = kCls;
            RegisterClassExW(&wc);
            cls_ready = true;
        }
        UINT dpi = 96;
        HDC dc = GetDC(nullptr);
        if (dc) {
            dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSY);
            ReleaseDC(nullptr, dc);
        }
        s_ = dpi / 96.0f;
        const int W = (int)(392 * s_);
        const int x = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - 460) / 2;
        hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kCls,
                                L"Duty On — 配对设备",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W,
                                (int)(100 * s_),  // 高度 build() 里按内容定
                                owner, nullptr, hi, this);
        if (!hwnd_) return false;
        // Win10 1903+ 深色标题栏（失败静默，仅影响标题栏颜色）
        BOOL dark = TRUE;
        if (FAILED(DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark))))
            DwmSetWindowAttribute(hwnd_, 19, &dark, sizeof(dark));

        font_title_ = makeFont(17, FW_SEMIBOLD);
        font_text_ = makeFont(14, FW_NORMAL);
        font_digit_ = makeFont(34, FW_SEMIBOLD);
        font_btn_ = makeFont(14, FW_NORMAL);
        bg_brush_ = CreateSolidBrush(kBg);
        edit_brush_ = CreateSolidBrush(kEditBg);

        build();
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        // 抢前台焦点：owner（桌宠窗口）是 WS_EX_NOACTIVATE，从不激活，弹窗
        // 不会自动成为前台——用户点菜单时本进程即前台，SetForegroundWindow
        // 允许同进程调用；否则得先点一下窗口键盘才生效
        SetForegroundWindow(hwnd_);
        SetActiveWindow(hwnd_);
        if (mode_ == Mode::kInput && edits_[0]) SetFocus(edits_[0]);
        return true;
    }

    void destroy() {
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        for (int i = 0; i < kBoxes; ++i) {
            delete ctx_[i];
            ctx_[i] = nullptr;
            edits_[i] = nullptr;
        }
        if (font_title_) { DeleteObject(font_title_); font_title_ = nullptr; }
        if (font_text_) { DeleteObject(font_text_); font_text_ = nullptr; }
        if (font_digit_) { DeleteObject(font_digit_); font_digit_ = nullptr; }
        if (font_btn_) { DeleteObject(font_btn_); font_btn_ = nullptr; }
        if (bg_brush_) { DeleteObject(bg_brush_); bg_brush_ = nullptr; }
        if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
        done_ = false;
    }

    // 销毁全部子控件并按当前 mode_ 重建布局/调整窗口高度
    void build() {
        HWND child = GetWindow(hwnd_, GW_CHILD);
        while (child) {
            const HWND next = GetWindow(child, GW_HWNDNEXT);
            DestroyWindow(child);
            child = next;
        }
        for (int i = 0; i < kBoxes; ++i) {  // BoxCtx 与 EDIT 同生命周期
            delete ctx_[i];
            ctx_[i] = nullptr;
            edits_[i] = nullptr;
        }
        unpair_btns_.clear();
        wifi_btns_.clear();
        HINSTANCE hi = GetModuleHandleW(nullptr);
        const float s = s_;
        const int W = (int)(392 * s);

        if (mode_ == Mode::kInput) {
            // 垂直居中布局：内容块 = 提示24 + 隙14 + 格58 + 隙18 + 钮34 = 148，
            // 窗口高 228（上下各留 40），起始 y 按客户区实测居中
            setHeight(228);
            RECT crc;
            GetClientRect(hwnd_, &crc);
            const int top = ((crc.bottom - crc.top) - (int)(148 * s)) / 2;
            label(hi, L"输入设备屏幕上的 6 位配对码", font_text_, kTextDim,
                  (int)(16 * s), top, W - (int)(32 * s), (int)(24 * s));
            const int bw = (int)(44 * s), bh = (int)(58 * s),
                      gap = (int)(12 * s);
            const int x0 = (W - (kBoxes * bw + (kBoxes - 1) * gap)) / 2,
                      by = top + (int)(38 * s);
            // 单行 EDIT 在过高的控件里文字顶部对齐（Win32 固有行为），
            // 故格框 58px 由 WM_PAINT 自绘，EDIT 内嵌 42px（贴近字号）
            // 垂直居中放置——文字即视觉居中；整格底色在 WM_PAINT 统一填充
            const int eh = (int)(42 * s);
            for (int i = 0; i < kBoxes; ++i) {
                const int ex = x0 + i * (bw + gap);
                box_rc_[i] = {ex, by, ex + bw, by + bh};  // 自绘格框（含边线）
                edits_[i] = CreateWindowExW(
                    0, L"EDIT", L"",
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER,
                    ex, by + (bh - eh) / 2, bw, eh, hwnd_,
                    (HMENU)(INT_PTR)(kEditIdBase + i), hi, nullptr);
                SendMessageW(edits_[i], WM_SETFONT, (WPARAM)font_digit_, TRUE);
                SendMessageW(edits_[i], EM_LIMITTEXT, 1, 0);
                ctx_[i] = new BoxCtx{this, i};
                SetWindowSubclass(edits_[i], &PairDialog::editProc, 1,
                                  (DWORD_PTR)ctx_[i]);
            }
            const int bwid = (int)(110 * s), bhei = (int)(34 * s),
                      bgap = (int)(16 * s);
            const int bx = (W - (bwid * 2 + bgap)) / 2,
                      bty = top + (int)(114 * s);
            btn_ok_ = button(hi, L"确定", IDOK, true, bx, bty, bwid, bhei);
            button(hi, L"取消", IDCANCEL, false, bx + bwid + bgap, bty, bwid,
                   bhei);
            SetFocus(edits_[0]);
        } else {
            // 已配对态：成功标示 + 每台设备一行（名称 + 两个操作按钮）
            const int n = (int)devices_.size();
            const int H = 84 + n * 46 + 60;
            setHeight(H);
            label(hi, L"\x2713  已配对成功", font_title_, kGreen,
                  (int)(16 * s), (int)(50 * s), W - (int)(32 * s),
                  (int)(30 * s));
            const int bwid = (int)(92 * s), bhei = (int)(30 * s),
                      bgap = (int)(10 * s);
            for (int i = 0; i < n; ++i) {
                const int ry = 92 + (int)(46 * s) * i;
                // 设备名（label）左对齐
                label(hi, utf8(devices_[i].label).c_str(), font_text_, kText,
                      (int)(24 * s), ry + (int)(4 * s),
                      W - (int)(48 * s) - (bwid * 2 + bgap * 2 + (int)(24 * s)),
                      (int)(24 * s));
                const int bx = W - (int)(24 * s) - bwid * 2 - bgap;
                HWND b1 = button(hi, L"解除配对", kBtnUnpairBase + 2 * i,
                                 false, bx, ry, bwid, bhei);
                HWND b2 = button(hi, L"清除 Wi-Fi", kBtnUnpairBase + 2 * i + 1,
                                 false, bx + bwid + bgap, ry, bwid, bhei);
                unpair_btns_.push_back(b1);
                wifi_btns_.push_back(b2);
            }
            const int cx = (W - bwid) / 2;
            button(hi, L"关闭", IDCANCEL, false, cx, H - 52, bwid, bhei);
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    // 窗口客户区高度调整（位置不动；总高 = 客户区 + 非客户区）
    void setHeight(int h) {
        RECT rc, crc;
        GetWindowRect(hwnd_, &rc);
        GetClientRect(hwnd_, &crc);
        const int nc = (rc.bottom - rc.top) - (crc.bottom - crc.top);
        SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left,
                     (int)(h * s_) + nc,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    HWND label(HINSTANCE hi, const wchar_t* text, HFONT f, COLORREF col,
               int x, int y, int w, int h) {
        HWND s = CreateWindowExW(0, L"STATIC", text,
                                 WS_CHILD | WS_VISIBLE | SS_CENTER, x, y, w, h,
                                 hwnd_, nullptr, hi, nullptr);
        SendMessageW(s, WM_SETFONT, (WPARAM)f, TRUE);
        SetWindowLongPtrW(s, GWLP_USERDATA, (LONG_PTR)col);  // 文字色（CTLCCOLOR 取）
        return s;
    }

    HWND button(HINSTANCE hi, const wchar_t* text, int id, bool primary,
                int x, int y, int w, int h) {
        HWND b = CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, x, y, w, h,
            hwnd_, (HMENU)(INT_PTR)id, hi, nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)font_btn_, TRUE);
        SetWindowLongPtrW(b, GWLP_USERDATA, primary ? 1 : 0);  // 样式标记
        return b;
    }

    void onOk() {  // 输码态确定
        std::string code;
        int first_empty = -1;
        for (int i = 0; i < kBoxes; ++i) {
            wchar_t buf[4] = {};
            GetWindowTextW(edits_[i], buf, 4);
            if (!buf[0]) {
                if (first_empty < 0) first_empty = i;
            } else {
                code += (char)buf[0];
            }
        }
        if (code.size() != kBoxes || first_empty >= 0) {
            MessageBeep(MB_ICONWARNING);  // 未填满：蜂鸣 + 定位第一个空格
            SetFocus(edits_[first_empty >= 0 ? first_empty : 0]);
            return;
        }
        bool ok = host_->confirm ? host_->confirm(code) : false;
        if (!ok) {
            MessageBoxW(hwnd_, L"配对码不匹配，请核对设备屏幕上的配对码",
                        L"Duty On", MB_OK | MB_ICONWARNING);
            for (int i = 0; i < kBoxes; ++i) SetWindowTextW(edits_[i], L"");
            SetFocus(edits_[0]);
            return;
        }
        // 成功：刷新已配对列表并切换到已配对态
        devices_ = host_->paired ? host_->paired() : std::vector<Device>();
        mode_ = Mode::kPaired;
        build();
    }

    void onUnpair(int idx) {  // 解除配对（行按钮）
        if (idx < 0 || idx >= (int)devices_.size()) return;
        if (host_->unpair) host_->unpair(devices_[idx].id);
        devices_ = host_->paired ? host_->paired() : std::vector<Device>();
        if (devices_.empty()) mode_ = Mode::kInput;  // 全解除 → 回输码态
        build();
    }

    void onResetWifi(int idx) {  // 清除设备 Wi-Fi（重新配网）
        if (idx < 0 || idx >= (int)devices_.size()) return;
        if (host_->reset_wifi) host_->reset_wifi(devices_[idx].id);
        devices_ = host_->paired ? host_->paired() : std::vector<Device>();
        build();
    }

    void finish() { done_ = true; }

    static HFONT makeFont(int h, int weight) {
        return CreateFontW(-h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }

    // UTF-8 → UTF-16（设备 label）
    static std::wstring utf8(const std::string& u8) {
        if (u8.empty()) return std::wstring();
        const int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr,
                                          0);
        std::wstring w((size_t)(n > 1 ? n - 1 : 0), L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, w.data(), n);
        return w;
    }

    // ---- 输入框子类过程：数字填充/跳格导航/粘贴取数字 ----
    static LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l,
                                     UINT_PTR, DWORD_PTR dw) {
        BoxCtx* c = (BoxCtx*)dw;
        PairDialog* d = c->dlg;
        switch (m) {
        case WM_GETDLGCODE:  // 接管全部按键：IsDialogMessage 不代吃 Enter/Esc/方向键
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS |
                   DLGC_WANTTAB;
        case WM_CHAR: {
            const wchar_t ch = (wchar_t)w;
            if (ch >= L'0' && ch <= L'9') {
                wchar_t one[2] = {ch, 0};
                SetWindowTextW(h, one);  // 覆盖式填入；跳格由 EN_CHANGE 处理
                return 0;
            }
            if (ch == VK_BACK) {
                wchar_t buf[4] = {};
                GetWindowTextW(h, buf, 4);
                if (!buf[0] && c->idx > 0) {  // 当前格已空：回退上一格并清空
                    SetWindowTextW(d->edits_[c->idx - 1], L"");
                    SetFocus(d->edits_[c->idx - 1]);
                    return 0;
                }
                break;  // 当前格有字：默认处理（删除）
            }
            if (ch == L'\r') {
                SendMessageW(d->hwnd_, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED),
                             0);
                return 0;
            }
            if (ch == VK_ESCAPE) {
                SendMessageW(d->hwnd_, WM_COMMAND,
                             MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                return 0;
            }
            return 0;  // 其余字符全部吞掉，只收数字
        }
        case WM_KEYDOWN:
            if (w == VK_TAB) {  // Shift+Tab 反向；末格→确定
                const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                if (!shift && c->idx < kBoxes - 1)
                    SetFocus(d->edits_[c->idx + 1]);
                else if (!shift && d->btn_ok_)
                    SetFocus(d->btn_ok_);
                else if (c->idx > 0)
                    SetFocus(d->edits_[c->idx - 1]);
                return 0;
            }
            if (w == VK_LEFT && c->idx > 0) {
                SetFocus(d->edits_[c->idx - 1]);
                return 0;
            }
            if (w == VK_RIGHT && c->idx < kBoxes - 1) {
                SetFocus(d->edits_[c->idx + 1]);
                return 0;
            }
            break;
        case WM_PASTE: {  // 粘贴：取剪贴板文本里的第一个数字填入当前格
            wchar_t digit = 0;
            if (OpenClipboard(h)) {
                HANDLE data = GetClipboardData(CF_UNICODETEXT);
                if (data) {
                    const wchar_t* t = (const wchar_t*)GlobalLock(data);
                    if (t) {
                        for (; *t; ++t)
                            if (*t >= L'0' && *t <= L'9') { digit = *t; break; }
                        GlobalUnlock(data);
                    }
                }
                CloseClipboard();
            }
            if (digit) {
                wchar_t one[2] = {digit, 0};
                SetWindowTextW(h, one);
            }
            return 0;
        }
        }
        return DefSubclassProc(h, m, w, l);
    }

    // ---- owner-draw 按钮绘制：圆角 8px，主/次两档配色 ----
    void drawButton(const DRAWITEMSTRUCT* d) {
        const bool primary = GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA) == 1;
        const bool down = (d->itemState & ODS_SELECTED) != 0;
        RECT r = d->rcItem;
        HDC dc = d->hDC;
        // 底色
        HBRUSH bg = CreateSolidBrush(down ? (primary ? kPrimaryDown : kBtnDown)
                                          : (primary ? kPrimary : kBtnBg));
        HPEN pen = CreatePen(PS_SOLID, 1, primary ? (down ? kPrimaryDown : kPrimary)
                                                 : kBtnBorder);
        HGDIOBJ ob = SelectObject(dc, bg);
        HGDIOBJ op = SelectObject(dc, pen);
        const int rad = (int)(8 * s_);
        RoundRect(dc, r.left, r.top, r.right, r.bottom, rad * 2, rad * 2);
        SelectObject(dc, op);
        SelectObject(dc, ob);
        DeleteObject(pen);
        DeleteObject(bg);
        // 文字
        wchar_t text[64] = {};
        GetWindowTextW(d->hwndItem, text, 64);
        SetTextColor(dc, primary ? RGB(255, 255, 255) : kAccent);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ of = SelectObject(dc, font_btn_);
        RECT tr = r;
        DrawTextW(dc, text, -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
        // 焦点框（键盘导航可见性）
        if (d->itemState & ODS_FOCUS) {
            RECT fr = r;
            InflateRect(&fr, -3, -3);
            DrawFocusRect(dc, &fr);
        }
    }

    // ---- 主窗口过程 ----
    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        PairDialog* self = nullptr;
        if (m == WM_CREATE) {
            auto* cs = (LPCREATESTRUCTW)l;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        } else {
            self = (PairDialog*)GetWindowLongPtrW(h, GWLP_USERDATA);
        }
        switch (m) {
        case WM_ERASEBKGND: {  // 深色底
            HDC dc = (HDC)w;
            RECT rc;
            GetClientRect(h, &rc);
            HBRUSH bg = CreateSolidBrush(kBg);
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            return 1;
        }
        case WM_PAINT: {  // 输码态：画 6 格框（底色 + 边线，EDIT 内嵌其中）
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (self && self->mode_ == Mode::kInput) {
                HBRUSH fill = CreateSolidBrush(kEditBg);
                for (int i = 0; i < kBoxes; ++i) {
                    if (!self->edits_[i]) continue;
                    FillRect(dc, &self->box_rc_[i], fill);
                }
                DeleteObject(fill);
                HPEN pen = CreatePen(PS_SOLID, 1, kEditBorder);
                HGDIOBJ op = SelectObject(dc, pen);
                HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                for (int i = 0; i < kBoxes; ++i) {
                    if (!self->edits_[i]) continue;
                    Rectangle(dc, self->box_rc_[i].left, self->box_rc_[i].top,
                              self->box_rc_[i].right, self->box_rc_[i].bottom);
                }
                SelectObject(dc, ob);
                SelectObject(dc, op);
                DeleteObject(pen);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {  // 点击格框（含 EDIT 外的 8px 上下边）聚焦对应格
            if (self && self->mode_ == Mode::kInput) {
                const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
                for (int i = 0; i < kBoxes; ++i) {
                    if (self->edits_[i] && PtInRect(&self->box_rc_[i], pt)) {
                        SetFocus(self->edits_[i]);
                        break;
                    }
                }
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {  // STATIC 深底 + 各自文字色（GWLP_USERDATA）
            HDC dc = (HDC)w;
            HWND ctrl = (HWND)l;
            SetTextColor(dc, (COLORREF)GetWindowLongPtrW(ctrl, GWLP_USERDATA));
            SetBkColor(dc, kBg);
            return (LRESULT)self->bg_brush_;
        }
        case WM_CTLCOLOREDIT: {  // EDIT 深底白字
            HDC dc = (HDC)w;
            SetTextColor(dc, kText);
            SetBkColor(dc, kEditBg);
            return (LRESULT)self->edit_brush_;
        }
        case WM_DRAWITEM: {
            if (self) self->drawButton((const DRAWITEMSTRUCT*)l);
            return TRUE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(w), code = HIWORD(w);
            if (code == BN_CLICKED) {
                if (id == IDOK && self && self->mode_ == Mode::kInput) {
                    self->onOk();
                    return 0;
                }
                if (id == IDCANCEL) {  // 取消/关闭
                    if (self) self->finish();
                    return 0;
                }
                if (self && id >= kBtnUnpairBase) {  // 已配对设备行按钮
                    const int idx = (id - kBtnUnpairBase) / 2;
                    if ((id - kBtnUnpairBase) % 2 == 0)
                        self->onUnpair(idx);
                    else
                        self->onResetWifi(idx);
                    return 0;
                }
            }
            // 输码格填入 1 位 → 自动跳下一格
            if (code == EN_CHANGE && id >= kEditIdBase &&
                id < kEditIdBase + kBoxes && self) {
                wchar_t buf[4] = {};
                GetWindowTextW(self->edits_[id - kEditIdBase], buf, 4);
                if (buf[0] && id < kEditIdBase + kBoxes - 1)
                    SetFocus(self->edits_[id - kEditIdBase + 1]);
                return 0;
            }
            break;
        }
        case WM_CLOSE:  // 标题栏 X / Alt+F4 = 关闭
            if (self) self->finish();
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    HWND hwnd_ = nullptr;
    HWND edits_[kBoxes] = {};
    HWND btn_ok_ = nullptr;
    RECT box_rc_[kBoxes] = {};  // 输码格自绘框（EDIT 内嵌其中）
    std::vector<HWND> unpair_btns_;
    std::vector<HWND> wifi_btns_;
    HFONT font_title_ = nullptr;
    HFONT font_text_ = nullptr;
    HFONT font_digit_ = nullptr;
    HFONT font_btn_ = nullptr;
    HBRUSH bg_brush_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    BoxCtx* ctx_[kBoxes] = {};
    const Host* host_ = nullptr;
    std::vector<Device> devices_;
    Mode mode_ = Mode::kInput;
    float s_ = 1.0f;    // DPI 缩放
    bool done_ = false;
};

} // namespace dutyon

#endif // _WIN32
