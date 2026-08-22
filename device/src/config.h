#pragma once

namespace dutyon {

// 桌面端 Duty On HTTP 服务地址（硬件端从此轮询状态）
constexpr const char* kApiBaseUrl = "http://192.168.1.100:23333";

// 轮询间隔（毫秒）
constexpr int kPollIntervalMs = 500;

// 渲染目标帧率（Native 路径轻松 60fps，这里保守取 30 平衡功耗）
constexpr int kTargetFps = 30;

// 显示分辨率（根据实际屏幕修改）
constexpr int kDisplayWidth = 480;
constexpr int kDisplayHeight = 480;

// Live2D 模型路径
constexpr const char* kModelDir = "/opt/dutyon/models/";
constexpr const char* kDefaultModel = "miku_pro";

} // namespace dutyon
