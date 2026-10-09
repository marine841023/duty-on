// ---------------------------------------------------------------------------
// SHA-256 单头实现（header-only）。
// 算法：public domain 参考实现（Brad Conte, brad@bradconte.com）改写为
// C++ 内联版本；对外仅提供云同步所需的 fileHex()（文件摘要，小写 hex，
// 与 Go 服务端 hex.EncodeToString 一致）。
// 用法：#include "sha256.h"  →  SHA256::fileHex("a.zip")
// ---------------------------------------------------------------------------
#ifndef DUTYON_SHA256_H
#define DUTYON_SHA256_H

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace dutyon {
namespace SHA256 {

namespace detail {

struct Ctx {
    uint32_t state[8];
    uint64_t datalen;   // 当前块内已积累字节数
    uint64_t bitlen;    // 总比特数
    uint8_t data[64];   // 当前块
};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

inline void transform(Ctx& c, const uint8_t data[]) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
        0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
        0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
        0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
        0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t m[64];
    uint32_t a, b, e, f, g, h, i, j, t1, t2;

    for (i = 0, j = 0; i < 16; ++i, j += 4)
        m[i] = (data[j] << 24) | (data[j + 1] << 16) | (data[j + 2] << 8) |
               (data[j + 3]);
    for (; i < 64; ++i)
        m[i] = (rotr(m[i - 2], 17) ^ rotr(m[i - 2], 19) ^ (m[i - 2] >> 10)) +
               m[i - 7] +
               (rotr(m[i - 15], 7) ^ rotr(m[i - 15], 18) ^ (m[i - 15] >> 3)) +
               m[i - 16];

    a = c.state[0];
    b = c.state[1];
    uint32_t cc = c.state[2], d = c.state[3];
    e = c.state[4]; f = c.state[5]; g = c.state[6]; h = c.state[7];

    for (i = 0; i < 64; ++i) {
        t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) +
             ((e & f) ^ (~e & g)) + k[i] + m[i];
        t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) +
             ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }

    c.state[0] += a; c.state[1] += b; c.state[2] += cc; c.state[3] += d;
    c.state[4] += e; c.state[5] += f; c.state[6] += g; c.state[7] += h;
}

inline void init(Ctx& c) {
    c.datalen = 0;
    c.bitlen = 0;
    c.state[0] = 0x6a09e667; c.state[1] = 0xbb67ae85;
    c.state[2] = 0x3c6ef372; c.state[3] = 0xa54ff53a;
    c.state[4] = 0x510e527f; c.state[5] = 0x9b05688c;
    c.state[6] = 0x1f83d9ab; c.state[7] = 0x5be0cd19;
}

inline void update(Ctx& c, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        c.data[c.datalen] = data[i];
        c.datalen++;
        if (c.datalen == 64) {
            transform(c, c.data);
            c.bitlen += 512;
            c.datalen = 0;
        }
    }
}

inline void final(Ctx& c, uint8_t out[32]) {
    size_t i = c.datalen;
    // 填充：0x80 + 0x00… + 8 字节大端比特长度
    if (c.datalen < 56) {
        c.data[i++] = 0x80;
        while (i < 56) c.data[i++] = 0x00;
    } else {
        c.data[i++] = 0x80;
        while (i < 64) c.data[i++] = 0x00;
        transform(c, c.data);
        memset(c.data, 0, 56);
    }
    c.bitlen += c.datalen * 8;
    for (i = 0; i < 8; ++i)
        c.data[63 - i] = (uint8_t)(c.bitlen >> (i * 8));
    transform(c, c.data);
    for (i = 0; i < 4; ++i)
        for (size_t j = 0; j < 8; ++j)
            out[i + j * 4] = (uint8_t)((c.state[j] >> (24 - i * 8)) & 0xff);
}

} // namespace detail

// 数据摘要 → 32 字节
inline std::vector<uint8_t> hash(const void* data, size_t len) {
    detail::Ctx c;
    detail::init(c);
    detail::update(c, (const uint8_t*)data, len);
    std::vector<uint8_t> out(32);
    detail::final(c, out.data());
    return out;
}

// 字节串 → 小写 hex（与 Go hex.EncodeToString 一致）
inline std::string toHex(const std::vector<uint8_t>& b) {
    std::ostringstream o;
    for (uint8_t x : b) o << std::hex << std::setw(2) << std::setfill('0')
                          << (int)x;
    return o.str();
}

// 文件摘要 → 小写 hex（64KB 分块，任意大小文件；失败返回空串）
inline std::string fileHex(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    detail::Ctx c;
    detail::init(c);
    std::vector<char> buf(64 * 1024);
    while (f.read(buf.data(), (std::streamsize)buf.size()) || f.gcount() > 0) {
        detail::update(c, (const uint8_t*)buf.data(), (size_t)f.gcount());
        if (!f) break;  // 短读（EOF）
    }
    uint8_t out[32];
    detail::final(c, out);
    return toHex(std::vector<uint8_t>(out, out + 32));
}

} // namespace SHA256
} // namespace dutyon

#endif // DUTYON_SHA256_H
