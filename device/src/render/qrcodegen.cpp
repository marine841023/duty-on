#ifndef _WIN32  // 仅设备端（ARM Linux）

#include "render/qrcodegen.h"

#include <algorithm>
#include <cstdlib>
#include <climits>
#include <vector>

namespace dutyon {

namespace {

// 每块 ECC 码字数（索引 0 为填充，非法）；行=ECC 级别(L/M/Q/H)，列=版本 1~40
const int8_t ECC_CODEWORDS_PER_BLOCK[4][41] = {
    {-1,  7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // Low
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},  // Medium
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // Quartile
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},  // High
};

// 纠错块数；索引同上
const int8_t NUM_ERROR_CORRECTION_BLOCKS[4][41] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4,  4,  4,  4,  4,  6,  6,  6,  6,  7,  8,  8,  9,  9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},  // Low
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5,  5,  8,  9,  9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},  // Medium
    {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8,  8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},  // Quartile
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},  // High
};

// ECC 级别 -> format 信息里的 2 位（L=1, M=0, Q=3, H=2）
const int kEclFormatBits[4] = {1, 0, 3, 2};

// 简易位缓冲（大端序追加）
class BitBuffer : public std::vector<bool> {
public:
    void appendBits(uint32_t val, int len) {
        for (int i = len - 1; i >= 0; --i)
            push_back(((val >> i) & 1) != 0);
    }
};

} // namespace

// ---------------------------------------------------------------------------
// 编码入口
// ---------------------------------------------------------------------------
QrCode QrCode::encodeText(const std::string& text, Ecc ecl) {
    std::vector<uint8_t> bytes(text.begin(), text.end());
    return encodeBytes(bytes, ecl);
}

QrCode QrCode::encodeBytes(const std::vector<uint8_t>& data, Ecc ecl) {
    // 选最小可容纳版本（byte mode：4 bit 模式 + 版本相关字符计数位 + 数据）
    int version = 0;
    for (int v = 1; v <= 40; ++v) {
        const int capacityBits = getNumDataCodewords(v, ecl) * 8;
        const int ccBits = (v <= 9) ? 8 : 16;  // byte mode 字符计数位宽
        const int usedBits = 4 + ccBits + (int)data.size() * 8;
        if (usedBits <= capacityBits) { version = v; break; }
    }
    if (version == 0) return QrCode();  // 数据过大

    const int ccBits = (version <= 9) ? 8 : 16;
    BitBuffer bb;
    bb.appendBits(4, 4);  // byte mode 指示符 0100
    bb.appendBits((uint32_t)data.size(), ccBits);
    for (uint8_t b : data) bb.appendBits(b, 8);

    // 终止符 + 补齐到字节边界 + 交替填充字节 0xEC/0x11
    const int capacityBits = getNumDataCodewords(version, ecl) * 8;
    bb.appendBits(0, std::min(4, capacityBits - (int)bb.size()));
    bb.appendBits(0, (8 - (int)bb.size() % 8) % 8);
    for (uint8_t pad = 0xEC; bb.size() < (size_t)capacityBits; pad ^= 0xEC ^ 0x11)
        bb.appendBits(pad, 8);

    std::vector<uint8_t> dataCodewords(bb.size() / 8);
    for (size_t i = 0; i < bb.size(); ++i)
        if (bb[i]) dataCodewords[i >> 3] |= (uint8_t)(1 << (7 - (i & 7)));

    return QrCode(version, ecl, dataCodewords, -1);
}

