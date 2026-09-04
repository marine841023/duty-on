#include "api/client.h"
#include "config.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

namespace dutyon {

// ---------------------------------------------------------------------------
// 单次请求（同步）；供后台线程调用，绝不在主渲染线程执行
// ---------------------------------------------------------------------------
static std::optional<PetStatus> FetchStatus(cpr::Session& session) {
    auto r = session.Get();
    if (r.status_code != 200) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(r.text);
        PetStatus s;
        s.overall_state = j.value("overallState", "sleeping");
        s.active_character = j.value("activeCharacter", std::string{});
        s.device_mode = j.value("deviceMode", "multi");
        s.server_time = j.value("serverTime", 0.0);
        s.utc_offset_min = j.value("utcOffset", 0);

        if (j.contains("sessions") && j["sessions"].is_array()) {
            s.session_count = static_cast<int>(j["sessions"].size());
            for (const auto& sess : j["sessions"]) {
                SessionInfo si;
                si.project_name = sess.value("projectName", std::string{});
                si.status = sess.value("status", "idle");
                si.ide = sess.value("ide", std::string{});
                if (sess.contains("alertMessage") && !sess["alertMessage"].is_null()) {
                    si.alert_message = sess.value("alertMessage", std::string{});
                }
                if (si.status == "confirmation-needed") s.has_confirmation = true;
                s.sessions.push_back(std::move(si));
            }
        }
        return s;
    } catch (...) {
        return std::nullopt;
    }
}

static std::optional<SysMetrics> FetchMetrics(cpr::Session& session) {
    auto r = session.Get();
    if (r.status_code != 200) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(r.text);
        SysMetrics m;
        m.cpu_usage = j.value("cpuUsage", 0.0f);
        m.mem_total = j.value("memTotal", 0ULL);
        m.mem_used = j.value("memUsed", 0ULL);
        if (!j["gpuUsage"].is_null()) {
            m.has_gpu = true;
            m.gpu_usage = j.value("gpuUsage", 0.0f);
            m.gpu_name = j.value("gpuName", std::string{});
            m.vram_total = j.value("vramTotal", 0ULL);
            m.vram_used = j.value("vramUsed", 0ULL);
        }
        m.net_rx_rate = j.value("netRxRate", 0ULL);
        m.net_tx_rate = j.value("netTxRate", 0ULL);
        m.self_cpu = j.value("selfCpu", 0.0f);
        m.self_mem = j.value("selfMem", 0ULL);
        return m;
    } catch (...) {
        return std::nullopt;
    }
}

struct ApiClient::Impl {
    cpr::Session status_session;   // 状态轮询专用连接（keep-alive）
    cpr::Session metrics_session;  // 监控轮询专用连接

    std::thread worker;
    std::atomic<bool> stop{false};

    // 目标地址（USB 直连下由租约发现动态更新；空 = 链路未建立）
    std::mutex url_mtx;
    std::string url;

    std::string snapshotUrl() {
        std::lock_guard<std::mutex> lk(url_mtx);
        return url;
    }

    // 主线程与工作线程共享的缓存（seq 防止重复消费同一条数据）
    std::mutex mtx;
    PetStatus status{};
    SysMetrics metrics{};
    uint64_t status_seq = 0;
    uint64_t metrics_seq = 0;
    uint64_t status_consumed = 0;
    uint64_t metrics_consumed = 0;

