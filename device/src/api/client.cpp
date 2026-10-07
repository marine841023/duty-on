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
static std::optional<PetStatus> FetchStatus(cpr::Session& session, int* http_code) {
    auto r = session.Get();
    if (http_code) *http_code = r.status_code;
    if (r.status_code != 200) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(r.text);
        PetStatus s;
        s.overall_state = j.value("overallState", "sleeping");
        s.active_character = j.value("activeCharacter", std::string{});
        s.device_mode = j.value("deviceMode", "multi");
        s.frame_source = j.value("frameSource", std::string{});
        s.clock_color = j.value("clockColor", "amber");
        s.device_brightness = j.value("deviceBrightness", 100);
        s.device_volume = j.value("deviceVolume", 80);
        s.screen_rotation = j.value("screenRotation", 0);
        s.flip_horizontal = j.value("flipHorizontal", false);
        s.server_time = j.value("serverTime", 0.0);
        s.utc_offset_min = j.value("utcOffset", 0);
        s.sound_mute = j.value("soundMute", false);
        s.welcome_seq = j.value("welcomeSeq", 0LL);
        // PC 下发的设备指令（目前仅"重新配网"）：收到即由 run() 自动回执，
        // main.cpp 消费 status 时比对序号执行（同号不重复执行）
        if (j.contains("deviceCmd") && j["deviceCmd"].is_object() &&
            j["deviceCmd"].value("type", std::string{}) == "reset-wifi")
            s.reset_wifi_cmd = j["deviceCmd"].value("id", std::string{});
        if (j.contains("activeAudio") && j["activeAudio"].is_object()) {
            for (auto it = j["activeAudio"].begin(); it != j["activeAudio"].end();
                 ++it)
                if (it.value().is_string())
                    s.active_audio[it.key()] = it.value().get<std::string>();
        }
        if (j.contains("soundMutedStates") && j["soundMutedStates"].is_array()) {
            for (const auto& v : j["soundMutedStates"])
                if (v.is_string())
                    s.sound_muted_states.push_back(v.get<std::string>());
        }
        // 状态动作覆盖：{状态: [组, 序号]}（PC「动作设定」下发）
        if (j.contains("stateMotions") && j["stateMotions"].is_object()) {
            for (auto it = j["stateMotions"].begin(); it != j["stateMotions"].end();
                 ++it) {
                const auto& v = it.value();
                if (v.is_array() && v.size() >= 2 && v[0].is_string() &&
                    v[1].is_number())
                    s.state_motions[it.key()] = {
                        v[0].get<std::string>(), v[1].get<int>()};
            }
        }

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

    // 目标地址（由 pc_discovery 发现动态更新；空 = 链路未建立）
    std::mutex url_mtx;
    std::string url;

    std::string snapshotUrl() {
        std::lock_guard<std::mutex> lk(url_mtx);
        return url;
    }

    // ---- 设备身份 + 配对（Wi-Fi 配对码方案）----
    std::mutex id_mtx;
    std::string device_id;
    std::string pair_code;
    bool identity_set = false;

    std::mutex tok_mtx;
    std::string token;          // 已附加到 session 请求头的令牌
    std::string pending_token;  // 配对成功待主线程取走持久化
    std::atomic<bool> has_token{false};

    // 本设备程序版本（附加到 X-DutyOn-Version 头；main 启动时读
    // /opt/dutyon/VERSION 注入，供 PC 端比对触发自动更新）
    std::mutex ver_mtx;
    std::string prog_ver;

    bool identityReady() {
        std::lock_guard<std::mutex> lk(id_mtx);
        return identity_set && !device_id.empty();
    }
    std::string snapshotToken() {
        std::lock_guard<std::mutex> lk(tok_mtx);
        return token;
    }
    std::string snapshotVersion() {
        std::lock_guard<std::mutex> lk(ver_mtx);
        return prog_ver;
    }
    std::string takePendingToken() {
        std::lock_guard<std::mutex> lk(tok_mtx);
        std::string t;
        t.swap(pending_token);
        return t;
    }
    void clearToken() {
        std::lock_guard<std::mutex> lk(tok_mtx);
        token.clear();
        has_token = false;
    }
    // POST /api/pair-request {deviceId, code}；PC 记 pending 或（用户已输码）
    // 直接签发 token。拿到 token 即置 has_token，后续请求自动带头。
    void tryPair(const std::string& base) {
        std::string did, code;
        {
            std::lock_guard<std::mutex> lk(id_mtx);
            did = device_id;
            code = pair_code;
        }
        if (did.empty()) return;
        try {
            auto r = cpr::Post(
                cpr::Url{base + "/api/pair-request"},
                cpr::Header{{"Content-Type", "application/json"}},
                cpr::Body{nlohmann::json{{"deviceId", did}, {"code", code}}.dump()},
                cpr::ConnectTimeout{1000}, cpr::Timeout{3000},
                cpr::Proxies{{"http", ""}, {"https", ""}});
            if (r.status_code != 200) return;
            auto j = nlohmann::json::parse(r.text, nullptr, /*allow_exceptions=*/false);
            if (j.is_discarded()) return;
            if (j.value("status", std::string{}) == "paired") {
                const std::string tok = j.value("token", std::string{});
                if (!tok.empty()) {
                    std::lock_guard<std::mutex> lk(tok_mtx);
                    token = tok;
                    pending_token = tok;
                    has_token = true;
                    printf("[ApiClient] paired, token acquired\n");
                }
            }
        } catch (...) {
        }
    }

    // POST /api/cmd-ack {id}：指令回执，PC 收到即清除挂起指令。失败不清：
    // PC 端指令还在，下轮 status 会重复下发 deviceCmd，本端 ack 幂等
    void ackDeviceCmd(const std::string& base, const std::string& cmd_id) {
        try {
            cpr::Header h{{"Content-Type", "application/json"}};
            if (const std::string tok = snapshotToken(); !tok.empty())
                h["X-DutyOn-Token"] = tok;
            cpr::Post(cpr::Url{base + "/api/cmd-ack"}, h,
                      cpr::Body{nlohmann::json{{"id", cmd_id}}.dump()},
                      cpr::ConnectTimeout{1000}, cpr::Timeout{3000},
                      cpr::Proxies{{"http", ""}, {"https", ""}});
        } catch (...) {
        }
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
        auto last_pair = Clock::now() - std::chrono::hours(1);
        std::string applied_url;     // 已应用到 session 的地址
        std::string applied_token;   // 已应用到 session 请求头的令牌
        std::string applied_version; // 已应用到 session 请求头的程序版本

        while (!stop.load()) {
            const std::string url = snapshotUrl();
            if (url.empty()) {
                // 链路未建立（未入网 / PC 未发现）：暂停请求等 setBaseUrl
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
            // 令牌 / 程序版本变化 -> 刷新两个 session 的请求头（未配对时无
            // token 头；未知版本时无 version 头）
            const std::string tok = snapshotToken();
            const std::string ver = snapshotVersion();
            if (tok != applied_token || ver != applied_version) {
                cpr::Header h;
                if (!tok.empty()) h["X-DutyOn-Token"] = tok;
                if (!ver.empty()) h["X-DutyOn-Version"] = ver;
                status_session.SetHeader(h);
                metrics_session.SetHeader(h);
                applied_token = tok;
                applied_version = ver;
            }
            auto now = Clock::now();

            // 未配对：周期尝试握手拿 token（需用户先在 PC 端输入屏幕配对码）；
            // 未配对不轮询业务数据（PC 对未配对请求一律 401）
            if (!has_token.load()) {
                if (identityReady() &&
                    now - last_pair >= std::chrono::milliseconds(3000)) {
                    last_pair = now;
                    tryPair(url);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            if (now - last_status >= std::chrono::milliseconds(kPollIntervalMs)) {
                last_status = now;
                int code = 0;
                if (auto s = FetchStatus(status_session, &code)) {
                    // 收到"重新配网"指令：先回执再入缓存（PC 清挂起；执行
                    // 由主线程消费 status 时触发，断网重进配网不影响本线程）
                    if (!s->reset_wifi_cmd.empty())
                        ackDeviceCmd(url, s->reset_wifi_cmd);
                    std::lock_guard<std::mutex> lk(mtx);
                    status = *s;
                    status_seq++;
                } else if (code == 401) {
                    // PC 侧已解除配对 / 令牌失效：丢弃令牌，下轮重新握手
                    printf("[ApiClient] 401, token rejected -> re-pair\n");
                    clearToken();
                    applied_token.clear();
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

void ApiClient::setIdentity(const std::string& device_id,
                            const std::string& pair_code,
                            const std::string& existing_token) {
    {
        std::lock_guard<std::mutex> lk(impl_->id_mtx);
        impl_->device_id = device_id;
        impl_->pair_code = pair_code;
        impl_->identity_set = true;
    }
    if (!existing_token.empty()) {
        // 已配对（上次持久化的 token）：直接用，不再握手
        std::lock_guard<std::mutex> lk(impl_->tok_mtx);
        impl_->token = existing_token;
        impl_->has_token = true;
        printf("[ApiClient] resume with saved token\n");
    }
}

void ApiClient::setProgramVersion(const std::string& version) {
    std::lock_guard<std::mutex> lk(impl_->ver_mtx);
    impl_->prog_ver = version;
}

std::string ApiClient::takePairToken() { return impl_->takePendingToken(); }

bool ApiClient::paired() const { return impl_->has_token.load(); }

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
        cpr::Header h;  // 已配对令牌（PC 对 /api/* 非回环请求门控）
        if (const std::string tok = impl_->snapshotToken(); !tok.empty())
            h["X-DutyOn-Token"] = tok;
        auto r = cpr::Get(cpr::Url{base + "/api/character"}, h,
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
        cpr::Header h;  // 已配对令牌（PC 对 /api/* 非回环请求门控）
        if (const std::string tok = impl_->snapshotToken(); !tok.empty())
            h["X-DutyOn-Token"] = tok;
        auto r = cpr::Get(cpr::Url{base + "/api/animations/" + file_name}, h,
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

bool ApiClient::fetchFramePhoto(std::vector<unsigned char>& out_bytes) {
    out_bytes.clear();
    const std::string base = impl_->snapshotUrl();
    if (base.empty()) return false;
    try {
        cpr::Header h;  // 已配对令牌（PC 对 /api/* 非回环请求门控）
        if (const std::string tok = impl_->snapshotToken(); !tok.empty())
            h["X-DutyOn-Token"] = tok;
        auto r = cpr::Get(cpr::Url{base + "/api/frame/photo?max_side=1024"}, h,
                          cpr::ConnectTimeout{2000}, cpr::Timeout{20000},
                          cpr::Proxies{{"http", ""}, {"https", ""}});
        if (r.status_code != 200 || r.text.empty()) return false;
        out_bytes.assign(r.text.begin(), r.text.end());
        return true;
    } catch (...) {
        out_bytes.clear();
        return false;
    }
}

// URL 路径逐字节百分号编码（'/' 分段保留；中文/空格目录名必须编码，
// 否则请求行非法。PC 端 httplib 路由前 decode_url 还原）
static std::string UrlEncodePath(const std::string& path) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (const unsigned char c : path) {
        const bool unreserved =
            (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~';
        if (unreserved || c == '/') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

bool ApiClient::downloadLive2dFile(const std::string& rel_path,
                                   const std::string& save_path) {
    const std::string base = impl_->snapshotUrl();
    if (base.empty() || rel_path.empty() || save_path.empty()) return false;
    try {
        cpr::Header h;  // 已配对令牌（PC 对 /live2d/* 非回环请求门控）
        if (const std::string tok = impl_->snapshotToken(); !tok.empty())
            h["X-DutyOn-Token"] = tok;
        auto r = cpr::Get(cpr::Url{base + "/live2d/" + UrlEncodePath(rel_path)}, h,
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