QrCode::QrCode(int version, Ecc ecl, const std::vector<uint8_t>& dataCodewords, int msk) {
    if (version <= 0) return;  // 空对象（编码失败）
    version_ = version;
    errorCorrectionLevel_ = (int)ecl;
    size_ = version * 4 + 17;
    modules_.assign((size_t)size_ * size_, false);
    isFunction_.assign((size_t)size_ * size_, false);

    drawFunctionPatterns();
    std::vector<uint8_t> allCodewords = addEccAndInterleave(dataCodewords);
    drawCodewords(allCodewords);

    // 掩码选优：遍历 8 个掩码取罚分最低（applyMask 异或对合，施加两次即撤销）
    if (msk < 0) {
        int minPenalty = INT_MAX;
        for (int i = 0; i < 8; ++i) {
            applyMask(i);
            int penalty = getPenaltyScore();
            if (penalty < minPenalty) { msk = i; minPenalty = penalty; }
            applyMask(i);  // 撤销
        }
    }
    mask_ = msk;
    applyMask(msk);  // 应用最终掩码并写 format bits
}

// ---------------------------------------------------------------------------
// 功能图形
// ---------------------------------------------------------------------------
void QrCode::setFunctionModule(int x, int y, bool isDark) {
    if (x < 0 || x >= size_ || y < 0 || y >= size_) return;
    modules_[(size_t)y * size_ + x] = isDark;
    isFunction_[(size_t)y * size_ + x] = true;
}

void QrCode::drawFunctionPatterns() {
    // 定位（timing）图形
    for (int i = 0; i < size_; ++i) {
        setFunctionModule(6, i, i % 2 == 0);
        setFunctionModule(i, 6, i % 2 == 0);
    }
    // 三个探测图形（finder，含外围分隔符）
    auto drawFinder = [&](int cx, int cy) {
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                int dist = std::max(std::abs(dx), std::abs(dy));
                setFunctionModule(cx + dx, cy + dy, dist != 2 && dist != 4);
            }
    };
    drawFinder(3, 3);
    drawFinder(size_ - 4, 3);
    drawFinder(3, size_ - 4);

    // 校正（alignment）图形
    std::vector<int> alignPos = getAlignmentPatternPositions();
    int numAlign = (int)alignPos.size();
    for (int i = 0; i < numAlign; ++i)
        for (int j = 0; j < numAlign; ++j) {
            if ((i == 0 && j == 0) || (i == 0 && j == numAlign - 1) ||
                (i == numAlign - 1 && j == 0))
                continue;  // 跳过与 finder 重叠的三角
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    setFunctionModule(alignPos[i] + dx, alignPos[j] + dy,
                                      std::max(std::abs(dx), std::abs(dy)) != 1);
        }
    // 预留 format / version 信息区（必须先于 drawCodewords 标记为功能
    // 模块，否则数据位会写进保留区、位游标错位）。format 值依赖掩码，
    // 此处先用掩码 0 占位，applyMask 时再以最终掩码号重绘
    drawFormatBits(0);
    drawVersionBits();
}

void QrCode::drawFormatBits(int msk) {
    // ECC(2) + mask(3)，BCH(15,5) 后 ^0x5412
    int data = (kEclFormatBits[errorCorrectionLevel_] << 3) | msk;
    int rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    int bits = ((data << 10) | rem) ^ 0x5412;
    // 第一副本（左上 finder 周围）
    for (int i = 0; i <= 5; ++i) setFunctionModule(8, i, ((bits >> i) & 1) != 0);
    setFunctionModule(8, 7, ((bits >> 6) & 1) != 0);
    setFunctionModule(8, 8, ((bits >> 7) & 1) != 0);
    setFunctionModule(7, 8, ((bits >> 8) & 1) != 0);
    for (int i = 9; i < 15; ++i) setFunctionModule(14 - i, 8, ((bits >> i) & 1) != 0);
    // 第二副本（右上 + 左下）
    for (int i = 0; i < 8; ++i) setFunctionModule(size_ - 1 - i, 8, ((bits >> i) & 1) != 0);
    for (int i = 8; i < 15; ++i) setFunctionModule(8, size_ - 15 + i, ((bits >> i) & 1) != 0);
    setFunctionModule(8, size_ - 8, true);  // 固定黑点
}

