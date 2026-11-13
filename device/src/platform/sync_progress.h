#pragma once
// ---------------------------------------------------------------------------
// 设备程序更新进度窗口（仅 Windows PC 端）。外观来自 dialog_kit.h。
// 被 main.cpp 的自动更新工作线程使用，独立于桌宠窗口，置顶显示，不抢焦点。
//
// 线程约定：create / update / pump / destroy 必须在同一个工作线程调用（窗口
// 归属该线程，pump() 自持非阻塞消息泵）。
//
// 用法：
//   SyncProgressDialog dlg;
//   dlg.create();
//   while (proc_running) { dlg.pump(); dlg.update(pct, L"阶段名"); }
//   dlg.update(100, L"更新完成"); Sleep(2000); dlg.destroy();
// ---------------------------------------------------------------------------
#ifdef _WIN32

#include <string>

#include "platform/dialog_kit.h"

namespace dutyon {

class SyncProgressDialog : public dk::Dialog {
public:
    ~SyncProgressDialog() override { destroy(); }

    bool create(const wchar_t* title = L"设备程序更新") {
        if (hwnd()) return true;
        if (!createWindow(nullptr, title, 400, 156, /*topmost=*/true)) return false;
        // 进度窗口不能关（更新在后台进行）：收起关闭按钮
 if (HWND x = GetDlgItem(hwnd(), IDCANCEL)) ShowWindow(x, SW_HIDE);
 stage_ = L"准备中…";
 show(/*take_focus=*/false);
        return true;
    }

    // 更新进度（0-100）与阶段文字（stage 为空表示只改进度）
    void update(int pct, const wchar_t* stage) {
        if (!hwnd()) return;
 pct_ = pct < 0 ? 0 : pct > 100 ? 100 : pct;
        if (stage) stage_ = stage;
 repaint();
    }

    void pump() { pumpOnce(); }
    void destroy() { destroyWindow(); }

protected:
    void paintContent(HDC dc, Gdiplus::Graphics& g) override {
        RECT rc;
 GetClientRect(hwnd(), &rc);
        const int x0 = S(kPad), x1 = rc.right - S(kPad);
        // 阶段文字在左，百分比在右
        RECT sr = {x0, S(60), x1 - S(56), S(60) + S(24)};
        dk::drawText(dc, stage_, sr, f_text_, dk::kPaper, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
       DT_END_ELLIPSIS);
        RECT pr = {x1 - S(56), S(60), x1, S(60) + S(24)};
        dk::drawText(dc, std::to_wstring(pct_) + L"%", pr, f_text_, dk::kInk,
       DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        // 进度条：深色槽 + 主题色
 RECT track = {x0, S(98), x1, S(98) + S(8)};
        dk::roundRect(g, track, (float)S(4), dk::kTileTop, dk::kTileTop, false);
        const int fill_w = (x1 - x0) * pct_ / 100;
        if (fill_w > S(8)) {
     RECT fill = {x0, track.top, x0 + fill_w, track.bottom};
     dk::roundRect(g, fill, (float)S(4), dk::kInk, dk::kInk, false);
        }
        RECT nr = {x0, S(116), x1, S(116) + S(20)};
 dk::drawText(dc, L"更新期间硬件屏会短暂重启，请不要断电。", nr, f_small_, dk::kFaint);
    }

private:
    std::wstring stage_;
    int pct_ = 0;
};

}  // namespace dutyon

#endif  // _WIN32
