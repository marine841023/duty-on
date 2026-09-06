#ifndef _WIN32  // 仅设备端（ARM Linux ALSA）

#include "audio/sound_player.h"

#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "config.h"

namespace dutyon {

namespace {

// PCM 合成（mono S16LE @ kAudioSampleRate）：正弦波 + 线性淡入淡出防爆音
void appendSilence(std::vector<int16_t>& out, float ms) {
    const int n = (int)((float)kAudioSampleRate * ms / 1000.f);
    out.insert(out.end(), (size_t)n, 0);
}

void appendTone(std::vector<int16_t>& out, float freq, float ms,
                float amp = 0.35f) {
    const int n = (int)((float)kAudioSampleRate * ms / 1000.f);
    int fade = (int)((float)kAudioSampleRate * 0.010f);  // 10ms 淡入淡出
    if (fade > n / 2) fade = n / 2;
    for (int i = 0; i < n; ++i) {
        float env = 1.f;
        if (i < fade)
            env = (float)i / (float)fade;
        else if (i > n - 1 - fade)
            env = (float)(n - 1 - i) / (float)fade;
        const float t = (float)i / (float)kAudioSampleRate;
        const float v = amp * env * std::sin(6.2831853f * freq * t);
        out.push_back((int16_t)(v * 32767.f));
    }
}

// 任务开始：上行双音（1 次）
std::vector<int16_t> buildStart() {
    std::vector<int16_t> b;
    appendTone(b, 660.f, 120.f);
    appendTone(b, 880.f, 200.f);
    return b;
}

// 任务结束：下行双音（1 次）
std::vector<int16_t> buildEnd() {
    std::vector<int16_t> b;
    appendTone(b, 880.f, 120.f);
    appendTone(b, 659.f, 200.f);
    return b;
}

// 任务提醒：双短促 beep 为一次，共播三次
std::vector<int16_t> buildReminder() {
    std::vector<int16_t> b;
    for (int i = 0; i < 3; ++i) {
        appendTone(b, 1046.f, 90.f);
        appendSilence(b, 70.f);
        appendTone(b, 1046.f, 90.f);
        if (i < 2) appendSilence(b, 260.f);
    }
    return b;
}

} // namespace

struct SoundPlayer::Impl {
    std::deque<std::vector<int16_t>> queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    bool warn_once_ = false;
    std::thread worker_;

    void start() { worker_ = std::thread([this] { run(); }); }

    void run() {
        for (;;) {
            std::vector<int16_t> buf;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [this] { return stop_ || !queue_.empty(); });
                if (queue_.empty()) return;  // stop_ 且已空
                buf = std::move(queue_.front());
                queue_.pop_front();
            }
            playPcm(buf);
        }
    }

    // 输出设备：环境变量 DUTYON_AUDIODEV 覆盖（接 I2S/USB 声卡后指向它），
    // 否则用 config.h 默认值
    static std::string device() {
        const char* env = getenv("DUTYON_AUDIODEV");
        return (env && *env) ? std::string(env) : std::string(kAudioDevice);
    }

    void playPcm(const std::vector<int16_t>& pcm) {
        if (pcm.empty()) return;
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "aplay -q -D %s -t raw -f S16_LE -r %d -c 1 -",
                 device().c_str(), kAudioSampleRate);
        FILE* f = popen(cmd, "w");
        if (!f) {
            if (!warn_once_) {
                warn_once_ = true;
                fprintf(stderr, "[Sound] aplay unavailable, sound disabled\n");
            }
            return;
        }
        fwrite(pcm.data(), sizeof(int16_t), pcm.size(), f);
        pclose(f);
    }

    void enqueue(std::vector<int16_t> buf) {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (queue_.size() >= 4) queue_.pop_front();  // 防提示音堆积
            queue_.push_back(std::move(buf));
        }
        cv_.notify_one();
    }
};

SoundPlayer::SoundPlayer() : impl_(new Impl) { impl_->start(); }

SoundPlayer::~SoundPlayer() {
    {
        std::lock_guard<std::mutex> lk(impl_->mu_);
        impl_->stop_ = true;
    }
    impl_->cv_.notify_all();
    if (impl_->worker_.joinable()) impl_->worker_.join();
    delete impl_;
}

void SoundPlayer::play(Event ev) {
    switch (ev) {
        case Event::TaskStart:
            impl_->enqueue(buildStart());
            break;
        case Event::TaskEnd:
            impl_->enqueue(buildEnd());
            break;
        case Event::Reminder:
            impl_->enqueue(buildReminder());
            break;
    }
}

} // namespace dutyon

#endif // !_WIN32
