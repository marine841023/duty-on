#pragma once

namespace dutyon {

// USB 直连（NCM gadget，Windows 内置 usbnet 免驱）：设备以 USB 网卡形态
// 接入 PC，usb0 = 192.168.7.1 并通过 systemd-networkd 内置 DHCP 给 PC 派发
// 地址；应用层从 ARP 邻居表自动发现 PC（见 net/usb_link.cpp）。
// PC 端仍需在 ~/.dutyon/config.json 设 "externalAccess": true（服务器改绑
// 0.0.0.0，http_server.cpp 读取）。
constexpr const char* kUsbLinkName = "usb0";
constexpr int kApiPort = 17521;

// 开机引导横幅（未插 USB 时屏幕底部提示"请通过 USB 连接电脑"）
constexpr const char* kPromptBannerPath = "/opt/dutyon/assets/prompt-usb.png";

// 任务列表文字字体（Noto Sans SC Regular，OFL 开源；随部署包放到 assets）
constexpr const char* kFontPath = "/opt/dutyon/assets/font-noto-sc.otf";

// 轮询间隔（毫秒）
constexpr int kPollIntervalMs = 500;

// 渲染目标帧率（Native 路径轻松 60fps，这里保守取 30 平衡功耗）
constexpr int kTargetFps = 30;

// 显示分辨率（香橙派 Zero 2W 外接竖屏 480x800，fb0/DRM 实测）
constexpr int kDisplayWidth = 480;
constexpr int kDisplayHeight = 800;

// Live2D 模型路径（仓库内置 5 个模型随部署包放到该目录，布局同
// frontend/assets/live2d：<dir>/<name>.model3.json）
constexpr const char* kModelDir = "/opt/dutyon/assets/live2d/";
constexpr const char* kDefaultModel = "nico";

} // namespace dutyon
