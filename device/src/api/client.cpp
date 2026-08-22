#include "api/client.h"
#include "config.h"

#include <atomic>
#include <chrono>
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

    // 主线程与工作线程共享的缓存（seq 防止重复消费同一条数据）
    std::mutex mtx;
    PetStatus status{};
    SysMetrics metrics{};
    uint64_t status_seq = 0;
    uint64_t metrics_seq = 0;
    uint64_t status_consumed = 0;
    uint64_t metrics_consumed = 0;

    void run(const std::string& base_url) {
        using Clock = std::chrono::steady_clock;
        // 首轮立即拉一次；此后状态 500ms / 监控 1500ms（与 Rust 采样 1.5s 错开）
        auto last_status = Clock::now() - std::chrono::hours(1);
        auto last_metrics = Clock::now() - std::chrono::hours(1);

        while (!stop.load()) {
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

ApiClient::ApiClient(const std::string& base_url)
    : base_url_(base_url), impl_(new Impl()) {
    impl_->status_session.SetUrl(cpr::Url{base_url_ + "/api/status"});
    impl_->metrics_session.SetUrl(cpr::Url{base_url_ + "/api/metrics"});

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

    impl_->worker = std::thread(&Impl::run, impl_, base_url_);
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
    try {
        auto r = cpr::Post(
            cpr::Url{base_url_ + "/api/bring-to-front"},
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
    try {
        auto r = cpr::Get(cpr::Url{base_url_ + "/api/hooks"},
                          cpr::ConnectTimeout{1000}, cpr::Timeout{3000},
                          cpr::Proxies{{"http", ""}, {"https", ""}});
        return r.status_code == 200 ? r.text : std::string{};
    } catch (...) {
        return {};
    }
}

std::string ApiClient::installHooks() {
    try {
        auto r = cpr::Post(cpr::Url{base_url_ + "/api/hooks/install"},
                           cpr::ConnectTimeout{1000}, cpr::Timeout{15000},
                           cpr::Proxies{{"http", ""}, {"https", ""}});
        return r.status_code == 200 ? r.text : std::string{};
    } catch (...) {
        return {};
    }
}

bool ApiClient::quitApp() {
    try {
        auto r = cpr::Post(cpr::Url{base_url_ + "/api/quit"},
                           cpr::ConnectTimeout{1000}, cpr::Timeout{3000},
                           cpr::Proxies{{"http", ""}, {"https", ""}});
        return r.status_code == 200;
    } catch (...) {
        return false;
    }
}

int ApiClient::getAutostart() {
    try {
        auto r = cpr::Get(cpr::Url{base_url_ + "/api/autostart"},
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
    try {
        auto r = cpr::Post(
            cpr::Url{base_url_ + "/api/autostart"},
            cpr::Header{{"Content-Type", "application/json"}},
            cpr::Body{nlohmann::json{{"enabled", enable}}.dump()},
            cpr::ConnectTimeout{1000}, cpr::Timeout{3000},
            cpr::Proxies{{"http", ""}, {"https", ""}});
        return r.status_code == 200;
    } catch (...) {
        return false;
    }
}

} // namespace dutyon
