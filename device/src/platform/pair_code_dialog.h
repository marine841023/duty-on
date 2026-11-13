#pragma once
// ---------------------------------------------------------------------------
// 配对设备弹窗（仅 Windows PC 端）。外观来自 dialog_kit.h。两个页面：
//   输码：六格配对码（三位一组，和硬件屏上的排法一致）+ [取消] [配对]
//   已配对：每台设备一张牌 + [解除配对] [清除 Wi-Fi] + [完成]
//
// 输码交互（对齐常见验证码控件）：数字填入当前格自动跳下一格（重输覆盖）；
// Backspace 在空格上回退上一格；←/→ 移动；Enter = 配对；Esc = 取消；粘贴
// 一串数字会依次填满后面的格。没填满或配对码不对时，状态行用珊瑚色说明，
// 不再另弹提示框。
//
// 解除配对、清除 Wi-Fi 都要点两次：第一次按钮变成珊瑚色的"确认…"，三秒内
// 再点才执行（清除 Wi-Fi 会让硬件屏回到配网热点，误点代价大）。
//
// 动作与数据经 Host 回调注入（main.cpp 用 lambda 组装）。仅 UI 线程，run()
// 自持模态消息泵。
// ---------------------------------------------------------------------------
#ifdef _WIN32

#include <functional>
#include <string>
#include <vector>

#include "platform/dialog_kit.h"

namespace dutyon {

class PairDialog : public dk::Dialog {
public:
    struct Device {
        std::string id, label;
    };

    // 宿主动作集（全部可选；为空时对应按钮不生效）
    struct Host {
 std::function<bool(const std::string&)> confirm;     // 输码 → 是否成功
        std::function<void(const std::string&)> unpair;      // 解除配对
        std::function<void(const std::string&)> reset_wifi;  // 清除设备 Wi-Fi
        std::function<std::vector<Device>()> paired;  // 当前已配对列表
    };

    // 模态运行：点「配对设备」直接调用；关闭窗口才返回
    void run(HWND owner, const Host& host) {
        host_ = &host;
 devices_ = host.paired ? host.paired() : std::vector<Device>();
        input_mode_ = devices_.empty();
        if (!createWindow(owner, L"配对设备", kWidth, 300)) return;
 f_digit_ = dk::makeFont(S(34), FW_BOLD, L"Arial Rounded MT Bold");
 build();
        show();
 if (input_mode_ && edits_[0]) SetFocus(edits_[0]);
        runModal();
 KillTimer(hwnd(), kDisarmTimer);
        if (f_digit_) DeleteObject(f_digit_);
        f_digit_ = nullptr;
        destroyWindow();
    }

private:
    static constexpr int kWidth = 420;
    static constexpr int kBoxes = 6;
    static constexpr int kEditIdBase = 100;     // 输入格 ID: 100~105
    static constexpr int kBtnDeviceBase = 300;  // 设备行按钮 ID: 300+2i 解除 / 301+2i 清除 Wi-Fi
    static constexpr int kBtnPair = 201;
    static constexpr UINT_PTR kDisarmTimer = 1;
    static constexpr int kRowPitch = 112;  // 已配对页每台设备占的高度

    struct BoxCtx {  // 输入格子类过程的引用数据（须活到控件销毁）
        PairDialog* dlg;
 int idx;
    };

    void build() {
        clearControls();
        for (int i = 0; i < kBoxes; ++i) edits_[i] = nullptr;
        armed_id_ = 0;
 status_.clear();
        setTone(dk::Tone::Info);
        if (input_mode_) buildInput(); else buildPaired();
 repaint();
    }

    void buildInput() {
        setClientHeight(296);
        RECT rc;
 GetClientRect(hwnd(), &rc);
 const int cw = S(48), ch = S(60), gap = S(8), group = S(20);
 const int total = cw * kBoxes + gap * 4 + group;
        int x = (rc.right - total) / 2;
        const int top = S(104);
 for (int i = 0; i < kBoxes; ++i) {
     RECT cell = {x, top, x + cw, top + ch};
     edits_[i] = addEdit(kEditIdBase + i, L"", false, 1, cell, f_digit_, ES_CENTER, 2);
     ctx_[i].dlg = this;
     ctx_[i].idx = i;
     SetWindowSubclass(edits_[i], &PairDialog::editProc, 2, (DWORD_PTR)&ctx_[i]);
     x += cw + (i == 2 ? group : gap);
        }
 const int bh = S(42), by = rc.bottom - S(24) - bh, bw = S(116);
        addButton(L"取消", IDCANCEL, dk::Btn::Secondary, rc.right - S(kPad) - bw * 2 - S(12), by,
    bw, bh);
        addButton(L"配对", kBtnPair, dk::Btn::Primary, rc.right - S(kPad) - bw, by, bw, bh);
 setDefault(kBtnPair);
        SetFocus(edits_[0]);
    }

