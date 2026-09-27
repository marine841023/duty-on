#pragma once

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

// 提示音输出 ALSA 设备（aplay -D）：default=系统默认；接 I2S/USB 声卡后
// 可用环境变量 DUTYON_AUDIODEV 覆盖（如 "hw:1,0" 指向 I2S DAC）
constexpr const char* kAudioDevice = "default";
// 提示音采样率（Hz，mono S16LE，正弦合成）
constexpr int kAudioSampleRate = 22050;

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
