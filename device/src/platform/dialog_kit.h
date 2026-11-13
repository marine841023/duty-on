#pragma once
// ---------------------------------------------------------------------------
// 弹窗外观组件（仅 Windows PC 端，header-only）。
//
// 云端账户、配对设备、设备程序更新、各种提示框原先各画各的，配色也和桌宠
// 本体不一致。这里提供一套共用的外观，和设备屏、官网是同一套语言：
//   - 深色舞台底、值班牌式的面板（上亮下暗的渐变 + 发丝边）；
//   - 文字只有纸白 / 次要 / 说明三档；主题色（琥珀）给主要按钮、焦点和进度；
//   - 珊瑚红只用于"需要你"：出错、确认破坏性操作。
//
// 窗口没有系统标题栏：自绘标题行（状态灯 + 标题 + 关闭），可拖动；Win11 上
// 圆角。形状用 GDI+ 画（抗锯齿），文字用 GDI ClearType。
//
// 用法：继承 Dialog，在 build() 里摆控件，在 paintContent() 里画文字和面板，
// 在 onCommand() 里处理按钮。Enter = 默认按钮，Esc / 关闭 = 取消。
// ---------------------------------------------------------------------------
#ifdef _WIN32

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <string>
#include <vector>

// GDI+ 头文件依赖 min/max 宏；本项目部分编译单元定义了 NOMINMAX
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")

namespace dutyon {
namespace dk {

// ---- 色板（与 ui/task_panel.cpp、server/web/style.css 一致）----
constexpr COLORREF kStage = RGB(0x12, 0x12, 0x15);
constexpr COLORREF kTileTop = RGB(0x24, 0x24, 0x2A);
constexpr COLORREF kTileBot = RGB(0x17, 0x17, 0x1B);
constexpr COLORREF kField = RGB(0x0B, 0x0B, 0x0E);
constexpr COLORREF kHair = RGB(0x33, 0x33, 0x3A);
constexpr COLORREF kPaper = RGB(0xF3, 0xEF, 0xE7);
constexpr COLORREF kDim = RGB(0xA2, 0x9D, 0x93);
constexpr COLORREF kFaint = RGB(0x76, 0x72, 0x6A);
constexpr COLORREF kInk = RGB(0xE0, 0xA6, 0x53);
constexpr COLORREF kInkDown = RGB(0xC8, 0x92, 0x44);
constexpr COLORREF kInkDark = RGB(0x15, 0x12, 0x0C);
constexpr COLORREF kCoral = RGB(0xFF, 0x5F, 0x52);
constexpr COLORREF kCoralDown = RGB(0xE8, 0x4E, 0x42);
constexpr COLORREF kCoralInk = RGB(0x2A, 0x09, 0x06);

inline Gdiplus::Color gp(COLORREF c, BYTE a = 255) {
    return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

inline void ensureGdiplus() {
    static ULONG_PTR token = 0;
    if (token) return;
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&token, &in, nullptr);
}

inline std::wstring widen(const std::string& u8) {
    if (u8.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), &w[0], n);
    return w;
}
inline std::string narrow(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr,
          nullptr);
    std::string u8((size_t)n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &u8[0], n, nullptr, nullptr);
    return u8;
}

inline HFONT makeFont(int px, int weight, const wchar_t* face = L"Microsoft YaHei UI") {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
         DEFAULT_PITCH | FF_DONTCARE, face);
}

