// 后端服务聚合实现 —— lib.rs setup 流程 + spawn_timers + spawn_ide_scanner
// 的 C++ 移植（仅 Windows）。

#ifdef _WIN32

#include "backend/backend_service.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "backend/autostart.h"
#include "backend/backend_config.h"
#include "backend/hooks_installer.h"
#include "backend/ide_scanner.h"

namespace dutyon::backend {

namespace fs = std::filesystem;

namespace {

std::string homeDir() {
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
    return home ? std::string(home) : std::string();
}

// 扫描器诊断日志（detected 集变化时落盘 —— "IDE 明明开着宠物却丢了"
// 类报告的关键线索）
void appendScannerLog(const std::string& msg) {
    const std::string home = homeDir();
    if (home.empty()) return;
    const fs::path path = fs::path(home) / ".dutyon" / "frontend.log";
    std::error_code ec;
    if (fs::exists(path) && fs::file_size(path, ec) > 512 * 1024) fs::remove(path, ec);
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::app);
    if (!out) return;
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
    out << "[" << secs << "][info] " << msg << "\n";
}

std::string joinDetected(const std::vector<DetectedProject>& names) {
    std::string out = "[";
    for (size_t i = 0; i < names.size(); i++) {
        if (i) out += ", ";
        out += names[i].name + "(" + ideKindStr(names[i].ide) + ")";
    }
    return out + "]";
}

std::string joinSuspects(const std::vector<std::string>& suspects) {
    std::string out = "[";
    for (size_t i = 0; i < suspects.size(); i++) {
        if (i) out += ", ";
        out += suspects[i];
    }
    return out + "]";
}

} // namespace

BackendService::~BackendService() { stop(); }

void BackendService::start() {
    bool expected = false;
    if (!run_.compare_exchange_strong(expected, true)) return;  // 已启动

    // HTTP 监听（端口被占 = 旧实例并存：宠物照常跑，仅没有 hook 接收）
    http_ = std::make_unique<HttpServer>(sm_, monitor_);
    http_->setQuitHandler([this] { requestQuit(); });
    if (!http_->start()) {
        fprintf(stderr,
                "[Backend] HTTP 端口被占，hook 事件不可用（另一实例在运行？）\n");
    }

    monitor_.start();
    scanner_thread_ = std::thread(&BackendService::runScanner, this);
    timer_thread_ = std::thread(&BackendService::runTimers, this);
}

void BackendService::stop() {
    run_ = false;
    if (scanner_thread_.joinable()) scanner_thread_.join();
    if (timer_thread_.joinable()) timer_thread_.join();
    monitor_.stop();
    if (http_) {
        http_->stop();
        http_.reset();
    }
}

std::optional<dutyon::PetStatus> BackendService::takeStatus() {
    const uint64_t v = sm_.version();
    if (v == status_consumer_version_) return std::nullopt;
    status_consumer_version_ = v;
    return sm_.petStatus();
}

std::optional<dutyon::SysMetrics> BackendService::takeMetrics() {
    return monitor_.takeMetrics();
}

int BackendService::getAutostart() { return autostartEnabled() ? 1 : 0; }

bool BackendService::setAutostart(bool enable) { return setAutostartEnabled(enable); }

std::string BackendService::getHooks() { return isHooksInstalled().toJson().dump(); }

std::string BackendService::installHooks() {
    // 注意限定：与成员函数同名，不加限定会无限递归
    return dutyon::backend::installHooks(resolveHooksSourceDir()).toJson().dump();
}

bool BackendService::bringToFront(const std::string& target) {
    // target 可能是完整路径（取末段文件夹名）或直接是项目名
    const size_t slash = target.find_last_of("\\/");
    const std::string name = slash == std::string::npos ? target : target.substr(slash + 1);
    return focusProjectWindow(name);
}

void BackendService::runScanner() {
    std::vector<std::string> last_names;
    while (run_) {
        // 窗口扫描 + CLI 存活探测一次完成（都是 OS 调用，放本线程不阻塞别人）
        auto [detected, suspects] = scanIdeProjects();
        const CliLiveness liveness = scanCliProcesses();

        std::vector<std::string> name_list;
        name_list.reserve(detected.size());
        for (const auto& d : detected) name_list.push_back(d.name + "(" + ideKindStr(d.ide) + ")");
        if (name_list != last_names) {
            appendScannerLog("[scanner] detected=" + joinDetected(detected) +
                             " suspects=" + joinSuspects(suspects));
            last_names = std::move(name_list);
        }

        sm_.syncDetectedWindows(detected);
        sm_.syncCliLiveness(liveness);
        const uint64_t delay = sm_.sessionCount() > 0 ? bc::kScanIntervalActiveMs
                                                      : bc::kScanIntervalIdleMs;
        // 分片 sleep：stop() 时最多 500ms 内退出
        for (uint64_t slept = 0; run_ && slept < delay; slept += 500) {
            std::this_thread::sleep_for(std::chrono::milliseconds(
                (std::min<uint64_t>)(500, delay - slept)));
        }
    }
}

void BackendService::runTimers() {
    // 合并两个 Rust 定时器：30s 超时清理 + 60s alert 重提醒（每 2 tick 一次）
    uint64_t tick = 0;
    while (run_) {
        for (int i = 0; i < 60 && run_; i++) {  // 60 × 500ms = 30s
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!run_) break;
        tick++;
        sm_.cleanupStaleSessions();
        if (tick % 2 == 0) sm_.checkAndRemindAlert();
    }
}

} // namespace dutyon::backend

#endif // _WIN32
