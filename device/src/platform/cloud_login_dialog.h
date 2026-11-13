#pragma once
// ---------------------------------------------------------------------------
// 云端账户弹窗（仅 Windows PC 端）。外观来自 dialog_kit.h。
//
// 布局：标题行 + 一句说明 + 三个输入框（服务器 / 用户名 / 密码，标签在框上方）
//       + 状态行（出错珊瑚色，进行中次要色）+ [创建账号] [登录]。
// 交互：Enter = 登录，Esc / 关闭 = 取消，Tab 切换焦点。登录和注册在 UI 线程
//       同步执行 HTTP（期间状态行显示"正在连接…"，主循环停帧可接受）。
//
// Host.auth(action, server, user, pass)：action = "login" / "register"；
// 返回空串 = 成功（弹窗关闭），否则为 UTF-8 错误文本（显示在状态行）。
// ---------------------------------------------------------------------------
#ifdef _WIN32

#include <functional>
#include <string>

#include "platform/dialog_kit.h"

namespace dutyon {

class CloudLoginDialog : public dk::Dialog {
public:
    struct Host {
        // action: "login" / "register"；空返回 = 成功
        std::function<std::string(const std::string& action, const std::string& server,
      const std::string& user, const std::string& pass)>
     auth;
    };

    // 模态运行；default_server 预填服务器地址（上次成功登录的值）
    void run(HWND owner, const Host& host, const std::string& default_server = "") {
        host_ = &host;
 if (!createWindow(owner, L"云端账户", kWidth, kHeight)) return;
 build(dk::widen(default_server));
        show();
        // 服务器地址已预填时，直接从用户名开始输入
        SetFocus(default_server.empty() ? ed_server_ : ed_user_);
 runModal();
 destroyWindow();
    }

private:
    static constexpr int kWidth = 420;
    static constexpr int kHeight = 452;
    static constexpr int kEdServer = 101, kEdUser = 102, kEdPass = 103;
    static constexpr int kBtnLogin = 201, kBtnRegister = 202;
    static constexpr int kLede = 52;      // 说明文字顶边
    static constexpr int kFirstRow = 112; // 第一个输入框标签顶边
    static constexpr int kRowPitch = 78;  // 标签 + 输入框 + 间隔
    static constexpr int kFieldH = 42;

    void build(const std::wstring& server) {
 RECT rc;
        GetClientRect(hwnd(), &rc);
 const int x0 = S(kPad), x1 = rc.right - S(kPad);
        const int ids[3] = {kEdServer, kEdUser, kEdPass};
        HWND* slots[3] = {&ed_server_, &ed_user_, &ed_pass_};
        for (int i = 0; i < 3; ++i) {
     const int top = S((float)(kFirstRow + i * kRowPitch + 24));
     RECT frame = {x0, top, x1, top + S(kFieldH)};
     *slots[i] = addEdit(ids[i], i == 0 ? server : std::wstring(), i == 2,
    i == 2 ? 128 : i == 0 ? 160 : 64, frame);
        }
        const int bh = S(42), by = rc.bottom - S(24) - bh;
 const int bw = (x1 - x0 - S(12)) / 2;
        addButton(L"创建账号", kBtnRegister, dk::Btn::Secondary, x0, by, bw, bh);
        addButton(L"登录", kBtnLogin, dk::Btn::Primary, x1 - bw, by, bw, bh);
        setDefault(kBtnLogin);
    }

    void paintContent(HDC dc, Gdiplus::Graphics&) override {
        RECT rc;
 GetClientRect(hwnd(), &rc);
 const int x0 = S(kPad), x1 = rc.right - S(kPad);
        RECT lede = {x0, S(kLede), x1, S(kLede) + S(48)};
        dk::drawText(dc, L"登录后每分钟自动备份你的角色、声音和设置，换一台电脑可以从云端恢复。",
       lede, f_small_, dk::kDim, DT_LEFT | DT_WORDBREAK);
        static const wchar_t* kLabels[3] = {L"服务器", L"用户名", L"密码"};
        for (int i = 0; i < 3; ++i) {
     const int top = S((float)(kFirstRow + i * kRowPitch));
     RECT lr = {x0 + S(2), top, x1, top + S(20)};
     dk::drawText(dc, kLabels[i], lr, f_small_, dk::kDim);
        }
        if (!status_.empty()) {
     const int top = S((float)(kFirstRow + 3 * kRowPitch - 6));
     RECT sr = {x0 + S(2), top, x1, top + S(38)};
     dk::drawText(dc, status_, sr, f_small_, status_error_ ? dk::kCoral : dk::kDim,
    DT_LEFT | DT_WORDBREAK);
 }
    }

    void setStatus(const std::wstring& text, bool error) {
        status_ = text;
 status_error_ = error;
        setTone(error ? dk::Tone::Alert : dk::Tone::Info);
 repaint();  // 同步重绘：接下来的网络请求会阻塞 UI 线程
    }

    void onAuth(bool register_mode) {
        if (!host_ || !host_->auth) return;
 const std::string server = dk::narrow(textOf(ed_server_));
 const std::string user = dk::narrow(textOf(ed_user_));
 const std::string pass = dk::narrow(textOf(ed_pass_));
 setFieldAlert(ed_user_, user.empty());
 setFieldAlert(ed_pass_, pass.empty());
        if (user.empty() || pass.empty()) {
     setStatus(L"请填写用户名和密码。", true);
     SetFocus(user.empty() ? ed_user_ : ed_pass_);
     return;
        }
 setStatus(register_mode ? L"正在创建账号…" : L"正在连接…", false);
 const std::string err = host_->auth(register_mode ? "register" : "login", server, user,
         pass);
        if (err.empty()) {
     close();  // 成功：关闭弹窗（main 侧随后触发恢复）
     return;
        }
        setStatus(dk::widen(err), true);
        SetFocus(ed_user_);
    }

    void onCommand(int id, int code, HWND) override {
        if (code == EN_CHANGE) {  // 开始改就撤掉红框
     setFieldAlert(GetDlgItem(hwnd(), id), false);
     return;
        }
        if (code != BN_CLICKED) return;
 if (id == kBtnLogin) onAuth(false);
        else if (id == kBtnRegister) onAuth(true);
 else if (id == IDCANCEL) close();
    }

    HWND ed_server_ = nullptr, ed_user_ = nullptr, ed_pass_ = nullptr;
    const Host* host_ = nullptr;
    std::wstring status_;
    bool status_error_ = false;
};

}  // namespace dutyon

#endif  // _WIN32
