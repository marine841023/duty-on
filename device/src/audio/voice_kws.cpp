#ifndef _WIN32  // 仅设备端（ARM Linux）

#include "audio/voice_kws.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "config.h"
#include "audio/voice_match.h"

namespace dutyon {

namespace {

// 事件队列上限：主循环每帧都会取，正常远不会积压；异常风暴时丢最旧的
constexpr size_t kMaxEvents = 8;

// 子进程死亡后的重启退避：1s 起步，翻倍至上限
constexpr int kRetryMinMs = 1000;
constexpr int kRetryMaxMs = 30000;

}  // namespace

VoiceKws::~VoiceKws() {
    stop();
}

bool VoiceKws::assetsReady() const {
    const std::string dir = kKwsDir;
    for (const char* f :
         {"asr-offline", "sense-voice/model.int8.onnx", "sense-voice/tokens.txt", "silero_vad.onnx", "pinyin.txt"}) {
        if (std::fopen((dir + f).c_str(), "rb") == nullptr) return false;
    }
    return true;
}

bool VoiceKws::start() {
    if (running_.load()) return true;
    if (!assetsReady()) {
        std::fprintf(stderr, "[Voice] asr assets missing under %s, voice mode disabled\n", kKwsDir);
        return false;
    }
    // 全量拼音字典（匹配层基础，见 voice_match.h）；失败则禁用语音模式
    const size_t n = voiceLoadPinyinDict(std::string(kKwsDir) + "pinyin.txt");
    if (n == 0) {
        std::fprintf(stderr, "[Voice] pinyin dict load failed, voice mode disabled\n");
        return false;
    }
    std::fprintf(stderr, "[Voice] pinyin dict: %zu chars\n", n);
    running_ = true;
    worker_ = std::thread([this] { run(); });
    return true;
}

void VoiceKws::stop() {
    if (!running_.exchange(false)) return;  // 未在运行（幂等）
    // 杀子进程解除 worker 在 fgets 的阻塞；worker 侧也有补刀（stop 时
    // 恰逢刚 fork 的竞态窗口），双保险后 join 不会挂死
    const pid_t pid = pid_.load();
    if (pid > 0) ::kill(pid, SIGTERM);
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lk(mu_);
    events_.clear();
    std::fprintf(stderr, "[Voice] asr stopped\n");
}

void VoiceKws::run() {
    // 麦克风设备名可被环境变量覆盖（联调换麦不改代码）
    const char* micEnv = std::getenv("DUTYON_MICDEV");
    const std::string mic = micEnv && *micEnv ? micEnv : kMicDevice;

    // 引擎：sherpa-onnx-vad-alsa-offline-asr（VAD 断句 + 非流式高精度识别）。
    // v4.2 定稿参数（设备实测 2026-10-06）：
    //   SenseVoice-small int8（228MB）：转写整句流畅，唤醒词转写为"在吗coco"
    //   （coco 由匹配层英文特例兜底为 kou-kou）；language=zh 锁中文防跨语言漂移
    //   silero-vad threshold 0.35（默认 0.5 实测大量漏段）、断句静音 0.5s
    //   （原 0.7 偏保守，指令/唤醒均为短促语音，0.5 足够断句且响应快 0.2s）
    //   4 线程（原 2：实测服务全 cgroup 平均仅 ~0.45 核，推理突发占用
    //   4 核不会与渲染争抢太久；推理耗时约减 40%，响应明显更跟手）
    //   稳态 RSS ~350MB（加载峰值 527MB 后回落，线程数不影响内存）
    const std::string dir = kKwsDir;
    const std::vector<std::string> args = {
        dir + "asr-offline",
        "--silero-vad-model=" + dir + "silero_vad.onnx",
        "--silero-vad-threshold=0.35",
        "--silero-vad-min-silence-duration=0.5",
        "--sense-voice-model=" + dir + "sense-voice/model.int8.onnx",
        "--sense-voice-language=zh",
        "--tokens=" + dir + "sense-voice/tokens.txt",
        "--num-threads=4",
        mic,
    };

    int retryMs = kRetryMinMs;
    while (running_.load()) {
        std::fprintf(stderr, "[Voice] starting asr-offline (mic=%s)\n", mic.c_str());
        // argv 在 fork 前构建（fork 后子进程内不能 malloc，见下）
        std::vector<char*> argv;
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        int outfd[2];
        if (::pipe(outfd) != 0) {
            std::fprintf(stderr, "[Voice] pipe failed\n");
        } else {
            const pid_t pid = ::fork();
            if (pid < 0) {
                std::fprintf(stderr, "[Voice] fork failed\n");
                ::close(outfd[0]);
                ::close(outfd[1]);
            } else if (pid == 0) {
                // 子进程：stdout/stderr 合流进管道（等价 shell 2>&1），
                // 随后 exec——中间只用 async-signal-safe 调用
                ::dup2(outfd[1], STDOUT_FILENO);
                ::dup2(outfd[1], STDERR_FILENO);
                ::close(outfd[0]);
                ::close(outfd[1]);
                ::execv(argv[0], argv.data());
                _exit(127);  // exec 失败
            } else {
                ::close(outfd[1]);
                pid_ = pid;
                available_ = true;
                retryMs = kRetryMinMs;  // 存活过则重置退避
                FILE* pipe = ::fdopen(outfd[0], "r");
                // v4.2 输出协议（实测）：每个 VAD 语音段一行 "段号: 转写文本"，
                // 段级一次性输出（无 partial 刷新）；banner 配置行不含该前缀，
                // 自然被过滤
                char line[16384];
                while (running_.load() && pipe &&
                       std::fgets(line, sizeof(line), pipe)) {
                    char* p = line;
                    while (*p == ' ' || *p == '\t') ++p;
                    if (*p < '0' || *p > '9') continue;  // banner / 状态行
                    char* q = p;
                    while (*q >= '0' && *q <= '9') ++q;
                    if (*q != ':') continue;
                    std::string text(q + 1);
                    while (!text.empty() &&
                           (text.back() == '\n' || text.back() == '\r'))
                        text.pop_back();
                    if (!text.empty()) handleSegment(text);
                }
                if (pipe) ::fclose(pipe);
                // 停机补刀：stop() 恰在 fork 后、pid_ 写入前杀了个寂寞，
                // 由这里补杀；正常运行期子进程已死则 kill 无害（ESRCH）
                if (!running_.load() && pid_.load() > 0)
                    ::kill(pid_.load(), SIGTERM);
                ::waitpid(pid, nullptr, 0);  // 收尸防僵尸
                pid_ = -1;
                available_ = false;
                if (running_.load())
                    std::fprintf(stderr, "[Voice] asr-offline exited\n");
            }
        }

        // 退避等待（可被 stop 打断）
        for (int waited = 0; waited < retryMs && running_.load(); waited += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        retryMs = retryMs * 2 > kRetryMaxMs ? kRetryMaxMs : retryMs * 2;
    }
}

void VoiceKws::handleSegment(const std::string& text) {
    // v4.2：VAD 段级输出天然一句一段，无 partial 重复，无需流式去重；
    // 重复说同一指令 = 多段 = 多次事件（播放中打断重播，符合预期）
    std::string matched;
    const std::string hit = voiceMatchText(text, true, &matched);
    std::fprintf(stderr, "[Voice] asr: %s\n", text.c_str());
    if (!hit.empty()) {
        std::fprintf(stderr, "[Voice] hit: %s (asr: %s)\n", hit.c_str(),
                     text.c_str());
        pushEvent(hit);
    }
}

void VoiceKws::pushEvent(const std::string& kw) {
    std::lock_guard<std::mutex> lk(mu_);
    if (events_.size() >= kMaxEvents) events_.pop_front();
    events_.push_back(kw);
}

std::string VoiceKws::takeKeyword() {
    std::lock_guard<std::mutex> lk(mu_);
    if (events_.empty()) return {};
    std::string kw = std::move(events_.front());
    events_.pop_front();
    return kw;
}

}  // namespace dutyon

#endif  // !_WIN32
