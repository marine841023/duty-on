#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dutyon {
inline bool validImageSize(int w, int h) {
    return w > 0 && h > 0 && w <= 4096 && h <= 4096 &&
           uint64_t(w) * h <= 8 * 1024 * 1024;
}

// Count frames before stb allocates the complete RGBA animation. The compressed
// size alone cannot bound memory (tiny GIF frames can expand enormously).
inline bool validGifBudget(const unsigned char* b, size_t size) {
    if (size < 13 || (std::memcmp(b, "GIF87a", 6) && std::memcmp(b, "GIF89a", 6)))
        return false;
    auto word = [&](size_t p) { return unsigned(b[p]) | (unsigned(b[p+1]) << 8); };
    const unsigned w = word(6), h = word(8);
    if (!validImageSize(w, h)) return false;
    size_t p = 13, frames = 0;
    auto skip = [&](size_t n) {
        if (n > size - p) return false;
        p += n; return true;
    };
    auto blocks = [&]() {
        while (p < size) {
            const size_t n = b[p++];
            if (!n) return true;
            if (!skip(n)) return false;
        }
        return false;
    };
    if ((b[10] & 0x80) && !skip(3u << ((b[10] & 7) + 1))) return false;
    while (p < size) {
        const unsigned tag = b[p++];
        if (tag == 0x3b) return frames > 0;
        if (tag == 0x21) {
            if (!skip(1) || !blocks()) return false;
        } else if (tag == 0x2c) {
            if (size - p < 9) return false;
            const unsigned x = word(p), y = word(p+2);
            const unsigned fw = word(p+4), fh = word(p+6), flags = b[p+8];
            p += 9;
            if (!fw || !fh || x + fw > w || y + fh > h) return false;
            if (++frames > 1000 || uint64_t(w) * h * 4 * frames > 128 * 1024 * 1024)
                return false;
            if ((flags & 0x80) && !skip(3u << ((flags & 7) + 1))) return false;
            if (!skip(1) || !blocks()) return false;
        } else return false;
    }
    return false;
}
} // namespace dutyon
