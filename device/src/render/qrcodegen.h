#pragma once

// 自包含 QR 码编码器（仅设备端）：字节模式（byte mode）+ Reed-Solomon 纠错，
// 版本自动选择（1~40），掩码按罚分自动选优。用于 AP 配网时把「加入热点」的
// WIFI: 连接串编码成二维码画到屏幕（见 render/qr_banner）。
//
// 无第三方依赖：GF(256) 运算 + RS 纠错 + 矩阵布局全部内联实现，算法遵循
// ISO/IEC 18004（参考 Nayuki qrcodegen 的公开实现，MIT）。
#ifndef _WIN32

#include <cstdint>
#include <string>
#include <vector>

namespace dutyon {

class QrCode {
public:
    // 纠错级别（越低可容纳数据越多；配网串短，用 Medium 足够且抗污损好）
    enum class Ecc { Low = 0, Medium = 1, Quartile = 2, High = 3 };

    // 把文本（按 UTF-8 原始字节，byte mode）编码为 QR。
    // 成功返回 QrCode（size()>0）；数据超出 40 版容量时返回空（size()==0）。
    static QrCode encodeBytes(const std::vector<uint8_t>& data, Ecc ecl);
    static QrCode encodeText(const std::string& text, Ecc ecl);

    // 边长（模块数，21~177，恒为奇数）；0 = 编码失败
    int size() const { return size_; }
    // 取 (x,y) 模块：true = 黑（深色），false = 白（浅色）。越界返回 false。
    bool at(int x, int y) const;

private:
    QrCode() = default;  // 空对象（编码失败）
    QrCode(int version, Ecc ecl, const std::vector<uint8_t>& dataCodewords, int msk);

    int size_ = 0;
    std::vector<bool> modules_;      // size_*size_，行主序
    std::vector<bool> isFunction_;   // 与 modules_ 同尺寸，标记功能图形（掩码不改）

    void drawFunctionPatterns();
    void drawFormatBits(int msk);   // 写/预留 format 信息（依赖掩码号）
    void drawVersionBits();         // 写/预留 version 信息（version>=7，与掩码无关）
    void drawCodewords(const std::vector<uint8_t>& data);
    void applyMask(int msk);
    int getPenaltyScore() const;

    void setFunctionModule(int x, int y, bool isDark);
    std::vector<uint8_t> addEccAndInterleave(const std::vector<uint8_t>& data) const;
    static std::vector<uint8_t> reedSolomonComputeDivisor(int degree);
    static std::vector<uint8_t> reedSolomonComputeRemainder(
        const std::vector<uint8_t>& data, const std::vector<uint8_t>& divisor);
    static uint8_t reedSolomonMultiply(uint8_t x, uint8_t y);
    std::vector<int> getAlignmentPatternPositions() const;
    static int getNumRawDataModules(int ver);
    static int getNumDataCodewords(int ver, Ecc ecl);

    int version_ = 0;
    int errorCorrectionLevel_ = 0;   // Ecc 的整数值
    int mask_ = -1;
};

} // namespace dutyon

#endif // !_WIN32
