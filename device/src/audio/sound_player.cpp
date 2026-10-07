#ifndef _WIN32  // 仅设备端（ARM Linux ALSA）

#include "audio/sound_player.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "config.h"

namespace dutyon {

namespace {

// ---------------------------------------------------------------------------
// 流格式约定
//
// 音频经 HDMI 输出（card1 / plughw:1,0）：实测 3.000s 测试 wav 的 aplay 时长比
// 为 1.005，即 HDMI 路径按真实 48000Hz 播放，不存在 H616 codec 那种 LRCK 减半
// 的问题。因此取消半速率补偿：PCM 内容一律按 48000Hz 生成，与 aplay 流标称一致。
// assets/sounds/*.wav 同样是真 48000Hz/2ch（内容与 header 一致），直接取其 PCM。
//
// 若日后改回 H616 内置 codec（plughw:0,0，LRCK = 标称/2）或换回竖屏 I2S 方案，
// 需恢复补偿：把 kContentRate 改回 24000，并用带补偿的 wav（内容 24k/header 48k）。
// ---------------------------------------------------------------------------
constexpr int kStreamRate = 48000;    // aplay raw 流标称采样率
constexpr int kStreamCh = 2;          // stereo
constexpr int kContentRate = 48000;   // PCM 内容真实采样率（= kStreamRate，无补偿）
constexpr int kChunkFrames = 2400;    // 每块 50ms（@kContentRate）
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

// 欢迎（welcome.wav 缺失时兜底）：上行三音，轻快招呼感
std::vector<int16_t> buildWelcome() {
    std::vector<int16_t> b;
    appendTone(b, 659.f, 110.f);
    appendTone(b, 831.f, 110.f);
    appendTone(b, 988.f, 220.f);
    return b;
}

// ---- wav 读取：只接受 16bit PCM（任意采样率/声道数）。成功返回 interleaved
// 样本并写回 rate/ch；非 wav 或压缩格式返回空由调用方处理 --------------------
std::vector<int16_t> loadWavRaw(const std::string& path, uint32_t* out_rate,
                                uint16_t* out_ch) {
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
    if (out_rate) *out_rate = rate;
    if (out_ch) *out_ch = ch;
    return out;
}

// 资源语音文件（真 48k stereo，内容与 header 一致），格式相符时样本按原样注入常驻流
std::vector<int16_t> loadWav(const std::string& path) {
    uint32_t rate = 0;
    uint16_t ch = 0;
    std::vector<int16_t> out = loadWavRaw(path, &rate, &ch);
    if (!out.empty() &&
        (rate != (uint32_t)kStreamRate || ch != (uint16_t)kStreamCh)) {
        fprintf(stderr,
                "[Sound] %s: %uHz/%uch != stream %dHz/%dch, fallback\n",
                path.c_str(), rate, (unsigned)ch, kStreamRate, kStreamCh);
        out.clear();
    }
    return out;
}

// 用户绑定 wav（任意采样率/声道数）转流内容格式（48k stereo）：
// mono → stereo 复制，再线性重采样 rate → kContentRate —— 与常驻流一致
// （HDMI 真实 48000Hz 播放，音调时长正确）
std::vector<int16_t> resampleToContent(const std::vector<int16_t>& in,
                                       uint32_t rate, uint16_t ch) {
    if (in.empty() || rate == 0 || (ch != 1 && ch != 2)) return {};
    const size_t inFrames = in.size() / ch;
    std::vector<int16_t> st;
    if (ch == 1) {
        st.resize(inFrames * 2);
        for (size_t i = 0; i < inFrames; ++i) {
            st[2 * i] = in[i];
            st[2 * i + 1] = in[i];
        }
    } else {
        st = in;
    }
    if (rate == (uint32_t)kContentRate) return st;
    const double step = (double)rate / (double)kContentRate;
    const size_t outFrames = (size_t)((double)inFrames / step);
    if (outFrames == 0) return {};
    std::vector<int16_t> out(outFrames * 2);
    for (size_t i = 0; i < outFrames; ++i) {
        const double pos = (double)i * step;
        const size_t i0 = (size_t)pos;
        const size_t i1 = (i0 + 1 < inFrames) ? i0 + 1 : i0;
        const double t = pos - (double)i0;
        for (int c = 0; c < 2; ++c) {
            const double a = st[i0 * 2 + c];
            const double b = st[i1 * 2 + c];
            out[i * 2 + c] = (int16_t)(a + (b - a) * t + 0.5);
        }
    }
    return out;
}

// ---- 外部命令与 shell 引用（playFile 压缩格式解码用） ----------------------
bool haveCmd(const char* cmd) {
    static std::map<std::string, bool> cache;
    auto it = cache.find(cmd);
    if (it != cache.end()) return it->second;
    const std::string c = std::string("command -v ") + cmd + " >/dev/null 2>&1";
    const bool ok = system(c.c_str()) == 0;
    cache[cmd] = ok;
    return ok;
}

// shell 单引号包裹并转义（路径由本程序生成通常安全，仍防御单引号）
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char ch : s) {
        if (ch == '\'')
            out += "'\\''";
        else
            out += ch;
    }
    out += "'";
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
// aplay 意外退出（如开机早于 HDMI 声卡注册）或卡死时由 respawn() 重建流。
//
// v4.2（2026-10-06）：popen/pclose → fork/execv 直控 aplay PID。
// 根因：H616 HDMI DMA 偶发死锁（USB 麦持续采集时高发），aplay 阻塞在
// snd_pcm_writei 永不退出——pclose() 死等 aplay → worker 卡死，pre-ack
// 重建与 15 分钟健康重建全部失效（实测 stream rebuild 后无 respawned 日志）。
// 直控 PID 后：writeChunk 用 poll 超时检测管道拥塞（aplay 不读 stdin），
// respawn 先 SIGKILL 再 waitpid（无条件、不阻塞），卡死流必能重建。
// ---------------------------------------------------------------------------
struct SoundPlayer::Impl {
    int pipe_fd_ = -1;      // aplay stdin 管道写端（父进程持有）
    pid_t aplay_pid_ = -1;  // aplay 子进程（直控，可 SIGKILL）
    std::deque<std::vector<int16_t>> queue_;
    std::mutex mu_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};  // 正在停机：抑制管道断开的误报警
    std::atomic<bool> rebuild_requested_{false};  // 唤醒应答前强制重建请求
    std::atomic<int> volume_{80};  // 软件音量 0-100（PC 菜单「设备→音量」下发）
    std::thread worker_;
    bool warn_once_ = false;
    // pcm 流最近一次打开时刻：HDMI 链路保活用（见 run 中的定期重建）
    std::chrono::steady_clock::time_point stream_opened_at_{};
    // 保活间隔：USB 麦在位时 HDMI 音频通道可能一次性卡死（ALSA 层仍在
    // 消费、物理输出无声，流重开即恢复）。15 分钟空闲重建一次，
    // pclose→popen 约 100ms 静音缝隙 + 一次轻微 codec 上下电 pop，无感。
    static constexpr int kHealthIntervalMin = 15;

