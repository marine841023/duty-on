#pragma once
// ---------------------------------------------------------------------------
// 云端账户登录弹窗（仅 Windows PC 端）—— 深色卡片风，照 pair_code_dialog.h
// 的骨架（DWM 深色标题栏 / 自绘圆角按钮 / 模态消息泵 / Host 回调注入）。
//
// 布局：标题 + 说明行 + 三输入框（服务器地址 / 用户名 / 密码 ES_PASSWORD）
//       + [登录] [注册] 双按钮 + 状态提示行（错误红 / 连接中灰）。
// 交互：Enter=登录，Esc/关闭=取消，Tab 切换焦点；登录/注册期间状态行显示
//       「连接中…」（回调在 UI 线程同步执行 HTTP，主循环停帧可接受，
//       与 pair 弹窗同一约定）。
//
// Host.auth(action, server, user, pass)：action = "login"/"register"；
// 返回空串 = 成功（弹窗关闭），否则为 UTF-8 错误文本（状态行红字显示）。
// ---------------------------------------------------------------------------
#ifdef _WIN32

#include <windows.h>
#include <dwmapi.h>

#include <functional>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

namespace dutyon {

class CloudLoginDialog {
public:
    struct Host {
        // action: "login" / "register"；空返回 = 成功
        std::function<std::string(const std::string& action,
                                  const std::string& server,
                                  const std::string& user,
                                  const std::string& pass)>
            auth;
    };

    // 模态运行；default_server 预填服务器地址（上次成功登录的值）
    void run(HWND owner, const Host& host,
             const std::string& default_server = "") {
        host_ = &host;
        default_server_ = default_server;
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
    static constexpr int kEdServer = 101;
    static constexpr int kEdUser = 102;
    static constexpr int kEdPass = 103;
    static constexpr int kBtnLogin = 201;
    static constexpr int kBtnRegister = 202;

    // ---- 深色配色（与 pair_code_dialog / ImGui 菜单一致）----
    static constexpr COLORREF kBg = RGB(0x1A, 0x1D, 0x24);
    static constexpr COLORREF kEditBg = RGB(0x12, 0x15, 0x1A);
    static constexpr COLORREF kText = RGB(0xE8, 0xEA, 0xED);
    static constexpr COLORREF kTextDim = RGB(0x9A, 0xA0, 0xA6);
    static constexpr COLORREF kRed = RGB(0xE5, 0x55, 0x55);
    static constexpr COLORREF kPrimary = RGB(0x4C, 0x8D, 0xFF);
    static constexpr COLORREF kPrimaryDown = RGB(0x3A, 0x76, 0xE0);
    static constexpr COLORREF kBtnBg = RGB(0x2A, 0x2F, 0x37);
    static constexpr COLORREF kBtnBorder = RGB(0x3A, 0x41, 0x50);
    static constexpr COLORREF kBtnDown = RGB(0x23, 0x27, 0x2E);
    static constexpr COLORREF kAccent = RGB(0x8A, 0xB4, 0xFF);

    struct EditCtx {  // 子类引用（须活到控件销毁）
        CloudLoginDialog* dlg;
    };

    bool create(HWND owner) {
        HINSTANCE hi = GetModuleHandleW(nullptr);
        static const wchar_t* kCls = L"DutyOnCloudLogin";
        static bool cls_ready = false;
        if (!cls_ready) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &CloudLoginDialog::wndProc;
            wc.hInstance = hi;
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
        const int H = (int)(300 * s_);
        const int x = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
        const int y = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
        hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kCls,
                                L"Duty On — 云端账户",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, W, H,
                                owner, nullptr, hi, this);
        if (!hwnd_) return false;
        // Win10 1903+ 深色标题栏（失败静默）
        BOOL dark = TRUE;
        if (FAILED(DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark))))
            DwmSetWindowAttribute(hwnd_, 19, &dark, sizeof(dark));

        font_title_ = makeFont(17, FW_SEMIBOLD);
        font_text_ = makeFont(14, FW_NORMAL);
        font_btn_ = makeFont(14, FW_NORMAL);
        bg_brush_ = CreateSolidBrush(kBg);
        edit_brush_ = CreateSolidBrush(kEditBg);

