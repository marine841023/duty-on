#ifndef _WIN32  // 仅设备端（ARM Linux ALSA）

#include "audio/sound_player.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config.h"

namespace dutyon {

namespace {

// ---------------------------------------------------------------------------
// 流格式约定
//
// H616 codec 在当前内核（6.18.45 / Armbian trunk）下 audio-codec-4x 时钟只有
// 期望的一半，导致 LRCK = 标称/2 —— 按标称采样率播放会变慢一倍且音调降八度。
// 对策是「半速率补偿」：aplay 流按 48000Hz 标称打开，但喂进去的 PCM 内容一律
// 按 24000Hz 生成。assets/sounds/*.wav 遵循同一约定（header 标 48000、内容是
// 24000 采样），因此直接取其 PCM 即可，无需重采样。
//
// 换 stable 内核后若 `time aplay` 实测时长比恢复 1.0，需同步取消补偿：
// 把 kContentRate 改回 kStreamRate，并用 --no-compensate 重新生成 wav。
// ---------------------------------------------------------------------------
constexpr int kStreamRate = 48000;    // aplay raw 流标称采样率
constexpr int kStreamCh = 2;          // stereo
constexpr int kContentRate = 24000;   // PCM 内容真实采样率（半速率补偿）
constexpr int kChunkFrames = 1200;    // 每块 50ms（@kContentRate）
constexpr int kChunkSamples = kChunkFrames * kStreamCh;
constexpr int kPrebufferChunks = 10;  // 启动预缓冲 500ms，防开局 underrun

// ---- PCM 合成兜底（wav 缺失时用）：stereo S16LE @kContentRate -------------
void appendSilence(std::vector<int16_t>& out, float ms) {
    const int frames = (int)((float)kContentRate * ms / 1000.f);
    out.insert(out.end(), (size_t)frames * kStreamCh, 0);
}

void appendTone(std::vector<int16_t>& out, float freq, float ms,
                float amp = 0.35f) {
    const int frames = (int)((float)kContentRate * ms / 1000.f);
    int fade = (int)((float)kContentRate * 0.010f);  // 10ms 淡入淡出
    if (fade > frames / 2) fade = frames / 2;
    for (int i = 0; i < frames; ++i) {
        float env = 1.f;
        if (i < fade)
            env = (float)i / (float)fade;
        else if (i > frames - 1 - fade)
            env = (float)(frames - 1 - i) / (float)fade;
        const float t = (float)i / (float)kContentRate;
        const float v = amp * env * std::sin(6.2831853f * freq * t);
        const int16_t s = (int16_t)(v * 32767.f);
        out.push_back(s);
        out.push_back(s);  // 左右声道相同
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

// ---- wav 读取：只接受 16bit PCM 且格式与流一致，否则返回空由调用方兜底 -----
std::vector<int16_t> loadWav(const std::string& path) {
    std::vector<int16_t> out;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return out;

    char riff[12];
    if (fread(riff, 1, 12, f) != 12 || memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(f);
        return out;
    }

    bool got_fmt = false;
    uint32_t rate = 0;
    uint16_t ch = 0;
    for (;;) {
        char id[4];
        uint32_t sz = 0;
        if (fread(id, 1, 4, f) != 4) break;
        if (fread(&sz, 4, 1, f) != 1) break;

        if (memcmp(id, "fmt ", 4) == 0 && sz >= 16) {
            uint16_t tag = 0, nch = 0, align = 0, bps = 0;
            uint32_t sr = 0, br = 0;
            if (fread(&tag, 2, 1, f) != 1 || fread(&nch, 2, 1, f) != 1 ||
                fread(&sr, 4, 1, f) != 1 || fread(&br, 4, 1, f) != 1 ||
                fread(&align, 2, 1, f) != 1 || fread(&bps, 2, 1, f) != 1) {
                break;
            }
            if (sz > 16) fseek(f, (long)(sz - 16), SEEK_CUR);
            if (tag != 1 || bps != 16) {  // 仅支持未压缩 16bit PCM
                fprintf(stderr, "[Sound] %s: unsupported fmt tag=%u bps=%u\n",
                        path.c_str(), (unsigned)tag, (unsigned)bps);
                break;
            }
            rate = sr;
            ch = nch;
            got_fmt = true;
        } else if (memcmp(id, "data", 4) == 0 && got_fmt) {
            if (rate != (uint32_t)kStreamRate || ch != (uint16_t)kStreamCh) {
                fprintf(stderr,
                        "[Sound] %s: %uHz/%uch != stream %dHz/%dch, fallback\n",
                        path.c_str(), rate, (unsigned)ch, kStreamRate,
                        kStreamCh);
                break;
            }
            const size_t n = sz / sizeof(int16_t);
            out.resize(n);
            const size_t rd = fread(out.data(), sizeof(int16_t), n, f);
            out.resize(rd);
            break;
        } else {
            fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);  // chunk 奇数长度需对齐
        }
    }

    fclose(f);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 常驻流实现
//
// 关键：声卡设备**只 open 一次、服务退出时才 close**。此前的实现每播一次提示音
// 都 popen 一个 aplay，codec 随之上下电，产生无法用软件 mute 消除的硬件 pop
// （实测 amixer mute 状态在播放期间不被驱动重置，但首尾 pop 依然存在）。
// 改为后台线程持续喂 50ms 静音块保持流不断，提示音 PCM 插进队列由同一线程
// 取出写入，pop 便只剩服务启动/退出各一次（开机、关机时刻，日常无感）。
// ---------------------------------------------------------------------------
struct SoundPlayer::Impl {
    FILE* pipe_ = nullptr;
    std::deque<std::vector<int16_t>> queue_;
    std::mutex mu_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};  // 正在停机：抑制管道断开的误报警
    std::thread worker_;
    bool warn_once_ = false;

    // 输出设备：环境变量 DUTYON_AUDIODEV 覆盖（接 I2S/USB 声卡后指向它），
    // 否则用 config.h 默认值
    static std::string device() {
        const char* env = getenv("DUTYON_AUDIODEV");
        return (env && *env) ? std::string(env) : std::string(kAudioDevice);
    }

    void start() {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "aplay -q -D %s -t raw -f S16_LE -r %d -c %d -",
                 device().c_str(), kStreamRate, kStreamCh);
        pipe_ = popen(cmd, "w");
        if (!pipe_) {
            fprintf(stderr, "[Sound] aplay unavailable, sound disabled\n");
            return;
        }

        running_ = true;
        // 预缓冲：先灌静音建立水位，再启动写线程，避免开局 underrun
        const std::vector<int16_t> silence(kChunkSamples, 0);
        for (int i = 0; i < kPrebufferChunks; ++i) writeChunk(silence);

        worker_ = std::thread([this] { run(); });
        printf("[Sound] stream ready: %s %dHz/%dch (content %dHz)\n",
               device().c_str(), kStreamRate, kStreamCh, kContentRate);
    }

    void stop() {
        // 注意：systemd 默认 KillMode=control-group，SIGTERM 会连 aplay 一起杀掉，
        // 写线程会在本函数被调之前就拿到 EPIPE（无法用 stopping_ 完全拦住）。
        // 不能改成 KillMode=process：那样 SIGTERM 只到主进程，若主进程未注册
        // 信号处理就直接终止，aplay 会成为孤儿进程持续占用声卡。
        stopping_ = true;
        running_ = false;
        if (worker_.joinable()) worker_.join();
        if (pipe_) {
            pclose(pipe_);  // 关闭流，codec 下电（此刻有最后一次 pop）
            pipe_ = nullptr;
        }
    }

    // 写一块到管道；fwrite 受管道背压自然限速，无需额外 sleep
    bool writeChunk(const std::vector<int16_t>& chunk) {
        if (!pipe_) return false;
        const size_t n = fwrite(chunk.data(), sizeof(int16_t), chunk.size(), pipe_);
        if (n != chunk.size()) {
            if (!warn_once_ && !stopping_.load()) {
                warn_once_ = true;
                // 措辞中性：正常停机时 systemd 连 aplay 一起 SIGTERM，这里必然
                // 拿到 EPIPE，属预期路径；运行中 aplay 意外退出也走同一分支。
                fprintf(stderr, "[Sound] aplay stream closed\n");
            }
            return false;
        }
        fflush(pipe_);
        return true;
    }

    void run() {
        std::vector<int16_t> chunk(kChunkSamples, 0);
        while (running_.load()) {
            bool filled = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!queue_.empty()) {
                    std::vector<int16_t>& head = queue_.front();
                    const size_t take =
                        head.size() < (size_t)kChunkSamples ? head.size()
                                                            : (size_t)kChunkSamples;
                    std::memcpy(chunk.data(), head.data(),
                                take * sizeof(int16_t));
                    head.erase(head.begin(), head.begin() + (long)take);
                    if (head.empty()) queue_.pop_front();
                    filled = true;
                }
            }
            if (!filled) std::fill(chunk.begin(), chunk.end(), 0);  // 空闲喂静音

            if (!writeChunk(chunk)) break;  // 管道断了就不再空转
        }
    }

