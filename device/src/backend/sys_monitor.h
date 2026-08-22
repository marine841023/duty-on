#pragma once

// ---------------------------------------------------------------------------
// 系统指标采样 —— src-tauri/src/sys_monitor.rs 的 C++ 移植（仅 Windows）。
//
// 专用线程每 1.5s 采样一次：CPU（GetSystemTimes）、内存（内存状态）、
// GPU/显存（NVML 动态加载，无 N 卡时禁用）、网络吞吐（GetIfTable2 增量）、
// 自身 CPU/私有内存。监控面板关闭时采样循环纯 sleep 零开销。
//
// 与 Rust 版的差异：
//   - sysinfo 刷新 -> 直接 Win32 API（GetSystemTimes/GlobalMemoryStatusEx/
//     GetProcessTimes/GetProcessMemoryInfo/GetIfTable2），零第三方依赖
//   - nvml_wrapper -> LoadLibraryW("nvml.dll") 动态加载，无 N 卡时函数表
//     保持空，GPU 行降级为 "—"
//   - LATEST_METRICS + Tauri 事件 -> 内部互斥锁保护的最新快照 +
//     takeMetrics()（seq 防重复消费，主循环零锁竞争轮询）
// ---------------------------------------------------------------------------

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "api/client.h"  // SysMetrics（UI 消费结构）

namespace dutyon::backend {

class SysMonitor {
public:
    SysMonitor() = default;
    ~SysMonitor();

    // 启动采样线程（幂等）
    void start();
    void stop();

    // 监控面板开关（UI 直接调用；false 时采样循环纯 sleep）
    void setActive(bool active);

    // 隐式激活：外部客户端轮询 /api/metrics 时调用（要指标显然就是想看，
    // 与 Rust 版语义一致：直接置 true，无超时回落）
    void pokeActive();

    // 主循环消费：自上次消费以来有新采样则返回最新值，否则 nullopt
    std::optional<dutyon::SysMetrics> takeMetrics();

    // HTTP /api/metrics 用：最新快照（尚无采样返回 nullopt —— 首次调用
    // 后 ~1.5s 内如此，对应 Rust 版的 503）
    std::optional<dutyon::SysMetrics> latestMetrics();

private:
    void runLoop();
    dutyon::SysMetrics sampleOnce(double elapsed_sec);

    std::thread thread_;
    std::atomic<bool> run_{false};
    std::atomic<bool> active_{false};

    std::mutex mtx_;  // 保护 latest_ / consumer_seq_ / 采样基线
    std::optional<dutyon::SysMetrics> latest_;
    uint64_t producer_seq_ = 0;   // 每次新采样 +1
    uint64_t consumer_seq_ = 0;   // takeMetrics 上次消费到的序号

    // ---- 采样基线（仅采样线程访问，无需加锁）----
    // CPU：GetSystemTimes 的 idle/kernel/user 累计值
    uint64_t cpu_idle_ = 0, cpu_kernel_ = 0, cpu_user_ = 0;
    bool cpu_primed_ = false;
    // 自身 CPU：GetProcessTimes 累计值
    uint64_t self_proc_time_ = 0;
    uint64_t self_wall_ = 0;
    bool self_primed_ = false;
    // 网络：全部接口 InOctets/OutOctets 总和
    uint64_t net_rx_total_ = 0, net_tx_total_ = 0;
    bool net_primed_ = false;
};

} // namespace dutyon::backend