void QrCode::drawVersionBits() {
    if (version_ < 7) return;
    int rem = version_;
    for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    int bits = (version_ << 12) | rem;
    for (int i = 0; i < 18; ++i) {
        bool bit = ((bits >> i) & 1) != 0;
        int a = size_ - 11 + i % 3, b = i / 3;
        setFunctionModule(a, b, bit);
        setFunctionModule(b, a, bit);
    }
}

std::vector<int> QrCode::getAlignmentPatternPositions() const {
    if (version_ == 1) return {};
    int numAlign = version_ / 7 + 2;
    int step = (version_ == 32)
                   ? 26
                   : (version_ * 4 + numAlign * 2 + 1) / (numAlign * 2 - 2) * 2;
    std::vector<int> result;
    for (int i = 0, pos = size_ - 7; i < numAlign - 1; ++i, pos -= step)
        result.insert(result.begin(), pos);
    // 6 必须置于最前（升序 [6, ..., size-7]）：三角跳过逻辑依赖首/尾元素
    // 对应 finder 角，顺序错了会把校正图形画到 finder/timing 区
    result.insert(result.begin(), 6);
    return result;
}

void QrCode::applyMask(int msk) {
    // 数据区异或掩码（功能图形不动）
    for (int y = 0; y < size_; ++y)
        for (int x = 0; x < size_; ++x) {
            if (isFunction_[(size_t)y * size_ + x]) continue;
            bool invert;
            switch (msk) {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                case 7: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                default: invert = false; break;
            }
            modules_[(size_t)y * size_ + x] = modules_[(size_t)y * size_ + x] != invert;
        }

    // 以最终掩码号重绘 format 信息（绝对赋值，不受上方异或影响）。
    // version 信息已在 drawFunctionPatterns 预留且与掩码无关，无需重绘
    drawFormatBits(msk);
}

void QrCode::drawCodewords(const std::vector<uint8_t>& data) {
    int i = 0;  // 比特游标
    for (int right = size_ - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;  // 跳过竖直 timing 列
        for (int vert = 0; vert < size_; ++vert) {
            for (int j = 0; j < 2; ++j) {
                int x = right - j;
                bool upward = ((right + 1) & 2) == 0;
                int y = upward ? size_ - 1 - vert : vert;
                if (!isFunction_[(size_t)y * size_ + x] && i < (int)data.size() * 8) {
                    modules_[(size_t)y * size_ + x] =
                        ((data[(size_t)i >> 3] >> (7 - (i & 7))) & 1) != 0;
                    ++i;
                }
            }
        }
    }
}

std::vector<uint8_t> QrCode::addEccAndInterleave(const std::vector<uint8_t>& data) const {
    const int ver = version_;
    const int ecl = errorCorrectionLevel_;
    const int numBlocks = NUM_ERROR_CORRECTION_BLOCKS[ecl][ver];
    const int blockEccLen = ECC_CODEWORDS_PER_BLOCK[ecl][ver];
    const int rawCodewords = getNumRawDataModules(ver) / 8;
    const int numShortBlocks = numBlocks - rawCodewords % numBlocks;
    const int shortBlockLen = rawCodewords / numBlocks;

    std::vector<std::vector<uint8_t>> blocks;
    std::vector<uint8_t> rsDiv = reedSolomonComputeDivisor(blockEccLen);
    for (int i = 0, k = 0; i < numBlocks; ++i) {
        const int datLen = shortBlockLen - blockEccLen + (i < numShortBlocks ? 0 : 1);
        std::vector<uint8_t> dat(data.begin() + k, data.begin() + k + datLen);
        k += datLen;
        std::vector<uint8_t> ecc = reedSolomonComputeRemainder(dat, rsDiv);
        if (i < numShortBlocks) dat.push_back(0);  // 短块数据末尾占位（交错时跳过）
        dat.insert(dat.end(), ecc.begin(), ecc.end());
        blocks.push_back(std::move(dat));
    }

    // 交错：逐列取，跳过短块的数据占位 0
    std::vector<uint8_t> result;
    for (size_t i = 0; i < blocks[0].size(); ++i)
        for (size_t j = 0; j < blocks.size(); ++j) {
            if (i == (size_t)(shortBlockLen - blockEccLen) && j < (size_t)numShortBlocks)
                continue;
            result.push_back(blocks[j][i]);
        }
    return result;
}