    void enqueue(std::vector<int16_t> buf) {
        if (buf.empty() || !pipe_) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (queue_.size() >= 4) queue_.pop_front();  // 防提示音堆积
        queue_.push_back(std::move(buf));
    }

    // 取提示音 PCM：优先读 wav 资源，缺失或格式不符则回退合成音
    std::vector<int16_t> acquire(const char* file,
                                 std::vector<int16_t> (*fallback)()) {
        std::vector<int16_t> pcm = loadWav(std::string(kSoundDir) + file);
        if (pcm.empty()) {
            if (!warn_once_) {
                warn_once_ = true;
                fprintf(stderr, "[Sound] %s%s missing, using synth fallback\n",
                        kSoundDir, file);
            }
            pcm = fallback();
        }
        return pcm;
    }
};

SoundPlayer::SoundPlayer() : impl_(new Impl) { impl_->start(); }

SoundPlayer::~SoundPlayer() {
    impl_->stop();
    delete impl_;
}

void SoundPlayer::play(Event ev) {
    switch (ev) {
        case Event::TaskStart:
            impl_->enqueue(impl_->acquire("mission_start.wav", buildStart));
            break;
        case Event::TaskEnd:
            impl_->enqueue(impl_->acquire("mission_complete.wav", buildEnd));
            break;
        case Event::Reminder:
            impl_->enqueue(impl_->acquire("attention.wav", buildReminder));
            break;
    }
}

}  // namespace dutyon

#endif  // !_WIN32
