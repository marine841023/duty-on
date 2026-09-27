#pragma once

// QR 配网横幅（仅设备端）：把 Wi-Fi 连接串编码成二维码并画到屏幕。
// 正交纹理四边形套路（同 gif_sprite）——先用自包含 QR 编码器
//（render/qrcodegen）生成模块矩阵，再烘焙成一张 RGBA 纹理（每模块一个
// 方块 + 4 模块白边静区），GL_NEAREST 采样保证边缘锐利。
//
// 用法：payload 变化时调 setPayload() 重建纹理（配网串稳定，通常只建一次）；
// 每帧调 render() 画到屏幕中央。绘制在角色/时钟之上（AP 配网画面）。
#ifndef _WIN32

#include <string>

namespace dutyon {

class QrBanner {
public:
    QrBanner();
    ~QrBanner();

    // 编码文本为 QR 并重建纹理；成功返回 true。文本过长（超 40 版容量）
    // 或 GL 未就绪时返回 false（render 静默跳过）。相同 payload 不重复重建。
    bool setPayload(const std::string& text_utf8);

    bool isReady() const;

    // 画到当前 GL 上下文：正方形，边长 = min(screen_w, screen_h) * fill。
    // 默认屏幕中央；center_x/center_y >= 0 时改以该像素坐标为中心（横屏把
    // QR 放左列、右列留给配网步骤文案时用）。screen_w/h 为整屏像素尺寸，
    // fill∈(0,1]（默认 0.7）。
    void render(int screen_w, int screen_h, float fill = 0.7f,
                float center_x = -1.f, float center_y = -1.f);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace dutyon

#endif // !_WIN32