// 圆角矩形：上下渐变填充 + 可选描边
inline void roundRect(Gdiplus::Graphics& g, const RECT& r, float radius, COLORREF top,
        COLORREF bottom, bool border, COLORREF border_color = kHair,
        float border_w = 1.0f) {
    const float x = (float)r.left, y = (float)r.top;
    const float w = (float)(r.right - r.left) - 1.0f, h = (float)(r.bottom - r.top) - 1.0f;
    if (w <= 0 || h <= 0) return;
    const float d = (std::min)(radius * 2.0f, (std::min)(w, h));
    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
    Gdiplus::LinearGradientBrush fill(Gdiplus::PointF(x, y - 1), Gdiplus::PointF(x, y + h + 1),
     gp(top), gp(bottom));
    g.FillPath(&fill, &path);
    if (border) {
        Gdiplus::Pen pen(gp(border_color), border_w);
        g.DrawPath(&pen, &path);
    }
}
inline void circle(Gdiplus::Graphics& g, float cx, float cy, float r, COLORREF c, BYTE a = 255) {
    Gdiplus::SolidBrush b(gp(c, a));
    g.FillEllipse(&b, cx - r, cy - r, r * 2, r * 2);
}
inline void ring(Gdiplus::Graphics& g, float cx, float cy, float r, float w, COLORREF c,
   BYTE a = 255) {
    Gdiplus::Pen p(gp(c, a), w);
    g.DrawEllipse(&p, cx - r, cy - r, r * 2, r * 2);
}
inline void drawText(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF col,
       UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    if (s.empty()) return;
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, col);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}
inline int textHeight(HDC dc, const std::wstring& s, int width, HFONT f) {
    RECT r = {0, 0, width, 0};
    HGDIOBJ old = SelectObject(dc, f);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom;
}

enum class Btn { Primary = 1, Secondary = 2, Danger = 3, Close = 4 };
enum class Tone { Info, Alert };

class Dialog {
public:
    virtual ~Dialog() { destroyWindow(); }

protected:
    // ---- 子类实现 ----
    virtual void paintContent(HDC, Gdiplus::Graphics&) {}
    virtual void onCommand(int /*id*/, int /*code*/, HWND /*ctrl*/) {}
    virtual void onTimer(UINT_PTR) {}
    virtual void onClick(POINT) {}
    virtual COLORREF editColor(HWND) { return kPaper; }

    int S(float v) const { return (int)(v * s_ + 0.5f); }
    int unS(int px) const { return (int)(px / s_ + 0.5f); }  // 物理像素 → 逻辑像素
    HWND hwnd() const { return hwnd_; }
    void close() { done_ = true; }
    void setDefault(int id) { default_id_ = id; }
    void setTone(Tone t) { tone_ = t; if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    void repaint() { if (hwnd_) { InvalidateRect(hwnd_, nullptr, FALSE); UpdateWindow(hwnd_); } }

    static constexpr int kCaption = 52;  // 标题行高（逻辑像素）
    static constexpr int kPad = 24;      // 内容左右边距

    bool createWindow(HWND owner, const wchar_t* title, int logical_w, int logical_h,
        bool topmost = false) {
        ensureGdiplus();
 HINSTANCE hi = GetModuleHandleW(nullptr);
        static bool cls_ready = false;
 static const wchar_t* kCls = L"DutyOnDialogKit";
        if (!cls_ready) {
     WNDCLASSEXW wc;
     ZeroMemory(&wc, sizeof(wc));
     wc.cbSize = sizeof(wc);
     wc.style = CS_DROPSHADOW;
     wc.lpfnWndProc = &Dialog::wndProc;
     wc.hInstance = hi;
     wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
     wc.lpszClassName = kCls;
     RegisterClassExW(&wc);
            cls_ready = true;
        }
        UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
        if (!dpi) dpi = 96;
        s_ = dpi / 96.0f;
        title_ = title ? title : L"";
        HMONITOR mon = MonitorFromWindow(owner, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
 GetMonitorInfoW(mon, &mi);
 const int W = S((float)logical_w), H = S((float)logical_h);
 const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - W) / 2;
        const int y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - H) / 2;
        hwnd_ = CreateWindowExW((topmost ? WS_EX_TOPMOST : 0) | WS_EX_TOOLWINDOW, kCls,
    title_.c_str(), WS_POPUP | WS_CLIPCHILDREN, x, y, W, H, owner,
    nullptr, hi, this);
        if (!hwnd_) return false;
        // Win11：圆角 + 发丝色边框（旧系统上静默失败，保持直角）
        const int corner = 2;  // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof(corner));
        const COLORREF edge = kHair;
        DwmSetWindowAttribute(hwnd_, 34, &edge, sizeof(edge));

 f_title_ = makeFont(S(16), FW_SEMIBOLD);
        f_text_ = makeFont(S(14), FW_NORMAL);
 f_small_ = makeFont(S(13), FW_NORMAL);
        f_btn_ = makeFont(S(14), FW_SEMIBOLD);
        field_brush_ = CreateSolidBrush(kField);

 RECT rc;
        GetClientRect(hwnd_, &rc);
        addButton(L"", IDCANCEL, Btn::Close, rc.right - S(44), S(10), S(32), S(32));
        return true;
    }

