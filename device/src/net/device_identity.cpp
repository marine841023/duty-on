#ifndef _WIN32  // 仅设备端（ARM Linux）

#include "net/device_identity.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace dutyon {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path identityPath() {
    const char* home = getenv("HOME");
    fs::path base = home ? fs::path(home) : fs::path("/root");
    return base / ".dutyon" / "device.json";
}

std::string randomHex(int bytes) {
    static const char* kHex = "0123456789abcdef";
    std::random_device rd;
    std::string out;
    out.reserve((size_t)bytes * 2);
    for (int i = 0; i < bytes; ++i) {
        uint8_t b = (uint8_t)(rd() & 0xFF);
        out += kHex[b >> 4];
        out += kHex[b & 0xF];
    }
    return out;
}

std::string randomDigits(int len) {
    std::random_device rd;
    std::string out;
    out.reserve((size_t)len);
    for (int i = 0; i < len; ++i) out += (char)('0' + (rd() % 10));
    // 避免全 0（观感差）
    if (out.find_first_not_of('0') == std::string::npos) out[0] = '1';
    return out;
}

} // namespace

DeviceIdentity& DeviceIdentity::instance() {
    static DeviceIdentity inst;
    return inst;
}

DeviceIdentity::DeviceIdentity() { load(); }

void DeviceIdentity::load() {
    const fs::path p = identityPath();
    std::error_code ec;
    if (fs::exists(p, ec)) {
        std::ifstream in(p);
        std::stringstream ss;
        ss << in.rdbuf();
        json j = json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
        if (!j.is_discarded() && j.is_object()) {
            device_id_ = j.value("deviceId", std::string{});
            pair_code_ = j.value("pairCode", std::string{});
            ap_pass_ = j.value("apPass", std::string{});
            token_ = j.value("token", std::string{});
        }
    }
    bool changed = false;
    if (device_id_.size() < 8) { device_id_ = randomHex(8); changed = true; }
    if (pair_code_.size() != 6) { pair_code_ = randomDigits(6); changed = true; }
    // AP 口令：8 位随机数字（WPA2 最短 8 位）；持久化保证跨重启热点口令不变
    if (ap_pass_.size() != 8) { ap_pass_ = randomDigits(8); changed = true; }
    if (changed) save();
    printf("[Identity] device_id=%s pair_code=%s paired=%d\n",
           device_id_.c_str(), pair_code_.c_str(), paired() ? 1 : 0);
}

void DeviceIdentity::save() const {
    const fs::path p = identityPath();
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    json j;
    j["deviceId"] = device_id_;
    j["pairCode"] = pair_code_;
    j["apPass"] = ap_pass_;
    j["token"] = token_;
    std::ofstream out(p);
    if (!out) {
        fprintf(stderr, "[Identity] save failed: %s\n", p.string().c_str());
        return;
    }
    out << j.dump(2);
}

std::string DeviceIdentity::shortSuffix() const {
    std::string s = device_id_;
    if (s.size() > 4) s = s.substr(s.size() - 4);
    for (auto& c : s)
        if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
    return s;
}

void DeviceIdentity::setToken(const std::string& token) {
    if (token_ == token) return;
    token_ = token;
    save();
    printf("[Identity] token %s\n", token.empty() ? "cleared" : "saved");
}

} // namespace dutyon

#endif // !_WIN32
