#include "api/client.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

namespace dutyon {

struct ApiClient::Impl {
    cpr::Session session;
};

ApiClient::ApiClient(const std::string& base_url)
    : base_url_(base_url), impl_(new Impl()) {
    impl_->session.SetUrl(cpr::Url{base_url_ + "/api/status"});
    impl_->session.SetTimeout(cpr::Timeout{2000});
}

ApiClient::~ApiClient() { delete impl_; }

std::optional<PetStatus> ApiClient::poll() {
    auto r = impl_->session.Get();
    if (r.status_code != 200) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(r.text);
        PetStatus s;
        s.overall_state = j.value("overallState", "sleeping");

        if (j.contains("sessions") && j["sessions"].is_array()) {
            s.session_count = static_cast<int>(j["sessions"].size());
            for (const auto& sess : j["sessions"]) {
                if (sess.value("status", "") == "confirmation-needed") {
                    s.has_confirmation = true;
                    break;
                }
            }
        }
        return s;
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<SysMetrics> ApiClient::pollMetrics() {
    cpr::Session session;
    session.SetUrl(cpr::Url{base_url_ + "/api/metrics"});
    session.SetTimeout(cpr::Timeout{2000});
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

} // namespace dutyon
