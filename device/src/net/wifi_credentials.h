#pragma once

#include <algorithm>
#include <string>

namespace dutyon {

inline bool validWifiCredentials(const std::string& ssid, const std::string& pass) {
    const auto safe = [](const std::string& s) {
        return s.find_first_of(std::string("\r\n\0", 3)) == std::string::npos;
    };
    if (ssid.empty() || ssid.size() > 32 || !safe(ssid) || !safe(pass)) return false;
    if (pass.empty()) return true;
    if (pass.size() == 64) {
        return std::all_of(pass.begin(), pass.end(), [](unsigned char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        });
    }
    return pass.size() >= 8 && pass.size() <= 63 &&
        std::all_of(pass.begin(), pass.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
}

inline std::string wifiSsidHex(const std::string& ssid) {
    const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : ssid) { out += digits[c >> 4]; out += digits[c & 15]; }
    return out;
}

} // namespace dutyon