    void show(bool take_focus = true) {
        ShowWindow(hwnd_, take_focus ? SW_SHOW : SW_SHOWNOACTIVATE);
        UpdateWindow(hwnd_);
 if (take_focus) {
     // owner（桌宠窗口）是 WS_EX_NOACTIVATE，弹窗不会自动成为前台
     SetForegroundWindow(hwnd_);
     SetActiveWindow(hwnd_);
 }
    }

    void runModal() {
        MSG m;
        while (!done_ && GetMessageW(&m, nullptr, 0, 0) > 0) {
     if (!IsDialogMessageW(hwnd_, &m)) {
  TranslateMessage(&m);
         DispatchMessageW(&m);
     }
 }
    }
    // 非阻塞：派发本线程积压的消息（进度窗口在轮询里周期调用）
    void pumpOnce() {
 MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

    void destroyWindow() {
 if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
 fields_.clear();
        for (HFONT* f : {&f_title_, &f_text_, &f_small_, &f_btn_}) {
     if (*f) DeleteObject(*f);
     *f = nullptr;
        }
        if (field_brush_) DeleteObject(field_brush_);
        field_brush_ = nullptr;
        done_ = false;
    }

    // 调整客户区高度并在所在显示器居中
    void setClientHeight(int logical_h) {
        RECT rc;
        GetWindowRect(hwnd_, &rc);
        const int W = rc.right - rc.left, H = S((float)logical_h);
        MONITORINFO mi;
        ZeroMemory(&mi, sizeof(mi));
        mi.cbSize = sizeof(mi);
 GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowPos(hwnd_, nullptr,
       mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - W) / 2,
       mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - H) / 2, W, H,
       SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // 删掉除关闭按钮外的全部子控件（切换页面时重建）
    void clearControls() {
        HWND child = GetWindow(hwnd_, GW_CHILD);
 while (child) {
     const HWND next = GetWindow(child, GW_HWNDNEXT);
     if (GetDlgCtrlID(child) != IDCANCEL ||
  (Btn)GetWindowLongPtrW(child, GWLP_USERDATA) != Btn::Close)
  DestroyWindow(child);
     child = next;
        }
        fields_.clear();
    }

    HWND addButton(const wchar_t* text, int id, Btn style, int x, int y, int w, int h) {
        HWND b = CreateWindowExW(0, L"BUTTON", text,
     WS_CHILD | WS_VISIBLE | BS_OWNERDRAW |
  (style == Btn::Close ? 0 : WS_TABSTOP),
     x, y, w, h, hwnd_, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr),
     nullptr);
        SetWindowLongPtrW(b, GWLP_USERDATA, (LONG_PTR)style);
        SetWindowSubclass(b, &Dialog::buttonProc, 1, 0);
 return b;
    }