        build();
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        // 抢前台焦点（owner 是 WS_EX_NOACTIVATE，同 pair 弹窗）
        SetForegroundWindow(hwnd_);
        SetActiveWindow(hwnd_);
        if (ed_server_) SetFocus(ed_server_);
        return true;
    }

    void destroy() {
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        for (auto& c : ctx_) { delete c; c = nullptr; }
        ed_server_ = ed_user_ = ed_pass_ = nullptr;
        btn_login_ = nullptr;
        if (font_title_) { DeleteObject(font_title_); font_title_ = nullptr; }
        if (font_text_) { DeleteObject(font_text_); font_text_ = nullptr; }
        if (font_btn_) { DeleteObject(font_btn_); font_btn_ = nullptr; }
        if (bg_brush_) { DeleteObject(bg_brush_); bg_brush_ = nullptr; }
        if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
        done_ = false;
    }

    HWND label(HINSTANCE hi, const wchar_t* text, HFONT f, COLORREF col,
               int x, int y, int w, int h) {
        HWND s = CreateWindowExW(0, L"STATIC", text,
                                 WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, w, h,
                                 hwnd_, nullptr, hi, nullptr);
        SendMessageW(s, WM_SETFONT, (WPARAM)f, TRUE);
        SetWindowLongPtrW(s, GWLP_USERDATA, (LONG_PTR)col);
        return s;
    }

    HWND button(HINSTANCE hi, const wchar_t* text, int id, bool primary,
                int x, int y, int w, int h) {
        HWND b = CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, x, y, w, h,
            hwnd_, (HMENU)(INT_PTR)id, hi, nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)font_btn_, TRUE);
        SetWindowLongPtrW(b, GWLP_USERDATA, primary ? 1 : 0);
        return b;
    }

    HWND edit(HINSTANCE hi, int id, const wchar_t* initial, bool password,
              int x, int y, int w, int h, int limit) {
        HWND e = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", initial ? initial : L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_LEFT | ES_AUTOHSCROLL |
                (password ? ES_PASSWORD : 0),
            x, y, w, h, hwnd_, (HMENU)(INT_PTR)id, hi, nullptr);
        SendMessageW(e, WM_SETFONT, (WPARAM)font_text_, TRUE);
        SendMessageW(e, EM_LIMITTEXT, limit, 0);
        auto* c = new EditCtx{this};
        ctx_.push_back(c);
        SetWindowSubclass(e, &CloudLoginDialog::editProc, 1, (DWORD_PTR)c);
        return e;
    }

    void build() {
        HINSTANCE hi = GetModuleHandleW(nullptr);
        const float s = s_;
        const int W = (int)(392 * s);
        const int pad = (int)(24 * s);
        int y = (int)(22 * s);

        label(hi, L"云端账户", font_title_, kText, pad, y,
              W - pad * 2, (int)(26 * s));
        y += (int)(32 * s);
        label(hi, L"登录后自动同步角色 / 音色 / 配置，支持应用内更新",
              font_text_, kTextDim, pad, y, W - pad * 2, (int)(20 * s));
        y += (int)(32 * s);

        const int lab_w = (int)(64 * s);
        const int row_h = (int)(34 * s);
        const int row_gap = (int)(14 * s);
        struct Row { const wchar_t* name; int id; bool pass; };
        const Row rows[] = {{L"服务器", kEdServer, false},
                            {L"用户名", kEdUser, false},
                            {L"密码", kEdPass, true}};
        for (const auto& r : rows) {
            label(hi, r.name, font_text_, kTextDim, pad, y + (int)(8 * s),
                  lab_w, (int)(20 * s));
            const wchar_t* init = nullptr;
            if (r.id == kEdServer && !default_server_.empty())
                init = utf8(default_server_).c_str();
            HWND e = edit(hi, r.id, init, r.pass, pad + lab_w, y,
                          W - pad * 2 - lab_w, row_h, r.pass ? 128 : 64);
            if (r.id == kEdServer) ed_server_ = e;
            if (r.id == kEdUser) ed_user_ = e;
            if (r.id == kEdPass) ed_pass_ = e;
            y += row_h + row_gap;
        }

        const int bwid = (int)(110 * s), bhei = (int)(34 * s),
                  bgap = (int)(16 * s);
        const int bx = (W - (bwid * 2 + bgap)) / 2;
        btn_login_ = button(hi, L"登录", kBtnLogin, true, bx, y, bwid, bhei);
        button(hi, L"注册", kBtnRegister, false, bx + bwid + bgap, y, bwid,
               bhei);
        y += bhei + (int)(16 * s);

        status_ = label(hi, L"", font_text_, kTextDim, pad, y, W - pad * 2,
                        (int)(20 * s));
    }

    void setStatus(const std::wstring& text, bool error) {
        if (!status_) return;
        SetWindowLongPtrW(status_, GWLP_USERDATA,
                          (LONG_PTR)(error ? kRed : kTextDim));
        SetWindowTextW(status_, text.c_str());
        InvalidateRect(status_, nullptr, TRUE);
        UpdateWindow(status_);  // 同步重绘（回调即将阻塞 UI 线程）
    }

    void onAuth(bool register_mode) {
        if (!host_ || !host_->auth) return;
        wchar_t ws[160] = {}, wu[80] = {}, wp[160] = {};
        if (ed_server_) GetWindowTextW(ed_server_, ws, 160);
        if (ed_user_) GetWindowTextW(ed_user_, wu, 80);
        if (ed_pass_) GetWindowTextW(ed_pass_, wp, 160);
        const std::string server = utf16(ws), user = utf16(wu),
                            pass = utf16(wp);
        if (user.empty() || pass.empty()) {
            setStatus(L"请输入用户名和密码", true);
            SetFocus(user.empty() ? ed_user_ : ed_pass_);
            return;
        }
        setStatus(register_mode ? L"注册中…" : L"连接中…", false);
        const std::string err = host_->auth(
            register_mode ? "register" : "login", server, user, pass);
        if (err.empty()) {
            finish();  // 成功：关闭弹窗（main 侧接 requestRestore）
            return;
        }
        setStatus(utf8(err), true);
        SetFocus(ed_user_);
    }

    void finish() { done_ = true; }

    static HFONT makeFont(int h, int weight) {
        return CreateFontW(-h, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }

    static std::wstring utf8(const std::string& u8) {
        if (u8.empty()) return std::wstring();
        const int n = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, nullptr,
                                          0);
        std::wstring w((size_t)(n > 1 ? n - 1 : 0), L'\0');
        if (n > 1) MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), -1, w.data(), n);
        return w;
    }

    static std::string utf16(const std::wstring& w) {
        if (w.empty()) return std::string();
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr,
                                          0, nullptr, nullptr);
        std::string u8((size_t)(n > 1 ? n - 1 : 0), '\0');
        if (n > 1)
            WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, u8.data(), n,
                                nullptr, nullptr);
        return u8;
    }

    // ---- 输入框子类过程：Enter=登录 / Esc=取消 ----
    static LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l,
                                     UINT_PTR, DWORD_PTR dw) {
        EditCtx* c = (EditCtx*)dw;
        CloudLoginDialog* d = c->dlg;
        switch (m) {
        case WM_GETDLGCODE:  // 接管 Enter/Esc（IsDialogMessage 不代吃）
            return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;
        case WM_CHAR:
            if ((wchar_t)w == L'\r') {
                SendMessageW(d->hwnd_, WM_COMMAND,
                             MAKEWPARAM(kBtnLogin, BN_CLICKED), 0);
                return 0;
            }
            if ((wchar_t)w == VK_ESCAPE) {
                SendMessageW(d->hwnd_, WM_COMMAND,
                             MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                return 0;
            }
            break;
        }
        return DefSubclassProc(h, m, w, l);
    }

    // ---- owner-draw 按钮绘制：圆角 8px，主/次两档配色（同 pair 弹窗）----
    void drawButton(const DRAWITEMSTRUCT* d) {
        const bool primary = GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA) == 1;
        const bool down = (d->itemState & ODS_SELECTED) != 0;
        RECT r = d->rcItem;
        HDC dc = d->hDC;
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
        wchar_t text[64] = {};
        GetWindowTextW(d->hwndItem, text, 64);
        SetTextColor(dc, primary ? RGB(255, 255, 255) : kAccent);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ of = SelectObject(dc, font_btn_);
        RECT tr = r;
        DrawTextW(dc, text, -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
        if (d->itemState & ODS_FOCUS) {
            RECT fr = r;
            InflateRect(&fr, -3, -3);
            DrawFocusRect(dc, &fr);
        }
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        CloudLoginDialog* self = nullptr;
        if (m == WM_CREATE) {
            auto* cs = (LPCREATESTRUCTW)l;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        } else {
            self = (CloudLoginDialog*)GetWindowLongPtrW(h, GWLP_USERDATA);
        }
        switch (m) {
        case WM_ERASEBKGND: {
            HDC dc = (HDC)w;
            RECT rc;
            GetClientRect(h, &rc);
            HBRUSH bg = CreateSolidBrush(kBg);
            FillRect(dc, &rc, bg);
            DeleteObject(bg);
            return 1;
        }
        case WM_CTLCOLORSTATIC: {  // STATIC 深底 + 各自文字色
            HDC dc = (HDC)w;
            HWND ctrl = (HWND)l;
            SetTextColor(dc, (COLORREF)GetWindowLongPtrW(ctrl, GWLP_USERDATA));
            SetBkColor(dc, kBg);
            return (LRESULT)(self ? self->bg_brush_ : 0);
        }
        case WM_CTLCOLOREDIT: {  // EDIT 深底白字
            HDC dc = (HDC)w;
            SetTextColor(dc, kText);
            SetBkColor(dc, kEditBg);
            return (LRESULT)(self ? self->edit_brush_ : 0);
        }
        case WM_DRAWITEM:
            if (self) self->drawButton((const DRAWITEMSTRUCT*)l);
            return TRUE;
        case WM_COMMAND: {
            const int id = LOWORD(w), code = HIWORD(w);
            if (code == BN_CLICKED && self) {
                if (id == kBtnLogin) {
                    self->onAuth(false);
                    return 0;
                }
                if (id == kBtnRegister) {
                    self->onAuth(true);
                    return 0;
                }
                if (id == IDCANCEL) {
                    self->finish();
                    return 0;
                }
            }
            break;
        }
        case WM_CLOSE:
            if (self) self->finish();
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    HWND hwnd_ = nullptr;
    HWND ed_server_ = nullptr;
    HWND ed_user_ = nullptr;
    HWND ed_pass_ = nullptr;
    HWND btn_login_ = nullptr;
    HWND status_ = nullptr;
    HFONT font_title_ = nullptr;
    HFONT font_text_ = nullptr;
    HFONT font_btn_ = nullptr;
    HBRUSH bg_brush_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    std::vector<EditCtx*> ctx_;
    const Host* host_ = nullptr;
    std::string default_server_;
    float s_ = 1.0f;
    bool done_ = false;
};

} // namespace dutyon

#endif // _WIN32
