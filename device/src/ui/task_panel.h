#pragma once

// 设备端任务列表面板（屏幕下半屏）：会话行 = 状态色圆点 + 项目名 + 状态文字，
// 配色/文案对齐 PC 端状态栏（ui_renderer.cpp StyleFor / StatusText）。
// 面板高度按行数自适应，不要求填满下半屏。

#ifndef _WIN32

#include <string>

#include "api/client.h"

namespace dutyon {

class TaskPanel {
public:
    TaskPanel();
    ~TaskPanel();

    // 加载文字字体；失败则面板只画底色无文字
    bool init(const std::string& font_path);

    // 在下半屏绘制：area_top = 面板区顶边（布局约定为 screen_h/2），
    // 面板充满该区（顶边贴角色区、底边贴屏幕底）
    void render(const PetStatus& status, int screen_w, int screen_h, float area_top);

    // 顶部时钟（HH:MM:SS 居中）：y_top = 文本区顶边（GL 坐标），
    // size = 字号（多任务模式 ~40，单任务模式 ~56）
    void renderClock(const std::string& text, float y_top, float size,
                     int screen_w, int screen_h);

    // 顶部日期行（YYYY年M月D日 星期X 居中）：时钟下方，同款字体/配色、
    // 字号更小；过宽自动缩字号保证一行放下
    void renderDate(const std::string& text, float y_top, float size,
                    int screen_w, int screen_h);

    // 时钟字体行高（用于日期行垂直定位；卡通字体加载失败回退主字体行高）
    float clockLineHeight(float pixel_size) const;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace dutyon

#endif // !_WIN32