    // 输入框：frame 是自绘的圆角底框，EDIT 无边框嵌在里面
    HWND addEdit(int id, const std::wstring& initial, bool password, int limit, RECT frame,
   HFONT font = nullptr, DWORD extra_style = 0, int inset_logical = 12) {
        HFONT f = font ? font : f_text_;
        HDC dc = GetDC(hwnd_);
 HGDIOBJ old = SelectObject(dc, f);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, old);
        ReleaseDC(hwnd_, dc);
        const int eh = tm.tmHeight + S(2), inset = S((float)inset_logical);
        HWND e = CreateWindowExW(
            0, L"EDIT", initial.c_str(),
     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra_style |
  (password ? ES_PASSWORD : 0),
     frame.left + inset, frame.top + ((frame.bottom - frame.top) - eh) / 2,
     (frame.right - frame.left) - inset * 2, eh, hwnd_, (HMENU)(INT_PTR)id,
     GetModuleHandleW(nullptr), nullptr);
 SendMessageW(e, WM_SETFONT, (WPARAM)f, TRUE);
        if (limit > 0) SendMessageW(e, EM_LIMITTEXT, (WPARAM)limit, 0);
        Field fd;
 fd.edit = e;
 fd.frame = frame;
 fields_.push_back(fd);
        return e;
    }
    void setFieldAlert(HWND edit, bool alert) {
        for (auto& f : fields_)
     if (f.edit == edit) f.alert = alert;
 InvalidateRect(hwnd_, nullptr, FALSE);
    }

    static std::wstring textOf(HWND ctrl) {
        const int n = GetWindowTextLengthW(ctrl);
 std::wstring s((size_t)n + 1, L'\0');
        GetWindowTextW(ctrl, &s[0], n + 1);
        s.resize((size_t)n);
        return s;
    }

    HFONT f_title_ = nullptr, f_text_ = nullptr, f_small_ = nullptr, f_btn_ = nullptr;

