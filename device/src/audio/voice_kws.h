#pragma once

// 设备端语音唤醒/指令监听（仅 ARM Linux）。
//
// v4.2 子进程方案：fork/execvp 启动 sherpa-onnx-vad-alsa-offline-asr
// （静态可执行，随资产部署到 /opt/dutyon/assets/kws/asr-offline），
// silero VAD 断句 + SenseVoice-small int8 非流式高精度识别全在子进程内
// 完成；本类拉起子进程、逐行读其输出——每个 VAD 语音段一行
// "段号: 转写文本"（段级一次性输出，无 partial），经拼音模糊匹配成事件
// （"DutyOn"/指令名）放入线程安全队列，主循环每帧 takeKeyword() 轮询
// （仿 ApiClient take* 模式）。
//
// 为何不用 popen：stop() 需要 kill 子进程 PID 解除读管道阻塞（popen
// 不暴露 PID，且 pclose 会阻塞等子进程退出），故 fork/execv 直控，
// stdout/stderr 在子进程内 dup2 合流（等价原 shell 2>&1）。
//
// 生命周期：收音随设备"语音交互模式"启停（start/stop 幂等、可反复）。
// 降级：可执行/模型/字典缺失时不启动；麦克风等运行期故障由线程内
// 指数退避重启兜底（上限 30s），期间 available()=false，其余功能不受影响。

#ifndef _WIN32

#include <sys/types.h>  // pid_t

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

namespace dutyon {

class VoiceKws {
public:
    VoiceKws() = default;
    ~VoiceKws();

    VoiceKws(const VoiceKws&) = delete;
    VoiceKws& operator=(const VoiceKws&) = delete;

    // 启动监听线程（子进程在后台加载模型，不阻塞调用方）。幂等。
    // 资产缺失时返回 false 并不再重试；麦克风等运行期故障由线程内退避重启兜底。
    bool start();

    // 停止收音：杀子进程 + 停线程 + 清空事件队列。幂等；之后可再 start()。
    void stop();

    // 主循环轮询：返回最近命中的事件（"DutyOn"=唤醒 / 指令中文名，由
    // voice_match 拼音模糊匹配产生），无事件返回空串。一次只取一条。
    std::string takeKeyword();

    // 子进程存活且资产齐备（可响应唤醒）。
    bool available() const { return available_.load(); }

private:
    void run();
    bool assetsReady() const;
    void pushEvent(const std::string& kw);
    // 一个 VAD 语音段文本 -> 拼音匹配事件（仅 worker 线程）
    void handleSegment(const std::string& text);

    std::atomic<bool> running_{false};
    std::atomic<bool> available_{false};
    std::atomic<pid_t> pid_{-1};  // 当前子进程（-1 无）；worker 内写、stop 读
    std::thread worker_;

    std::mutex mu_;
    std::deque<std::string> events_;  // 命中队列（上限防堆积）
};

}  // namespace dutyon

#endif  // !_WIN32
