#pragma once

#include "net/wifi_credentials.h"

#include <istream>
#include <vector>

namespace dutyon {

// iw prints non-ASCII bytes and literal backslashes as \xHH. Decode exactly
// once: a network literally named "\xe5" is printed as "\x5cxe5".
inline std::string decodeIwSsid(const std::string& encoded) {
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '\\' && i + 3 < encoded.size() && encoded[i + 1] == 'x') {
            const int hi = hex(encoded[i + 2]), lo = hex(encoded[i + 3]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 3;
                continue;
            }
        }
        out += encoded[i];
    }
    return out;
}

// The portal submits UTF-8 text. Do not offer byte sequences the browser would
// replace, or control characters it could normalize into a different SSID.
inline bool displayableWifiSsid(const std::string& ssid) {
    if (!validWifiCredentials(ssid, "")) return false;
    for (size_t i = 0; i < ssid.size();) {
        const unsigned char first = ssid[i++];
        unsigned cp = first, minimum = 0;
        int remaining = 0;
        if (first >= 0xc2 && first <= 0xdf) { cp &= 0x1f; remaining = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { cp &= 0x0f; remaining = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { cp &= 7; remaining = 3; minimum = 0x10000; }
        else if (first >= 0x80) return false;
        while (remaining-- > 0) {
            if (i == ssid.size()) return false;
            const unsigned char next = ssid[i++];
            if ((next & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (next & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp < 0x20 || (cp >= 0x7f && cp <= 0x9f)) return false;
    }
    return true;
}

inline std::vector<std::string> parseWifiScanList(std::istream& in) {
    std::vector<std::string> out;
    std::string line;
    while (out.size() < 8 && std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto ssid = decodeIwSsid(line);
        // Spaces belong to the network name; never trim after decoding.
        if (displayableWifiSsid(ssid) && std::find(out.begin(), out.end(), ssid) == out.end())
            out.push_back(ssid);
    }
    return out;
}

} // namespace dutyon