private:
    struct Field {
        HWND edit = nullptr;
 RECT frame = RECT();
        bool alert = false;
    };

    void paint() {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd_, &ps);
        RECT rc;
 GetClientRect(hwnd_, &rc);
 HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
        HGDIOBJ old_bmp = SelectObject(dc, bmp);
        HBRUSH bg = CreateSolidBrush(kStage);
        FillRect(dc, &rc, bg);
 DeleteObject(bg);
 {
     Gdiplus::Graphics g(dc);
     g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
     g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
     // 标题行：状态灯 + 标题
     const float cy = S(26), cx = (float)S(kPad) + S(7);
     const COLORREF lamp = tone_ == Tone::Alert ? kCoral : kInk;
     ring(g, cx, cy, (float)S(9), (float)S(1.5f), lamp, 90);
     circle(g, cx, cy, (float)S(5), lamp);
     // 输入框底框（聚焦 = 主题色边，出错 = 珊瑚色边）
     const HWND focus = GetFocus();
     for (const auto& f : fields_) {
         const COLORREF edge = f.alert ? kCoral : f.edit == focus ? kInk : kHair;
  roundRect(g, f.frame, (float)S(10), kField, kField, true, edge,
     f.edit == focus || f.alert ? (float)S(1.5f) : 1.0f);
     }
     paintContent(dc, g);
        }
 RECT tr = {S(kPad) + S(24), 0, rc.right - S(52), S(kCaption)};
        drawText(dc, title_, tr, f_title_, kPaper);
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, old_bmp);
        DeleteObject(bmp);
        DeleteDC(dc);
 EndPaint(hwnd_, &ps);
    }

    void drawButton(const DRAWITEMSTRUCT* d) {
        const Btn style = (Btn)GetWindowLongPtrW(d->hwndItem, GWLP_USERDATA);
        const bool down = (d->itemState & ODS_SELECTED) != 0;
        const bool hot = GetPropW(d->hwndItem, L"dk_hot") != nullptr;
        const bool off = (d->itemState & ODS_DISABLED) != 0;
 RECT r = d->rcItem;
        HDC dc = d->hDC;
 HBRUSH bg = CreateSolidBrush(kStage);
        FillRect(dc, &r, bg);
 DeleteObject(bg);
 Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        const float radius = (float)(r.bottom - r.top) * 0.5f;
        COLORREF text = kPaper;
        if (style == Btn::Close) {
     if (hot || down) roundRect(g, r, (float)S(8), kTileTop, kTileTop, false);
     Gdiplus::Pen pen(gp(hot ? kPaper : kDim), (float)S(1.6f));
     pen.SetStartCap(Gdiplus::LineCapRound);
     pen.SetEndCap(Gdiplus::LineCapRound);
     const float cx = (r.left + r.right) * 0.5f, cy = (r.top + r.bottom) * 0.5f;
     const float k = (float)S(5);
     g.DrawLine(&pen, cx - k, cy - k, cx + k, cy + k);
     g.DrawLine(&pen, cx - k, cy + k, cx + k, cy - k);
     return;
        }
        if (style == Btn::Primary) {
     const COLORREF c = down ? kInkDown : kInk;
     roundRect(g, r, radius, c, c, false);
     text = kInkDark;
        } else if (style == Btn::Danger) {
     const COLORREF c = down ? kCoralDown : kCoral;
     roundRect(g, r, radius, c, c, false);
     text = kCoralInk;
        } else {
     roundRect(g, r, radius, down ? kTileBot : kTileTop, kTileBot, true,
       hot ? RGB(0x55, 0x55, 0x5E) : kHair);
        }
 wchar_t label[96];
 label[0] = 0;
        GetWindowTextW(d->hwndItem, label, 96);
        drawText(dc, label, r, f_btn_, off ? kFaint : text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
 if (d->itemState & ODS_FOCUS) {  // 键盘焦点：外圈一道主题色
            RECT fr = r;
     InflateRect(&fr, -S(3), -S(3));
     Gdiplus::Pen pen(gp(style == Btn::Secondary ? kInk : kStage, 200), (float)S(1.5f));
            const float x = (float)fr.left, y = (float)fr.top;
     const float w = (float)(fr.right - fr.left) - 1, h = (float)(fr.bottom - fr.top) - 1;
     Gdiplus::GraphicsPath path;
     path.AddArc(x, y, h, h, 90, 180);
     path.AddArc(x + w - h, y, h, h, 270, 180);
     path.CloseFigure();
     g.DrawPath(&pen, &path);
        }
    }

    // 按钮悬停状态（owner-draw 按钮自己不报悬停）
    static LRESULT CALLBACK buttonProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
        if (m == WM_MOUSEMOVE && !GetPropW(h, L"dk_hot")) {
     SetPropW(h, L"dk_hot", (HANDLE)1);
     TRACKMOUSEEVENT t;
     t.cbSize = sizeof(t);
     t.dwFlags = TME_LEAVE;
            t.hwndTrack = h;
     t.dwHoverTime = 0;
     TrackMouseEvent(&t);
     InvalidateRect(h, nullptr, FALSE);
        } else if (m == WM_MOUSELEAVE) {
     RemovePropW(h, L"dk_hot");
     InvalidateRect(h, nullptr, FALSE);
        } else if (m == WM_NCDESTROY) {
     RemovePropW(h, L"dk_hot");
        }
 return DefSubclassProc(h, m, w, l);
    }

    static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Dialog* self = nullptr;
        if (m == WM_NCCREATE) {
     auto* cs = (LPCREATESTRUCTW)l;
     self = (Dialog*)cs->lpCreateParams;
            SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
     self->hwnd_ = h;
 } else {
     self = (Dialog*)GetWindowLongPtrW(h, GWLP_USERDATA);
 }
        if (!self) return DefWindowProcW(h, m, w, l);
        switch (m) {
     case WM_ERASEBKGND:
         return 1;
     case WM_PAINT:
  self->paint();
  return 0;
     case WM_NCHITTEST: {  // 标题行可拖动
  POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
  ScreenToClient(h, &pt);
  if (pt.y < self->S(kCaption)) return HTCAPTION;
  break;
     }
     case WM_LBUTTONDOWN: {
  POINT pt = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
  self->onClick(pt);
         break;
     }
     case WM_CTLCOLOREDIT: {
  HDC dc = (HDC)w;
  SetTextColor(dc, self->editColor((HWND)l));
  SetBkColor(dc, kField);
  return (LRESULT)self->field_brush_;
     }
     case WM_DRAWITEM:
  self->drawButton((const DRAWITEMSTRUCT*)l);
  return TRUE;
     case WM_TIMER:
  self->onTimer((UINT_PTR)w);
         return 0;
     case WM_COMMAND: {
  int id = LOWORD(w);
  const int code = HIWORD(w);
  if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
      InvalidateRect(h, nullptr, FALSE);  // 焦点边框跟着走
  }
  // IsDialogMessage 把 Enter 变成 IDOK：转给默认按钮
  if (id == IDOK && code == BN_CLICKED && l == 0 && self->default_id_)
      id = self->default_id_;
  self->onCommand(id, code, (HWND)l);
         return 0;
     }
     case WM_CLOSE:
  self->onCommand(IDCANCEL, BN_CLICKED, nullptr);
  return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }

    HWND hwnd_ = nullptr;
    std::wstring title_;
    std::vector<Field> fields_;
    HBRUSH field_brush_ = nullptr;
    Tone tone_ = Tone::Info;
    float s_ = 1.0f;
    int default_id_ = 0;
    bool done_ = false;
};

