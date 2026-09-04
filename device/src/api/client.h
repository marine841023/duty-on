#pragma once

#include <optional>
#include <string>
#include <vector>

#include "config/user_config.h"  // CustomCharacter（fetchCharacter 返回值）

namespace dutyon {

// 与 PC 端 /api/status 返回的 Snapshot 结构对应
// （device/src/backend/state_manager.cpp，camelCase）
//
//   {
//     "overallState": "sleeping" | "working" | "alert",
//     "sessions": [ { "projectName": ..., "status": ..., "ide": ... } ],
//     "activeCharacter": "assets/live2d/<name>.model3.json",
//     "lastEventAt": 1692...,
//     "timestamp": 1692...
//   }

// 单个 IDE 会话（= 项目列表里的一行）
struct SessionInfo {
    std::string project_name;
    std::string status;          // "idle"|"working"|"thinking"|"tool-use"|"confirmation-needed"
    std::string ide;             // "trae"|"qoder"|"cursor"|"codex"|"opencode"（可为空）
    std::string alert_message;   // 需要确认时的提示文本（可为空）
};

struct PetStatus {
    std::string overall_state;  // "sleeping" | "working" | "alert"
    std::vector<SessionInfo> sessions;  // 项目列表数据
    int session_count = 0;      // 活跃会话数（sessions 数组长度）
    bool has_confirmation = false; // 有会话处于 confirmation-needed
    // PC 当前角色键（与 UserConfigStore::listModels 的 ModelEntry::key 同格式）；
    // 设备端据此热切换形象，与 PC 设定保持一致。旧版后端无此字段时为空串。
    std::string active_character;
    // 硬件显示端布局模式（PC 菜单设定，config.json deviceMode）：
    // single=单任务（角色全屏+大时钟）/ multi=多任务（角色+任务列表，默认）/
    // frame=电子相框（角色全屏循环播放动作，不响应任务状态）
    std::string device_mode;
    // PC 时间（设备无 RTC/NTP 不可信，时钟跟随 PC）：epoch 秒 + PC 本地
    // 时区偏移分钟；设备端取到后用 steady_clock 自行推进直到下次轮询覆盖
    double server_time = 0;
    int utc_offset_min = 0;
};

// 与 PC 端 /api/metrics 返回的 MetricsSnapshot 对应
// （device/src/backend/sys_monitor.cpp，camelCase）
// GPU 字段在无 N 卡时服务端给 null，这里用 has_gpu 标记
struct SysMetrics {
    float cpu_usage = 0.0f;     // 0-100
    unsigned long long mem_total = 0;
    unsigned long long mem_used = 0;
    bool has_gpu = false;
    std::string gpu_name;
    float gpu_usage = 0.0f;
    unsigned long long vram_total = 0;
    unsigned long long vram_used = 0;
    unsigned long long net_rx_rate = 0;  // bytes/sec
    unsigned long long net_tx_rate = 0;  // bytes/sec
    float self_cpu = 0.0f;
    unsigned long long self_mem = 0;
};

#ifndef _WIN32
// 后台轮询客户端（仅 ARM Linux 设备端）：构造即启动工作线程
// （状态 ~2Hz / 监控 ~0.7Hz），主渲染线程只通过 take* 读缓存，
// 网络阻塞不会影响帧率。PC 端内嵌后端直连，不走此类。
class ApiClient {
public:
    // base_url 可为空：表示链路未建立，轮询线程暂停（USB 直连场景下
    // 开机未插线即此状态，由 setBaseUrl 接入租约发现的地址）
    explicit ApiClient(const std::string& base_url);
    ~ApiClient();

    // USB 链路变化时更新目标地址（主线程调用；空串 = 断连暂停轮询）。
    // 线程安全：内部加锁，轮询线程下一周期（≤100ms）生效
    void setBaseUrl(const std::string& url);

    // 取自上次消费以来最新一次成功轮询的状态；无新数据返回 nullopt
    std::optional<PetStatus> takeStatus();

    // 取自上次消费以来最新一次成功轮询的监控数据；无新数据返回 nullopt
    std::optional<SysMetrics> takeMetrics();

    // ---- 一次性动作（菜单触发；在调用线程同步执行，短超时）----

    // 项目行点击：把对应 IDE 窗口前置（v1 bringToFront）
    bool bringToFront(const std::string& target);

    // Hook 安装状态查询（菜单 "Hook 状态"）；返回原始 JSON
    std::string getHooks();

    // 安装/刷新 IDE 集成（菜单 "安装 IDE 集成"）；返回结果 JSON
    std::string installHooks();

    // 开机自启动状态：1 开 / 0 关 / -1 查询失败（后端未启动）
    int getAutostart();

    // 设置开机自启动；返回是否成功
    bool setAutostart(bool enable);

    // 退出整个应用（后端 + 宠物客户端一起退出，v1 菜单 "退出"）
    bool quitApp();

    // 从 PC 拉取当前自定义 GIF 角色定义（/api/character）。
    // expect_id 不匹配（PC 已又切换）或失败时返回空 id
    CustomCharacter fetchCharacter(const std::string& expect_id);

    // 下载自定义形象动画文件（/api/animations/<file>）到 save_path。
    // GIF 数 MB 走 USB 直连约 1s；调用方应避免每帧触发
    bool downloadAnimation(const std::string& file_name, const std::string& save_path);

private:
    struct Impl;
    Impl* impl_;
};
#endif // !_WIN32

} // namespace dutyon
