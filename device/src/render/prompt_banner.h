#pragma once

#include <string>

namespace dutyon {

// 开机引导横幅（仅设备端）：预渲染 PNG（stb 解码）+ 正交纹理四边形。
// 无字体引擎场景下的文案方案：提示文字烤进贴图，运行期零文本渲染开销。
// 绘制位置：屏幕底部水平居中（引导画面 = 宠物待机 + 本横幅）。
class PromptBanner {
public:
    PromptBanner();
    ~PromptBanner();

    // 加载 PNG（UTF-8 路径）；失败返回 false（render 时静默跳过）
    bool load(const std::string& path_utf8);
    bool isLoaded() const;

    // 画到当前 GL 上下文：宽度缩放到屏幕 92%（保持长宽比），
    // 底边距屏幕底部 5%（screen_w/h 为整屏像素尺寸）
    void render(int screen_w, int screen_h);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace dutyon