// ---------------------------------------------------------------------------
// 提示框：代替系统 MessageBox。flags 用 MessageBox 的同名常量：
//   MB_OK / MB_YESNO；MB_ICONWARNING、MB_ICONERROR 用珊瑚色状态灯。
// 返回 IDOK / IDYES / IDNO（关闭 = IDNO 或 IDOK，与系统行为一致）。
// ---------------------------------------------------------------------------
class MessageDialog : public Dialog {
public:
    int run(HWND owner, const std::wstring& text, const wchar_t* title, UINT flags) {
        text_ = text;
        yes_no_ = (flags & MB_TYPEMASK) == MB_YESNO;
        const bool alert = (flags & MB_ICONMASK) == MB_ICONWARNING ||
      (flags & MB_ICONMASK) == MB_ICONERROR;
 result_ = yes_no_ ? IDNO : IDOK;
 if (!createWindow(owner, title && *title ? title : L"Duty On", 400, 200)) return result_;
        setTone(alert ? Tone::Alert : Tone::Info);
 RECT rc;
        GetClientRect(hwnd(), &rc);
        HDC dc = GetDC(hwnd());
        text_h_ = textHeight(dc, text_, rc.right - S(kPad) * 2, f_text_);
        ReleaseDC(hwnd(), dc);
 const int H = kCaption + 4 + unS(text_h_) + 22 + 40 + 24;
        setClientHeight(H);
        GetClientRect(hwnd(), &rc);
 const int bw = S(104), bh = S(40), by = rc.bottom - S(24) - bh;
        if (yes_no_) {
     addButton(L"不用了", IDNO, Btn::Secondary, rc.right - S(kPad) - bw * 2 - S(10), by,
 bw, bh);
     HWND yes = addButton(L"确定", IDYES, alert ? Btn::Danger : Btn::Primary,
     rc.right - S(kPad) - bw, by, bw, bh);
     setDefault(IDYES);
     show();
     SetFocus(yes);
        } else {
     HWND ok = addButton(L"知道了", IDOK, Btn::Primary, rc.right - S(kPad) - bw, by, bw, bh);
     setDefault(IDOK);
     show();
     SetFocus(ok);
        }
 runModal();
 destroyWindow();
        return result_;
    }

protected:
    void paintContent(HDC dc, Gdiplus::Graphics&) override {
 RECT rc;
        GetClientRect(hwnd(), &rc);
 RECT tr = {S(kPad), S(kCaption) + S(4), rc.right - S(kPad), S(kCaption) + S(4) + text_h_};
        drawText(dc, text_, tr, f_text_, kPaper, DT_LEFT | DT_WORDBREAK);
    }
    void onCommand(int id, int code, HWND) override {
        if (code != BN_CLICKED) return;
        if (id == IDCANCEL) {
     result_ = yes_no_ ? IDNO : IDOK;
     close();
        } else if (id == IDOK || id == IDYES || id == IDNO) {
     result_ = id;
     close();
        }
    }

private:
    std::wstring text_;
    int text_h_ = 0;
    int result_ = IDOK;
    bool yes_no_ = false;
};

inline int messageBox(HWND owner, const std::wstring& text, const wchar_t* title, UINT flags) {
    MessageDialog dlg;
    return dlg.run(owner, text, title, flags);
}

}  // namespace dk
}  // namespace dutyon

#endif  // _WIN32
