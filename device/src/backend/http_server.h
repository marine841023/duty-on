#pragma once

// ---------------------------------------------------------------------------
// 内嵌 HTTP 服务器 —— src-tauri/src/server.rs 的 C++ 移植（cpp-httplib）。
//
// 单进程化后的角色：继续监听 127.0.0.1:17521，接收 IDE 桥接脚本 POST 的
// hook 事件、给硬件显示端提供只读 API（/api/status /api/events SSE
// /api/metrics），并承接宠物客户端菜单动作（安装 hooks / 前置窗口 /
// 自启动 / 退出 —— 本机直连后这些端点退化为兼容层，但已装的 IDE hook
// 脚本无需任何改动）。
//
// 端点分层（同 Rust 版）：
//   internal  /hook /unregister /log /status /live2d/* —— 写端点仅限回环
//   external  /api/status /api/events /api/metrics /api/sounds/:state /health
//             —— 任意来源只读（硬件显示面）
//   client    /api/hooks /api/hooks/install /api/bring-to-front
//             /api/autostart /api/quit —— POST，仅限回环
// ---------------------------------------------------------------------------

#include <functional>
#include <string>

#include "backend/state_manager.h"
#include "backend/sys_monitor.h"

namespace httplib {
class Server;
}

namespace dutyon::backend {

class HttpServer {
public:
    HttpServer(StateManager& sm, SysMonitor& monitor);
    ~HttpServer();

    // 启动监听线程。端口被占（AddrInUse）= 已有实例在跑，返回 false。
    bool start();
    void stop();

    bool isRunning() const { return running_; }

    // /api/quit 触发完整退出（main 注入：关窗口/停主循环/结束进程）
    void setQuitHandler(std::function<void()> fn) { quit_handler_ = std::move(fn); }

private:
    void registerRoutes();

    StateManager& sm_;
    SysMonitor& monitor_;
    std::function<void()> quit_handler_;
    httplib::Server* svr_ = nullptr;
    bool running_ = false;
};

} // namespace dutyon::backend
