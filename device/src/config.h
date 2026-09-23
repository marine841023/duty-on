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

// 事件提示音目录（wav：真 48kHz stereo，内容采样率与标称一致）。音频经 HDMI
// 输出（card1/plughw:1,0，实测时长比 1.0，无需半速率补偿）。文件缺失时回退
// 到正弦波合成。
constexpr const char* kSoundDir = "/opt/dutyon/assets/sounds/";

// 轮询间隔（毫秒）
constexpr int kPollIntervalMs = 500;

// 提示音输出 ALSA 设备（aplay -D）：default=系统默认；生产环境经 systemd
// drop-in 用 DUTYON_AUDIODEV 覆盖为 HDMI 音频 "plughw:1,0"（card1）。
// H616 内置 codec（plughw:0,0）无物理输出，不接功放时静音。
constexpr const char* kAudioDevice = "default";
// 提示音采样率（Hz，mono S16LE，正弦合成）
constexpr int kAudioSampleRate = 22050;

// 渲染目标帧率（Native 路径轻松 60fps，这里保守取 30 平衡功耗）
constexpr int kTargetFps = 30;

// 逻辑渲染分辨率（竖屏 480x800）：所有布局/投影按此尺寸绘制到离屏 FBO，
// 布局为上下分区（角色区 + 任务面板）。物理面板是横屏 800x480（HDMI-A-1 仅
// 支持横屏模式，无 480x800），由 kRotationDeg 把竖屏画面旋转合成上屏——
// 显示器需物理竖放。旋转 90° 后 480x800 恰好铺满 800x480，无黑边。
constexpr int kDisplayWidth = 480;
constexpr int kDisplayHeight = 800;

// 竖屏逻辑画面合成到横屏面板的旋转角（度）：90 或 270（决定显示器竖放朝向）。
// 可用环境变量 DUTYON_ROTATE 覆盖（0/90/180/270），无需重编译即可调方向。
constexpr int kRotationDeg = 90;

// Live2D 模型路径（仓库内置 5 个模型随部署包放到该目录，布局同
// frontend/assets/live2d：<dir>/<name>.model3.json）
constexpr const char* kModelDir = "/opt/dutyon/assets/live2d/";
constexpr const char* kDefaultModel = "nico";

} // namespace dutyon