    // 输出设备：环境变量 DUTYON_AUDIODEV 覆盖（接 I2S/USB 声卡后指向它），
    // 否则用 config.h 默认值
    static std::string device() {
        const char* env = getenv("DUTYON_AUDIODEV");
        return (env && *env) ? std::string(env) : std::string(kAudioDevice);
    }

    // fork/execv 启动 aplay（stdin = 管道读端，raw 流模式）
    bool spawnAplay() {
        int fds[2];
        if (::pipe(fds) != 0) return false;
        const std::string rate = std::to_string(kStreamRate);
        const std::string ch = std::to_string(kStreamCh);
        // argv 在 fork 前构建（子进程内不 malloc）
        std::vector<std::string> argsStr = {
            "aplay", "-q", "-D", device(), "-t", "raw",
            "-f", "S16_LE", "-r", rate, "-c", ch, "-"};
        std::vector<char*> argv;
        for (auto& a : argsStr) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        const pid_t pid = ::fork();
        if (pid < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return false;
        }
        if (pid == 0) {  // 子进程：stdin←管道读端，exec aplay
            ::dup2(fds[0], STDIN_FILENO);
            ::close(fds[0]);
            ::close(fds[1]);
            ::execvp("aplay", argv.data());
            _exit(127);  // exec 失败
        }
        ::close(fds[0]);
        pipe_fd_ = fds[1];
        aplay_pid_ = pid;
        return true;
    }

    // 强杀并回收 aplay（SIGKILL 无条件，HDMI DMA 卡死也能解除；不阻塞）
    void killAplay() {
        if (aplay_pid_ > 0) {
            ::kill(aplay_pid_, SIGKILL);
            ::waitpid(aplay_pid_, nullptr, 0);
            aplay_pid_ = -1;
        }
        if (pipe_fd_ >= 0) {
            ::close(pipe_fd_);
            pipe_fd_ = -1;
        }
    }

