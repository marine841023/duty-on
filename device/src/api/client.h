#pragma once

#include <map>
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
    // 相框播放源（PC 菜单设定，config.json frameSource，deviceMode=frame 时
    // 才有意义）：motion=动作轮播（现行）/ folder=指定文件夹（PC 本机照片
    // 逐张下发随机播放，见 PhotoPlayer）。旧版后端无此字段时为空=动作轮播
    std::string frame_source;
    // 时钟颜色主题（PC 菜单设定，config.json clockColor）：
    // amber=暗橙(默认)/ice=冰蓝/white=暖白/green=翠绿/pink=粉紫
    std::string clock_color;
    // 屏幕亮度（10-100，PC 菜单"设备→亮度"设定）：设备端优先写 sysfs
    // 背光，无背光接口时以渲染层整屏压暗实现
    int device_brightness = 100;
    // PC 时间（设备无 RTC/NTP 不可信，时钟跟随 PC）：epoch 秒 + PC 本地
    // 时区偏移分钟；设备端取到后用 steady_clock 自行推进直到下次轮询覆盖
    double server_time = 0;
    int utc_offset_min = 0;
    // 状态音频（PC 菜单绑定，config.json stateAudio 按当前角色算好下发）：
    // 状态 -> 音频文件名（相对 ~/.dutyon/animations/）
    std::map<std::string, std::string> active_audio;
    // 完全静音（config.json soundMute）：true 时设备端不播任何音频
    bool sound_mute = false;
    // 当前角色被单独静音的状态列表（config.json stateAudioMuted 按当前
    // 角色键过滤后下发）
    std::vector<std::string> sound_muted_states;
    // 屏幕旋转角（度：0/90/180/270，PC 菜单"设备→屏幕旋转"设定，
    // config.json screenRotation）：设备端逻辑竖屏 480x800 渲染到离屏
    // FBO，swapBuffers 时 quad 按旋转角 blit 到 800x480 横 mode 上屏
    int screen_rotation = 0;
    // 左右翻转（镜像）人物（PC 菜单"左右翻转"设定，config.json
    // flipHorizontal）：设备端同步翻转 Live2D/GIF 角色与 PC 保持一致。
    // 翻转在逻辑场景内进行，与整屏旋转合成正交、可叠加
    bool flip_horizontal = false;
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
    // base_url 可为空：表示链路未建立，轮询线程暂停（开机未入网/未发现
    // PC 即此状态，由 setBaseUrl 接入 pc_discovery 发现的地址）
    explicit ApiClient(const std::string& base_url);
    ~ApiClient();

    // 链路变化时更新目标地址（主线程调用；空串 = 断连暂停轮询）。
    // 线程安全：内部加锁，轮询线程下一周期（≤100ms）生效
    void setBaseUrl(const std::string& url);

    // ---- Wi-Fi 配对码方案：设备身份 + 配对握手 ----
    // 提供 device_id / pair_code；existing_token 非空表示已配对（直接用于
    // 后续请求，不再握手）。链路建立且未配对时，工作线程周期 POST
    // /api/pair-request，拿到 token 后经 takePairToken() 交主线程持久化，
    // 并自动附加到后续 /api/* 请求头（X-DutyOn-Token）。token 失效（PC
    // 侧 401）时自动清除并重新握手，实现自愈。
    void setIdentity(const std::string& device_id, const std::string& pair_code,
                     const std::string& existing_token);

    // 设置本设备程序版本（启动时读 /opt/dutyon/VERSION，由 sync-device.ps1
    // 部署时写入）：附加到每次 /api/status 轮询头 X-DutyOn-Version，供 PC
    // 端与源码哈希比对触发自动更新。空串 = 未知（旧固件/未同步）
    void setProgramVersion(const std::string& version);

    // 工作线程配对成功后返回新 token（消费一次后返回空串）；主线程据此
    // 调 DeviceIdentity::setToken 持久化
    std::string takePairToken();

    // 是否已持有有效 token（本地视角）
    bool paired() const;

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
    // GIF 数 MB 走局域网约 1s；调用方应避免每帧触发
    bool downloadAnimation(const std::string& file_name, const std::string& save_path);

    // 相框「指定文件夹」：向 PC 要下一张照片（GET /api/frame/photo），编码
    // 字节直接收进内存（不落盘，设备侧任意时刻最多一张）。失败（PC 离线 /
    // 无照片）返回 false。调用方应在后台线程执行（网络可能耗时）。
    bool fetchFramePhoto(std::vector<unsigned char>& out_bytes);

    // 下载用户 Live2D 模型文件（GET /live2d/<rel>，rel 相对 PC 端
    // ~/.dutyon/live2d/，可含子目录）到 save_path。路径按段百分号编码，
    // 支持中文/空格目录名；model3.json 引用文件由调用方解析后逐个拉取
    bool downloadLive2dFile(const std::string& rel_path,
                            const std::string& save_path);

private:
    struct Impl;
    Impl* impl_;
};
#endif // !_WIN32

} // namespace dutyon