    void run() {
        using Clock = std::chrono::steady_clock;
        // 首轮立即拉一次；此后状态 500ms / 监控 1500ms（与 Rust 采样 1.5s 错开）
        auto last_status = Clock::now() - std::chrono::hours(1);
        auto last_metrics = Clock::now() - std::chrono::hours(1);
        std::string applied_url;  // 已应用到 session 的地址

        while (!stop.load()) {
            const std::string url = snapshotUrl();
            if (url.empty()) {
                // 链路未建立（未插 USB / DHCP 未完成）：暂停请求等 setBaseUrl
                applied_url.clear();
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (url != applied_url) {
                // 首次连接或地址变化（DHCP 重新分配）：重设端点并立即拉一轮
                status_session.SetUrl(cpr::Url{url + "/api/status"});
                metrics_session.SetUrl(cpr::Url{url + "/api/metrics"});
                applied_url = url;
                last_status = Clock::now() - std::chrono::hours(1);
                last_metrics = Clock::now() - std::chrono::hours(1);
            }
            auto now = Clock::now();
            if (now - last_status >= std::chrono::milliseconds(kPollIntervalMs)) {
                last_status = now;
                if (auto s = FetchStatus(status_session)) {
                    std::lock_guard<std::mutex> lk(mtx);
                    status = *s;
                    status_seq++;
                }
            }
            if (now - last_metrics >= std::chrono::milliseconds(1500)) {
                last_metrics = now;
                if (auto m = FetchMetrics(metrics_session)) {
                    std::lock_guard<std::mutex> lk(mtx);
                    metrics = *m;
                    metrics_seq++;
                }
            }
            // 100ms 粒度睡眠：退出响应快，节拍误差可忽略
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
};

ApiClient::ApiClient(const std::string& base_url) : impl_(new Impl()) {
    impl_->url = base_url;  // 可为空（未连接）；构造期单线程无需加锁

    // 连接本机回环地址，显式禁用代理——系统开了代理（Clash 等）时，
    // libcurl 会读 http_proxy 环境变量把 127.0.0.1 的请求也送进代理，
    // 单次请求可能拖到几百毫秒甚至超时
    const cpr::Proxies no_proxy{{"http", ""}, {"https", ""}};
    impl_->status_session.SetProxies(no_proxy);
    impl_->metrics_session.SetProxies(no_proxy);

    impl_->status_session.SetConnectTimeout(cpr::ConnectTimeout{1000});
    impl_->status_session.SetTimeout(cpr::Timeout{2000});
    impl_->metrics_session.SetConnectTimeout(cpr::ConnectTimeout{1000});
    impl_->metrics_session.SetTimeout(cpr::Timeout{2000});

    impl_->worker = std::thread(&Impl::run, impl_);
}

void ApiClient::setBaseUrl(const std::string& url) {
    std::lock_guard<std::mutex> lk(impl_->url_mtx);
    if (impl_->url != url) {
        impl_->url = url;
        printf("[ApiClient] base url -> %s\n",
               url.empty() ? "(link down)" : url.c_str());
    }
}

ApiClient::~ApiClient() {
    impl_->stop.store(true);
    if (impl_->worker.joinable()) impl_->worker.join();
    delete impl_;
}

std::optional<PetStatus> ApiClient::takeStatus() {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (impl_->status_seq != impl_->status_consumed) {
        impl_->status_consumed = impl_->status_seq;
        return impl_->status;
    }
    return std::nullopt;
}

std::optional<SysMetrics> ApiClient::takeMetrics() {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    if (impl_->metrics_seq != impl_->metrics_consumed) {
        impl_->metrics_consumed = impl_->metrics_seq;
        return impl_->metrics;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// 一次性动作：菜单点击触发，同步执行（短超时，卡一下 UI 可接受）
// ---------------------------------------------------------------------------

bool ApiClient::bringToFront(const std::string& target) {
    const std::string base = impl_->snapshotUrl();
    if (base.empty()) return false;
    try {
        auto r = cpr::Post(
            cpr::Url{base + "/api/bring-to-front"},
            cpr::Header{{"Content-Type", "application/json"}},
            cpr::Body{nlohmann::json{{"target", target}}.dump()},
            cpr::ConnectTimeout{1000}, cpr::Timeout{3000},
            cpr::Proxies{{"http", ""}, {"https", ""}});
        return r.status_code == 200;
    } catch (...) {
        return false;
    }
}

std::string ApiClient::getHooks() {
    const std::string base = impl_->snapshotUrl();
    if (base.empty()) return {};
    try {
        auto r = cpr::Get(cpr::Url{base + "/api/hooks"},
                          cpr::ConnectTimeout{1000}, cpr::Timeout{3000},
                          cpr::Proxies{{"http", ""}, {"https", ""}});
        return r.status_code == 200 ? r.text : std::string{};
    } catch (...) {
        return {};
    }
}

std::string ApiClient::installHooks() {
    const std::string base = impl_->snapshotUrl();
    if (base.empty()) return {};
    try {
        auto r = cpr::Post(cpr::Url{base + "/api/hooks/install"},
                           cpr::ConnectTimeout{1000}, cpr::Timeout{15000},
                           cpr::Proxies{{"http", ""}, {"https", ""}});
        return r.status_code == 200 ? r.text : std::string{};
    } catch (...) {
        return {};
    }
}

bool ApiClient::quitApp() {
    const std::string base = impl_->snapshotUrl();
    if (base.empty()) return false;
    try {
        auto r = cpr::Post(cpr::Url{base + "/api/quit"},
                           cpr::ConnectTimeout{1000}, cpr::Timeout{3000},
                           cpr::Proxies{{"http", ""}, {"https", ""}});
        return r.status_code == 200;
    } catch (...) {
        return false;
    }
}

int ApiClient::getAutostart() {
    const std::string base = impl_->snapshotUrl();
    if (base.empty()) return -1;
    try {
        auto r = cpr::Get(cpr::Url{base + "/api/autostart"},
                          cpr::ConnectTimeout{1000}, cpr::Timeout{3000},
                          cpr::Proxies{{"http", ""}, {"https", ""}});
        if (r.status_code != 200) return -1;
        auto j = nlohmann::json::parse(r.text);
        return j.value("enabled", false) ? 1 : 0;
    } catch (...) {
        return -1;
    }
}

bool ApiClient::setAutostart(bool enable) {
    const std::string base = impl_->snapshotUrl();
    if (base.empty()) return false;
    try {
        auto r = cpr::Post(
            cpr::Url{base + "/api/autostart"},
            cpr::Header{{"Content-Type", "application/json"}},
            cpr::Body{nlohmann::json{{"enabled", enable}}.dump()},
            cpr::ConnectTimeout{1000}, cpr::Timeout{3000},
            cpr::Proxies{{"http", ""}, {"https", ""}});
        return r.status_code == 200;
    } catch (...) {
        return false;
    }
}

CustomCharacter ApiClient::fetchCharacter(const std::string& expect_id) {
    CustomCharacter out;  // 失败保持空 id，调用方据此判别
    const std::string base = impl_->snapshotUrl();
    if (base.empty() || expect_id.empty()) return out;
    try {
        auto r = cpr::Get(cpr::Url{base + "/api/character"},
                          cpr::ConnectTimeout{1000}, cpr::Timeout{3000},
                          cpr::Proxies{{"http", ""}, {"https", ""}});
        if (r.status_code != 200) return out;
        auto j = nlohmann::json::parse(r.text);
        if (j.value("type", "") != "custom") return out;
        const std::string id = j.value("id", std::string{});
        // PC 在两次轮询之间又切换了角色：本次放弃，下一轮 status 会重试
        if (id != expect_id) return out;
        out.id = id;
        out.name = j.value("name", std::string{});
        out.sleeping = j.value("sleeping", std::string{});
        out.working = j.value("working", std::string{});
        out.alert = j.value("alert", std::string{});
    } catch (...) {
        out = CustomCharacter{};
    }
    return out;
}

bool ApiClient::downloadAnimation(const std::string& file_name,
                                   const std::string& save_path) {
    const std::string base = impl_->snapshotUrl();
    if (base.empty() || file_name.empty() || save_path.empty()) return false;
    try {
        auto r = cpr::Get(cpr::Url{base + "/api/animations/" + file_name},
                          cpr::ConnectTimeout{2000}, cpr::Timeout{30000},
                          cpr::Proxies{{"http", ""}, {"https", ""}});
        if (r.status_code != 200 || r.text.empty()) return false;
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(save_path).parent_path(), ec);
        std::ofstream out(save_path, std::ios::binary);
        if (!out) return false;
        out.write(r.text.data(), (std::streamsize)r.text.size());
        return out.good();
    } catch (...) {
        return false;
    }
}

} // namespace dutyon