    void start() {
        ::signal(SIGPIPE, SIG_IGN);  // aplay 被杀后 write 得 EPIPE 而非进程终止
        if (!spawnAplay()) {
            fprintf(stderr, "[Sound] aplay unavailable, sound disabled\n");
            return;
        }

        running_ = true;
        // 预缓冲：先灌静音建立水位，再启动写线程，避免开局 underrun
        const std::vector<int16_t> silence(kChunkSamples, 0);
        for (int i = 0; i < kPrebufferChunks; ++i) writeChunk(silence);
        stream_opened_at_ = std::chrono::steady_clock::now();

        worker_ = std::thread([this] { run(); });
        printf("[Sound] stream ready: %s %dHz/%dch (content %dHz)\n",
               device().c_str(), kStreamRate, kStreamCh, kContentRate);
    }

    void stop() {
        stopping_ = true;
        running_ = false;
        if (worker_.joinable()) worker_.join();
        killAplay();  // 关闭流，codec 下电（此刻有最后一次 pop）
    }

    // 写一块到 aplay stdin。poll 250ms 超时检测 aplay 卡死（DMA 死锁时不读
    // stdin、管道满 → POLLOUT 不就绪 → false → 上层 respawn 强杀重建）
    bool writeChunk(const std::vector<int16_t>& chunk) {
        if (pipe_fd_ < 0) return false;
        struct pollfd pfd;
        pfd.fd = pipe_fd_;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        if (::poll(&pfd, 1, 250) != 1) return false;  // 超时 = 流卡死
        const size_t bytes = chunk.size() * sizeof(int16_t);
        const ssize_t n = ::write(pipe_fd_, chunk.data(), bytes);
        if (n != (ssize_t)bytes) {
            if (!warn_once_ && !stopping_.load()) {
                warn_once_ = true;
                // 措辞中性：正常停机时连 aplay 一起杀，这里必然拿到 EPIPE，
                // 属预期路径；运行中 aplay 意外退出也走同一分支。
                fprintf(stderr, "[Sound] aplay stream closed\n");
            }
            return false;
        }
        return true;
    }

    void run() {
        std::vector<int16_t> chunk(kChunkSamples, 0);
        while (running_.load()) {
            // 应答前强制重建请求（唤醒应答防 H616 HDMI 间歇静音）：
            // 重建后本轮流空 chunk 预热，下一轮恢复常规输出
            if (rebuild_requested_.exchange(false)) {
                printf("[Sound] stream rebuild (pre-ack)\n");
                if (!respawn()) break;
                continue;
            }
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

            // HDMI 链路保活：空闲且距上次开流超过间隔时主动重建 pcm 流。
            // 卡死时 ALSA 层照常消费（writeChunk 不会失败），只能靠定时
            // pclose→popen 强制 HDMI 音频通道重新初始化来自愈。
            if (!filled && std::chrono::steady_clock::now() - stream_opened_at_ >
                               std::chrono::minutes(kHealthIntervalMin)) {
                printf("[Sound] stream health rebuild (every %dmin idle)\n",
                       kHealthIntervalMin);
                if (!respawn()) break;  // 停机中 / 重建最终失败
                continue;               // respawn 已预缓冲，进入下一轮
            }

            // 软件音量（0-100，PC 菜单「设备→音量」下发）：写入前按系数
            // 缩放；>=100 直通省 CPU。无声问题已确认是 USB 麦克风在位时
            // HDMI 链路一次性卡死（与软件无关），缩放逻辑恢复启用
            const int vol = volume_.load(std::memory_order_relaxed);
            if (vol < 100) {
                const float f = (float)vol / 100.f;
                for (auto& s : chunk) s = (int16_t)((float)s * f);
            }
            if (writeChunk(chunk)) continue;
            if (stopping_.load()) break;  // 正常停机（systemd 连 aplay 一起杀）
            if (!respawn()) break;        // aplay 意外退出：重建流（见 respawn）
        }
    }

