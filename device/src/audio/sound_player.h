#pragma once

// 设备端事件提示音：任务开始 / 任务结束 / 任务提醒（待确认）。
// 提醒提示音播放三次，开始/结束各播放一次。
// PCM 由正弦波合成（无需音频资源文件），经 aplay 管道输出到 ALSA；
// 后台线程播放，不阻塞渲染循环；无可用声音设备时静默无操作。

#ifndef _WIN32

#include <cstdint>
#include <vector>

namespace dutyon {

class SoundPlayer {
public:
    enum class Event {
        TaskStart,  // 任务开始：上行双音，1 次
        TaskEnd,    // 任务结束：下行双音，1 次
        Reminder,   // 任务提醒（待确认）：双短促 beep，3 次
    };

    SoundPlayer();
    ~SoundPlayer();

    // 非阻塞：提示音入队，由后台线程依次播放
    void play(Event ev);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace dutyon

#endif // !_WIN32
