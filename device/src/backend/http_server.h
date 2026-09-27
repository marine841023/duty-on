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

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "backend/state_manager.h"
#include "backend/sys_monitor.h"

namespace httplib {
class Server;
}

namespace dutyon::backend {

class PairingManager;  // 配对状态（pending/paired + token 门控），BackendService 持有

class HttpServer {
public:
    HttpServer(StateManager& sm, SysMonitor& monitor, PairingManager& pairing);
    ~HttpServer();

    // 启动监听线程。端口被占（AddrInUse）= 已有实例在跑，返回 false。
    bool start();
    void stop();

    bool isRunning() const { return running_; }

    // /api/quit 触发完整退出（main 注入：关窗口/停主循环/结束进程）
    void setQuitHandler(std::function<void()> fn) { quit_handler_ = std::move(fn); }

    // 硬件显示端在线状态：10 秒内有过「带有效 token 的 /api/* 轮询」即视为
    // 在线（Wi-Fi 配对码方案：设备入网配对后 ~2s 轮询一次 /api/status）。
    // 菜单"设备"子页据此显示/隐藏在线设置项
    bool deviceOnline() const {
        return device_last_seen_.load() != 0 &&
               std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                       .count() -
                   device_last_seen_.load() <
                   10;
    }

    // 设备最近一次轮询上报的程序版本（X-DutyOn-Version 头；与在线时间戳
    // 同请求刷新，故 deviceOnline() 翻真时本值必为最新）：PC 端连接时
    // 与源码哈希比对触发自动更新。空 = 未知（旧固件/未同步）
    std::string deviceVersion() const {
        std::lock_guard<std::mutex> lk(ver_mtx_);
        return device_version_;
    }

private:
    void registerRoutes();
    void runDiscovery();  // UDP 17522 发现应答线程（替代旧 USB ARP 通告）

    StateManager& sm_;
    SysMonitor& monitor_;
    PairingManager& pairing_;
    std::function<void()> quit_handler_;
    httplib::Server* svr_ = nullptr;
    bool running_ = false;
    std::atomic<bool> discovery_run_{false};     // 发现线程运行标志（stop 置否）
    std::thread discovery_thread_;                // UDP 发现应答线程（stop 内 join）
    std::atomic<long long> device_last_seen_{0};  // 秒（steady_clock）
    mutable std::mutex ver_mtx_;                  // 保护 device_version_
    std::string device_version_;                  // 设备上报的程序版本
};

} // namespace dutyon::backend