    // 流重建：aplay 意外退出或卡死（H616 HDMI DMA 死锁）后的恢复路径。
    // 典型场景：① 开机竞态——dutyon.service 不再等 HDMI 声卡注册（内核
    // deferred probe ~9s）就启动，早于它起的 aplay 打不开设备即退；② DMA
    // 死锁——aplay 阻塞在 snd_pcm_writei，writeChunk poll 超时，必须
    // SIGKILL 才能解除。失败退避 1s 重试，期间入队丢弃。
    bool respawn() {
        killAplay();  // SIGKILL + waitpid：不阻塞，卡死的 aplay 也必死
        const std::vector<int16_t> silence(kChunkSamples, 0);
        while (running_.load() && !stopping_.load()) {
            if (spawnAplay()) {
                bool ok = true;
                for (int i = 0; i < kPrebufferChunks; ++i)
                    ok = writeChunk(silence) && ok;
                if (ok) {
                    stream_opened_at_ = std::chrono::steady_clock::now();
                    printf("[Sound] stream respawned: %s\n", device().c_str());
                    return true;
                }
                killAplay();  // aplay 起了但立刻死（设备未就绪），继续等
            }
            for (int w = 0; w < 10 && running_.load() && !stopping_.load(); ++w)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    void enqueue(std::vector<int16_t> buf) {
        if (buf.empty() || pipe_fd_ < 0) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (queue_.size() >= 4) queue_.pop_front();  // 防提示音堆积
        queue_.push_back(std::move(buf));
    }

    // playFile 解码输出整段入队（不参与 cap-4 淘汰，避免长音频被挤掉）
    void enqueueFull(std::vector<int16_t> buf) {
        if (buf.empty()) return;
        std::lock_guard<std::mutex> lk(mu_);
        queue_.push_back(std::move(buf));
    }

    // playFile 工作体：按扩展名提取 PCM 后整段注入常驻流队列。
    // wav 直接解析 + 重采样；压缩格式统一走 ffmpeg（24k stereo S16LE），
    // 缺 ffmpeg 时告警一次后忽略（mpg123 的重采样/声道 CLI 管道不可控，
    // 不再兜底）。解码在 detached 线程执行，避免阻塞渲染主线程
    void decodeFile(const std::string& path) {
        uint32_t rate = 0;
        uint16_t ch = 0;
        std::vector<int16_t> pcm = loadWavRaw(path, &rate, &ch);
        if (!pcm.empty()) {
            enqueueFull(resampleToContent(pcm, rate, ch));
            return;
        }
        // 非 16bit PCM wav 或压缩格式 → ffmpeg 解码
        if (!haveCmd("ffmpeg")) {
            if (!warn_once_) {
                warn_once_ = true;
                fprintf(stderr, "[Sound] no decoder (ffmpeg) for %s, skipped\n",
                        path.c_str());
            }
            return;
        }
        const std::string cmd = "ffmpeg -v quiet -i " + shellQuote(path) +
                                " -f s16le -ar " + std::to_string(kContentRate) +
                                " -ac 2 - 2>/dev/null";
        FILE* f = popen(cmd.c_str(), "r");
        if (!f) return;
        std::vector<int16_t> acc;
        int16_t buf[4096];
        size_t n;
        while ((n = fread(buf, sizeof(int16_t), 4096, f)) > 0)
            acc.insert(acc.end(), buf, buf + n);
        pclose(f);
        enqueueFull(std::move(acc));
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

void SoundPlayer::setVolume(int percent) {
    // 软件音量（0-100）：clamp 后原子写入，worker 下一块生效（≤50ms）
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    impl_->volume_.store(percent, std::memory_order_relaxed);
}

void SoundPlayer::play(Event ev) {
    printf("[Sound] play ev=%d\n", (int)ev);
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
        case Event::Welcome:
            impl_->enqueue(impl_->acquire("welcome.wav", buildWelcome));
            break;
    }
}

void SoundPlayer::playFile(const std::string& path) {
    if (path.empty()) return;
    // 解码/重采样可能耗时数十 ms 至数秒，detached 线程执行；PCM 注入常驻
    // 流队列后播放时序由流保证。主进程退出时析构 SoundPlayer，此线程若
    // 仍在解码会随进程终止（全局对象生命周期覆盖全部业务场景）
    std::thread([path, this] { impl_->decodeFile(path); }).detach();
}

// 唤醒应答前调用：请求 worker 线程 pclose→popen 强制重建 HDMI PCM 流
// （防 H616 间歇静音，见 run 循环头部消费点）
void SoundPlayer::requestRebuild() {
    impl_->rebuild_requested_.store(true);
}

}  // namespace dutyon

#endif  // !_WIN32
