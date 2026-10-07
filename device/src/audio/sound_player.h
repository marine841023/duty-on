#pragma once

// 设备端事件提示音：任务开始 / 任务结束 / 任务提醒（待确认）。
// 提醒提示音播放三次，开始/结束各播放一次。
// PCM 由正弦波合成（无需音频资源文件），经 aplay 管道输出到 ALSA；
// 后台线程播放，不阻塞渲染循环；无可用声音设备时静默无操作。

#ifndef _WIN32

#include <cstdint>
#include <string>
#include <vector>

namespace dutyon {

class SoundPlayer {
public:
    enum class Event {
        TaskStart,  // 任务开始：上行双音，1 次
        TaskEnd,    // 任务结束：下行双音，1 次
        Reminder,   // 任务提醒（待确认）：双短促 beep，3 次
        Welcome,    // 欢迎（设备连接）：优先 welcome.wav，缺失回退上行三音
    };

    SoundPlayer();
    ~SoundPlayer();

    // 非阻塞：提示音入队，由后台线程依次播放
    void play(Event ev);

    // 非阻塞：播放音频文件（wav/mp3/ogg/flac/m4a，路径可为任意可读路径）。
    // wav 直通 aplay；压缩格式优先 ffmpeg 解码管到 aplay，退 mpg123。
    // 解码器缺失时忽略并告警一次（stderr）
    void playFile(const std::string& path);

    // 设置软件音量（0-100）：worker 写 PCM 前乘系数；线程安全，
    // 下一块（≤50ms）生效。PC 菜单「设备→音量」经 /api/status 下发
    void setVolume(int percent);

    // 请求强制重建输出流（pclose→popen）：唤醒应答前调用，防 H616 HDMI
    // 间歇静音（软件正常但物理无声，重开 PCM 逼内核重初始化通道）
    void requestRebuild();

private:
    struct Impl;
    Impl* impl_;
};

} // namespace dutyon

#endif // !_WIN32
