#pragma once

// 设备端文字渲染（ARM Linux，GLES2 语法）：stb_truetype 按需烘焙字形 +
// 单张图集纹理 + 四边形批量绘制，支持 UTF-8（含 CJK）。
// PC 端走 ImGui 自有字体系统，本类仅设备端编译。

#ifndef _WIN32

#include <string>

namespace dutyon {

class TextRenderer {
public:
    TextRenderer();
    ~TextRenderer();

    // 加载字体文件（TTF/OTF，须常驻至析构）；失败返回 false，之后绘制为空操作
    bool load(const std::string& font_path);
    bool isLoaded() const;

    // 绘制一行文字（UTF-8）。(x, y) 为文字左上角锚点，像素坐标（GL 原点左下，
    // y 向上）。内部自动启用 alpha 混合。
    void draw(const std::string& text_utf8, float x, float y, float pixel_size,
              float r, float g, float b, float a,
              int screen_w, int screen_h);

    // 加粗绘制：origin 偏移 stroke 像素再叠画一次（卡通字体本身笔画细，
    // 单通道偏细）。stroke 默认 1.5px。
    void drawBold(const std::string& text_utf8, float x, float y,
                  float pixel_size, float r, float g, float b, float a,
                  int screen_w, int screen_h, float stroke = 1.5f);

    // 整行宽度（字距 + kerning，不含烘焙）
    float measureWidth(const std::string& text_utf8, float pixel_size) const;

    // 行高（ascent - descent，用于垂直居中）
    float lineHeight(float pixel_size) const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace dutyon

#endif // !_WIN32
