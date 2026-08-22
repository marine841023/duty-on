#pragma once

// 渲染/UI 共享的通用几何类型。从 live2d_renderer.h 抽出：屏幕矩形
// 并非 Live2D 渲染器专有职责（GIF 精灵、UI 命中区、点击穿透都用它）。
namespace dutyon {

// 屏幕矩形（像素，窗口客户区坐标系，Y 向下）
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

}  // namespace dutyon
