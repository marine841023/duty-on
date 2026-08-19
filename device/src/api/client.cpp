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

} // namespace dutyon
