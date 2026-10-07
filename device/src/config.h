#pragma once

// 版本号由 CMake 注入（CMakeLists project VERSION 唯一定义处）；
// 直接编译 / IDE 未注入时为 "dev"（云端升级检查禁用）
#ifndef DUTYON_VERSION
#define DUTYON_VERSION "dev"
#endif

namespace dutyon {

// Wi-Fi 局域网连接（配对码方案）：设备加入家庭 Wi-Fi 后与 PC 同网段，
// 经 UDP 广播发现 PC（见 net/pc_discovery.cpp）；PC 端服务器绑 0.0.0.0，
// 由配对 token 门控（见 backend/http_server.cpp）。
constexpr int kApiPort = 17521;
// 设备发现 PC 的 UDP 广播端口（Wi-Fi 配对码方案）：设备广播
// DUTYON_DISCOVER 到此端口，PC 后端监听并单播回 DUTYON_OFFER。
// 见 net/pc_discovery.cpp（设备）与 backend/http_server.cpp（PC）。
constexpr int kDiscoveryPort = 17522;

// 任务列表文字字体（Noto Sans SC Regular，OFL 开源；随部署包放到 assets）
constexpr const char* kFontPath = "/opt/dutyon/assets/font-noto-sc.otf";

// 事件提示音目录（wav：48kHz stereo，内容按 24kHz 生成以补偿 H616 codec
// 在当前内核下 LRCK 只有标称一半的问题）。文件缺失时回退到正弦波合成。
constexpr const char* kSoundDir = "/opt/dutyon/assets/sounds/";

// 轮询间隔（毫秒）
constexpr int kPollIntervalMs = 500;

// 提示音输出 ALSA 设备（aplay -D）。恒定按名字走 HDMI（plughw:CARD=HDMI），
// 不用 default/card 序号——USB 声卡（如麦克风）插入会抢占 card 序号把
// HDMI 挤后（实测 C-Media USB 麦克风占走 card1，HDMI 1→2），序号寻址会
// 被带偏；按 CARD= 名字寻址稳定不受插拔影响。仍可用环境变量
// DUTYON_AUDIODEV 覆盖（如指向 I2S DAC / USB 音箱）
constexpr const char* kAudioDevice = "plughw:CARD=HDMI";
// 提示音采样率（Hz，mono S16LE，正弦合成）
constexpr int kAudioSampleRate = 22050;

// 语音互动（设备端）：唤醒词监听资产目录（kws-spotter 可执行 +
// zipformer2 模型三件套 + tokens.txt + keywords.txt，随部署包推送）
constexpr const char* kKwsDir = "/opt/dutyon/assets/kws/";
// 语音采集麦克风 ALSA 设备（arecord -D）。按 CARD= 名字寻址（C-Media
// USB 麦注册名 "Device"），不受 HDMI 播放卡插拔影响；可用环境变量
// DUTYON_MICDEV 覆盖。无麦时 kws-spotter 退避重启、语音模式不可用
constexpr const char* kMicDevice = "plughw:CARD=Device";

// 渲染目标帧率（Native 路径轻松 60fps，这里保守取 30 平衡功耗）
constexpr int kTargetFps = 30;

// 显示分辨率（期望值：竖屏设备 480x800）。直出模式下实际逻辑尺寸 =
// DRM 选中的模式尺寸——initDrm 优先匹配 480x800，无匹配（如只输出
// 800x480 横模式的屏）时取首个模式，业务布局按实际尺寸自适应
constexpr int kDisplayWidth = 480;
constexpr int kDisplayHeight = 800;

// Live2D 模型路径（仓库内置 5 个模型随部署包放到该目录，布局同
// frontend/assets/live2d：<dir>/<name>.model3.json）
constexpr const char* kModelDir = "/opt/dutyon/assets/live2d/";
constexpr const char* kDefaultModel = "nico";

} // namespace dutyon