    void buildPaired() {
 const int n = (int)devices_.size();
 setClientHeight(kCaption + 40 + n * kRowPitch + 30 + 42 + 24);
 RECT rc;
 GetClientRect(hwnd(), &rc);
        const int x0 = S(kPad), x1 = rc.right - S(kPad);
        const int bw = (x1 - x0 - S(10)) / 2, bh = S(38);
 for (int i = 0; i < n; ++i) {
     const int by = S((float)(kCaption + 40 + i * kRowPitch + 56));
     addButton(L"解除配对", kBtnDeviceBase + 2 * i, dk::Btn::Secondary, x0, by, bw, bh);
     addButton(L"清除 Wi-Fi", kBtnDeviceBase + 2 * i + 1, dk::Btn::Secondary, x1 - bw, by, bw,
        bh);
        }
        const int dw = S(116), dh = S(42);
        HWND done = addButton(L"完成", IDCANCEL, dk::Btn::Primary, x1 - dw, rc.bottom - S(24) - dh,
  dw, dh);
        setDefault(IDCANCEL);
 SetFocus(done);
    }

    void paintContent(HDC dc, Gdiplus::Graphics& g) override {
 RECT rc;
        GetClientRect(hwnd(), &rc);
 const int x0 = S(kPad), x1 = rc.right - S(kPad);
 if (input_mode_) {
     RECT lede = {x0, S(56), x1, S(56) + S(24)};
     dk::drawText(dc, L"输入硬件屏上显示的 6 位配对码。", lede, f_text_, dk::kDim);
     if (!status_.empty()) {
  RECT sr = {x0, S(176), x1, S(176) + S(40)};
  dk::drawText(dc, status_, sr, f_small_, dk::kCoral, DT_LEFT | DT_WORDBREAK);
     }
     return;
        }
 RECT lede = {x0, S(56), x1, S(56) + S(24)};
        dk::drawText(dc, L"这些硬件屏已经连上这台电脑。", lede, f_text_, dk::kDim);
 for (int i = 0; i < (int)devices_.size(); ++i) {
     const int top = S((float)(kCaption + 40 + i * kRowPitch));
     RECT tile = {x0, top, x1, top + S(48)};
     dk::roundRect(g, tile, (float)S(14), dk::kTileTop, dk::kTileBot, true);
     // 状态灯：实心点 + 外圈（与设备屏"忙碌"的灯同形，表示连着）
     const float cx = (float)(x0 + S(22)), cy = (float)(top + S(24));
     dk::ring(g, cx, cy, (float)S(8), (float)S(1.4f), dk::kInk, 110);
     dk::circle(g, cx, cy, (float)S(4.5f), dk::kInk);
     const std::string& name = devices_[i].label.empty() ? devices_[i].id : devices_[i].label;
     RECT nr = {x0 + S(42), top, x1 - S(14), top + S(48)};
     dk::drawText(dc, dk::widen(name), nr, f_text_, dk::kPaper,
    DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
 }
        const int note_top = S((float)(kCaption + 40 + (int)devices_.size() * kRowPitch));
        RECT note = {x0, note_top, x1, note_top + S(22)};
 dk::drawText(dc, L"清除 Wi-Fi 后，硬件屏会回到配网热点，需要重新联网。", note, f_small_,
       dk::kFaint);
    }

    COLORREF editColor(HWND) override { return dk::kInk; }

    void setStatus(const wchar_t* text) {
        status_ = text;
 setTone(status_.empty() ? dk::Tone::Info : dk::Tone::Alert);
 repaint();
    }

    void onPair() {
        std::string code;
 int first_empty = -1;
        for (int i = 0; i < kBoxes; ++i) {
     const std::wstring t = textOf(edits_[i]);
     if (t.empty()) { if (first_empty < 0) first_empty = i; }
     else code += (char)t[0];
 }
        if (first_empty >= 0) {
     setStatus(L"配对码是 6 位数字，还没有填完。");
     SetFocus(edits_[first_empty]);
     return;
        }
 const bool ok = host_->confirm ? host_->confirm(code) : false;
        if (!ok) {
     for (int i = 0; i < kBoxes; ++i) SetWindowTextW(edits_[i], L"");
     setStatus(L"配对码不对。请对照硬件屏上的数字再输一次；硬件屏和电脑要在同一个 Wi-Fi 下。");
     SetFocus(edits_[0]);
     return;
        }
        devices_ = host_->paired ? host_->paired() : std::vector<Device>();
        input_mode_ = devices_.empty();
        build();
    }

    // 设备行按钮：第一次点只"上膛"，三秒内再点才执行
    void onDeviceButton(int id, HWND ctrl) {
        const int idx = (id - kBtnDeviceBase) / 2;
 const bool wifi = (id - kBtnDeviceBase) % 2 == 1;
        if (idx < 0 || idx >= (int)devices_.size()) return;
        if (armed_id_ != id) {
     disarm();
     armed_id_ = id;
     SetWindowLongPtrW(ctrl, GWLP_USERDATA, (LONG_PTR)dk::Btn::Danger);
     SetWindowTextW(ctrl, wifi ? L"确认清除" : L"确认解除");
     InvalidateRect(ctrl, nullptr, FALSE);
     SetTimer(hwnd(), kDisarmTimer, 3000, nullptr);
     return;
        }
 KillTimer(hwnd(), kDisarmTimer);
        armed_id_ = 0;
        const std::string dev = devices_[idx].id;
 if (wifi) { if (host_->reset_wifi) host_->reset_wifi(dev); }
 else if (host_->unpair) host_->unpair(dev);
        devices_ = host_->paired ? host_->paired() : std::vector<Device>();
        input_mode_ = devices_.empty();  // 全部解除 → 回到输码页
 build();
    }
    void disarm() {
        if (!armed_id_) return;
 if (HWND b = GetDlgItem(hwnd(), armed_id_)) {
     SetWindowLongPtrW(b, GWLP_USERDATA, (LONG_PTR)dk::Btn::Secondary);
     SetWindowTextW(b, (armed_id_ - kBtnDeviceBase) % 2 == 1 ? L"清除 Wi-Fi" : L"解除配对");
     InvalidateRect(b, nullptr, FALSE);
        }
 armed_id_ = 0;
    }

    void onTimer(UINT_PTR id) override {
        if (id != kDisarmTimer) return;
        KillTimer(hwnd(), kDisarmTimer);
 disarm();
    }

    void onCommand(int id, int code, HWND ctrl) override {
        if (code == EN_CHANGE && id >= kEditIdBase && id < kEditIdBase + kBoxes) {
     // 填入一位 → 跳下一格；开始重输就撤掉错误提示
     if (!status_.empty()) setStatus(L"");
     if (!textOf(edits_[id - kEditIdBase]).empty() && id < kEditIdBase + kBoxes - 1)
  SetFocus(edits_[id - kEditIdBase + 1]);
     return;
 }
        if (code != BN_CLICKED) return;
        if (id == IDCANCEL) close();
        else if (id == kBtnPair && input_mode_) onPair();
 else if (id >= kBtnDeviceBase && !input_mode_) onDeviceButton(id, ctrl);
    }

    // 点格子的边缘（EDIT 之外的部分）也聚焦对应格
    void onClick(POINT pt) override {
        if (!input_mode_) return;
        for (int i = 0; i < kBoxes; ++i) {
     if (!edits_[i]) continue;
     RECT r;
     GetWindowRect(edits_[i], &r);
     MapWindowPoints(nullptr, hwnd(), (LPPOINT)&r, 2);
     InflateRect(&r, S(4), S(12));
     if (PtInRect(&r, pt)) { SetFocus(edits_[i]); break; }
        }
    }

    // ---- 输入格子类过程：只收数字 / 跳格 / 粘贴整串 ----
    static LRESULT CALLBACK editProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR dw) {
        BoxCtx* c = (BoxCtx*)dw;
        PairDialog* d = c->dlg;
 switch (m) {
     case WM_GETDLGCODE:  // 方向键、Tab 由本格处理；Enter/Esc 交回对话框
  return DLGC_WANTCHARS | DLGC_WANTARROWS;
     case WM_CHAR: {
  const wchar_t ch = (wchar_t)w;
  if (ch >= L'0' && ch <= L'9') {
      const wchar_t one[2] = {ch, 0};
      SetWindowTextW(h, one);  // 覆盖式填入；跳格由 EN_CHANGE 处理
      return 0;
  }
  if (ch == VK_BACK) {
      if (d->textOf(h).empty() && c->idx > 0) {  // 空格上退格：回上一格并清空
   SetWindowTextW(d->edits_[c->idx - 1], L"");
          SetFocus(d->edits_[c->idx - 1]);
   return 0;
      }
      break;
  }
  return 0;  // 其余字符不收
     }
     case WM_KEYDOWN:
  if (w == VK_LEFT && c->idx > 0) { SetFocus(d->edits_[c->idx - 1]); return 0; }
  if (w == VK_RIGHT && c->idx < kBoxes - 1) { SetFocus(d->edits_[c->idx + 1]); return 0; }
         break;
     case WM_SETFOCUS:  // 聚焦即全选：直接输入就是覆盖
  PostMessageW(h, EM_SETSEL, 0, -1);
         break;
     case WM_PASTE: {  // 粘贴：剪贴板里的数字从当前格起依次填入
  std::wstring digits;
  if (OpenClipboard(h)) {
      if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
   if (const wchar_t* t = (const wchar_t*)GlobalLock(data)) {
       for (; *t; ++t)
    if (*t >= L'0' && *t <= L'9') digits += *t;
       GlobalUnlock(data);
   }
      }
      CloseClipboard();
  }
  for (size_t k = 0; k < digits.size() && c->idx + (int)k < kBoxes; ++k) {
      const wchar_t one[2] = {digits[k], 0};
      SetWindowTextW(d->edits_[c->idx + (int)k], one);
  }
         return 0;
     }
        }
 return DefSubclassProc(h, m, w, l);
    }

    HWND edits_[kBoxes] = {nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    BoxCtx ctx_[kBoxes];
    HFONT f_digit_ = nullptr;
    const Host* host_ = nullptr;
    std::vector<Device> devices_;
    std::wstring status_;
    int armed_id_ = 0;
    bool input_mode_ = true;
};

}  // namespace dutyon

#endif  // _WIN32
