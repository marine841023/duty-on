#pragma once

// ---------------------------------------------------------------------------
// 后端服务聚合 —— 单进程化核心。把 Rust 后端（duty-on.exe）的全部职责
// 内嵌进宠物客户端进程：
//
//   StateManager   会话状态机（hook 事件驱动）
//   SysMonitor     系统指标采样线程（1.5s，面板关闭时纯 sleep）
//   HttpServer     127.0.0.1:17521（IDE 桥接 POST /hook + 硬件显示只读 API）
//   扫描线程       IDE 窗口 + CLI 存活（自适应 4s/15s）
//   定时器线程     30s 超时清理 + 60s alert 重提醒
//
// 对 main.cpp 提供与旧 ApiClient 相同的消费面（takeStatus/takeMetrics/
// 菜单动作直调），主循环零网络、零锁竞争。
// ---------------------------------------------------------------------------

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "api/client.h"  // PetStatus / SysMetrics
#include "backend/http_server.h"
#include "backend/pairing_manager.h"
#include "backend/state_manager.h"
#include "backend/sys_monitor.h"

namespace dutyon::backend {

class BackendService {
public:
    BackendService() = default;
    ~BackendService();

    // 启动全部线程与 HTTP 监听。端口被占（旧实例并存）时宠物仍正常运行
    // —— 只是没有 hook 接收，日志警告。
    void start();
    void stop();

    // ---- 本机 UI 消费（替代旧 ApiClient 轮询缓存）----
    // 自上次消费以来状态机有新版本则返回最新快照（首次调用总返回）
    std::optional<dutyon::PetStatus> takeStatus();
    // 自上次消费以来有新采样则返回（委托 SysMonitor）
    std::optional<dutyon::SysMetrics> takeMetrics();
    // 监控面板开关（面板关 = 采样循环纯 sleep 零开销）
    void setMonitorActive(bool active) { monitor_.setActive(active); }

    // ---- 菜单动作（替代旧一次性 HTTP 调用）----
    // 1 开 / 0 关 / -1 查询失败（无注册表访问异常时不会出现）
    int getAutostart();
    bool setAutostart(bool enable);
    // Hook 安装状态 JSON（{"installed":...}，菜单提示与弹窗用）
    std::string getHooks();
    // 安装/刷新 IDE 集成，返回结果 JSON（含 success/installed 字段）
    std::string installHooks();
    // 项目行点击：前置匹配的 IDE 窗口
    bool bringToFront(const std::string& target);

    // ---- 退出 ----
    // 请求整个应用退出（HTTP /api/quit 与本地菜单共用）；主循环每帧用
    // quitRequested() 轮询
    void requestQuit() { quit_requested_ = true; }
    // 硬件显示端在线（已配对设备 10s 内有带 token 的 /api/* 轮询）；菜单"设备"子页用
    bool deviceOnline() const { return http_ && http_->deviceOnline(); }
    // 触发一次「欢迎」（设备连接边沿由主循环调用）：递增序号经 /api/status
    // 下发，设备端据此播放欢迎动作 + 专属音频
    void triggerWelcome() { if (http_) http_->triggerWelcome(); }
    // 设备最近上报的程序版本（连接时与源码哈希比对触发自动更新）；空 = 未知
    std::string deviceVersion() const {
        return http_ ? http_->deviceVersion() : std::string{};
    }

    // ---- 设备配对（Wi-Fi 配对码方案；菜单「设备→配对设备」用）----
    // 待配对请求列表（设备已 POST pair-request，等用户输码确认）
    std::vector<PendingPair> pendingPairings() { return pairing_.pendingList(); }
    // 已配对设备 id 列表
    std::vector<std::string> pairedDevices() { return pairing_.pairedList(); }
    // 用户输入配对码确认：匹配待配对请求即签发 token 持久化，成功返回 true
    bool confirmPairing(const std::string& code) {
        return !pairing_.confirmByCode(code).empty();
    }
    // 解除某设备配对（设备下次轮询得 401 → 自动重新握手）
    bool unpairDevice(const std::string& device_id) {
        return pairing_.unpair(device_id);
    }

    bool quitRequested() const { return quit_requested_; }
    // ApiClient 兼容别名（菜单 quit 项调用面保持一致）
    bool quitApp() {
        requestQuit();
        return true;
    }

    StateManager& stateManager() { return sm_; }
    SysMonitor& sysMonitor() { return monitor_; }

private:
    void runScanner();  // 窗口扫描 + CLI 存活（自适应间隔）
    void runTimers();   // 30s 清理 + 60s alert 重提醒

    StateManager sm_;
    SysMonitor monitor_;
    PairingManager pairing_;  // 声明先于 http_：析构时 http_ 先亡（join 发现线程）
    std::unique_ptr<HttpServer> http_;
    std::atomic<bool> quit_requested_{false};
    std::atomic<bool> run_{false};
    std::thread scanner_thread_;
    std::thread timer_thread_;
    // takeStatus 的消费位（UINT64_MAX = 从未消费，首次总返回）
    uint64_t status_consumer_version_ = UINT64_MAX;
};

} // namespace dutyon::backend