std::vector<uint8_t> QrCode::reedSolomonComputeDivisor(int degree) {
    std::vector<uint8_t> result(degree);
    result[degree - 1] = 1;
    uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            result[j] = reedSolomonMultiply(result[j], root);
            if (j + 1 < degree) result[j] ^= result[j + 1];
        }
        root = reedSolomonMultiply(root, 0x02);
    }
    return result;
}

std::vector<uint8_t> QrCode::reedSolomonComputeRemainder(
    const std::vector<uint8_t>& data, const std::vector<uint8_t>& divisor) {
    std::vector<uint8_t> result(divisor.size(), 0);
    for (uint8_t b : data) {
        uint8_t factor = b ^ result[0];
        result.erase(result.begin());
        result.push_back(0);
        for (size_t i = 0; i < result.size(); ++i)
            result[i] ^= reedSolomonMultiply(divisor[i], factor);
    }
    return result;
}

uint8_t QrCode::reedSolomonMultiply(uint8_t x, uint8_t y) {
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return (uint8_t)z;
}

int QrCode::getNumRawDataModules(int ver) {
    int result = (16 * ver + 128) * ver + 64;
    if (ver >= 2) {
        int numAlign = ver / 7 + 2;
        result -= (25 * numAlign - 10) * numAlign - 55;
        if (ver >= 7) result -= 36;
    }
    return result;
}

int QrCode::getNumDataCodewords(int ver, Ecc ecl) {
    return getNumRawDataModules(ver) / 8 -
           ECC_CODEWORDS_PER_BLOCK[(int)ecl][ver] *
               NUM_ERROR_CORRECTION_BLOCKS[(int)ecl][ver];
}

// ---------------------------------------------------------------------------
// 掩码罚分（ISO 18004 规则 1/2/4；规则 3 的 finder 型图案从简省略，
// 对掩码选择影响可忽略）
// ---------------------------------------------------------------------------
int QrCode::getPenaltyScore() const {
    int result = 0;
    // 规则 1：行/列连续同色，长度 n>=5 记 3 + (n-5)
    for (int y = 0; y < size_; ++y) {
        int runColor = -1, runLen = 0;
        for (int x = 0; x < size_; ++x) {
            int c = at(x, y) ? 1 : 0;
            if (c == runColor) { ++runLen; }
            else { if (runLen >= 5) result += 3 + (runLen - 5); runColor = c; runLen = 1; }
        }
        if (runLen >= 5) result += 3 + (runLen - 5);
    }
    for (int x = 0; x < size_; ++x) {
        int runColor = -1, runLen = 0;
        for (int y = 0; y < size_; ++y) {
            int c = at(x, y) ? 1 : 0;
            if (c == runColor) { ++runLen; }
            else { if (runLen >= 5) result += 3 + (runLen - 5); runColor = c; runLen = 1; }
        }
        if (runLen >= 5) result += 3 + (runLen - 5);
    }
    // 规则 2：每个 2x2 同色块 +3
    for (int y = 0; y < size_ - 1; ++y)
        for (int x = 0; x < size_ - 1; ++x) {
            bool c = at(x, y);
            if (c == at(x + 1, y) && c == at(x, y + 1) && c == at(x + 1, y + 1))
                result += 3;
        }
    // 规则 4：黑白比例偏离 50% —— 每偏 5% 记 10
    int dark = 0;
    for (bool b : modules_) if (b) ++dark;
    int total = size_ * size_;
    int k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
    result += k * 10;
    return result;
}

bool QrCode::at(int x, int y) const {
    if (x < 0 || x >= size_ || y < 0 || y >= size_) return false;
    return modules_[(size_t)y * size_ + x];
}

} // namespace dutyon

#endif // !_WIN32
