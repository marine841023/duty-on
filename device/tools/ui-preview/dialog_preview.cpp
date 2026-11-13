// 弹窗外观预览：把各个弹窗的各种状态逐个显示、截图成 PNG，然后自动关闭。
// 改弹窗时用它看效果，不必在桌宠里一个个点出来。
//
// 构建与运行：device\tools\ui-preview\dialog-preview.cmd <输出目录>
#include <windows.h>

#include <string>
#include <thread>
#include <vector>

#include "platform/cloud_login_dialog.h"
#include "platform/dialog_kit.h"
#include "platform/pair_code_dialog.h"
#include "platform/sync_progress.h"

using namespace dutyon;

static std::wstring g_out;
static void noop(HWND) { return; }

static HWND findDialog() {
    return FindWindowW(L"DutyOnDialogKit", nullptr);
}
static void savePng(HWND h, const std::wstring& name) {
    RECT rc;
    GetWindowRect(h, &rc);
    const int w = rc.right - rc.left, hh = rc.bottom - rc.top;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, hh);
    HGDIOBJ old = SelectObject(dc, bmp);
    PrintWindow(h, dc, 2);
    SelectObject(dc, old);
    Gdiplus::Bitmap image(bmp, nullptr);
    CLSID png;
    CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);
    image.Save((g_out + L"\\" + name + L".png").c_str(), &png, nullptr);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}
// 在另一个线程里：等弹窗出现 → 做点操作 → 截图 → 关闭
template <typename Fn>
static std::thread driver(const std::wstring& name, Fn act) {
    return std::thread([name, act] {
 HWND h = nullptr;
        for (int i = 0; i < 100 && !h; ++i) { Sleep(50); h = findDialog(); }
 if (!h) return;
 Sleep(350);
        act(h);
 Sleep(450);
 savePng(h, name);
        PostMessageW(h, WM_CLOSE, 0, 0);
    });
}
static void typeInto(HWND dlg, int id, const wchar_t* text) {
    HWND e = GetDlgItem(dlg, id);
    for (const wchar_t* p = text; *p; ++p) SendMessageW(e, WM_CHAR, *p, 0);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR cmd, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_out = (cmd && *cmd) ? cmd : L".";

    {  // 云端账户：初始
        CloudLoginDialog::Host host;
 host.auth = [](const std::string&, const std::string&, const std::string&,
  const std::string&) { return std::string("用户名或密码不对"); };
        auto t = driver(L"cloud-empty", &noop);
        CloudLoginDialog().run(nullptr, host, "https://dutyon.megrezsoft.com");
        t.join();
 // 云端账户：登录失败
 auto t2 = driver(L"cloud-error", [](HWND h) {
     typeInto(h, 102, L"marine84");
     typeInto(h, 103, L"wrong-password");
     SendMessageW(h, WM_COMMAND, MAKEWPARAM(201, BN_CLICKED), 0);
        });
 CloudLoginDialog().run(nullptr, host, "https://dutyon.megrezsoft.com");
        t2.join();
    }
    {  // 配对：输码中 / 配对码不对 / 已配对 / 确认清除
 PairDialog::Host host;
        host.confirm = [](const std::string&) { return false; };
 host.paired = [] { return std::vector<PairDialog::Device>(); };
        auto t = driver(L"pair-input", [](HWND h) {
     for (int i = 0; i < 4; ++i) SendMessageW(GetDlgItem(h, 100 + i), WM_CHAR, L'4' + i, 0);
        });
        PairDialog().run(nullptr, host);
 t.join();
        auto t2 = driver(L"pair-wrong", [](HWND h) {
     for (int i = 0; i < 6; ++i) SendMessageW(GetDlgItem(h, 100 + i), WM_CHAR, L'1' + i, 0);
     SendMessageW(h, WM_COMMAND, MAKEWPARAM(201, BN_CLICKED), 0);
 });
 PairDialog().run(nullptr, host);
 t2.join();
 host.paired = [] {
     return std::vector<PairDialog::Device>{
  {"dev-1", "Duty On 硬件屏 · 书房"},
  {"dev-2", "名字很长的硬件屏用来检查省略号是否正常工作不会把按钮挤走"}};
        };
 auto t3 = driver(L"pair-paired", [](HWND h) {
     SendMessageW(h, WM_COMMAND, MAKEWPARAM(301, BN_CLICKED), (LPARAM)GetDlgItem(h, 301));
        });
 PairDialog().run(nullptr, host);
 t3.join();
    }
    {  // 提示框：普通 / 确认删除
        auto t = driver(L"msg-info", &noop);
        dk::messageBox(nullptr, L"IDE 集成安装完成。重启正在运行的 IDE，或者开一个新的 AI 会话后生效。",
  L"Duty On", MB_OK | MB_ICONINFORMATION);
        t.join();
 auto t2 = driver(L"msg-confirm", &noop);
 dk::messageBox(nullptr, L"确定删除角色「庄方宜」吗？\r\n对应的动画文件会一并删除。", L"删除角色",
  MB_YESNO | MB_ICONWARNING);
 t2.join();
    }
    {  // 设备程序更新进度
 SyncProgressDialog dlg;
        dlg.create();
 dlg.update(62, L"正在上传到硬件屏");
        for (int i = 0; i < 12; ++i) { dlg.pump(); Sleep(50); }
 savePng(findDialog(), L"sync-progress");
 dlg.destroy();
    }
    return 0;
}
