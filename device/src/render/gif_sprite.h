#pragma once

#include <string>

#include "render/types.h"  // Rect

namespace dutyon {

// 自定义 GIF 形象（1.x customCharacters 的 animation 类型）
// WIC 解码：逐帧合成（帧偏移 + 处置方式）到 BGRA 缓冲，OpenGL 纹理逐帧播放。
// 适配：等比 contain、水平居中、垂直贴底、最大占画布 80%。
//
// 内存策略：合成后的全帧序列驻留 RAM（480x480x120 帧 ≈ 110MB 上限），
// 纹理只存当前帧（帧切换时 glTexSubImage2D）。同一时刻仅加载一个状态
// 的 GIF，切换状态时释放上一个（对齐 1.x <img> 单实例语义）。
class GifSprite {
public:
    GifSprite();
    ~GifSprite();

    // 加载 GIF（UTF-8 路径）；重复调用 = 换动画。失败保持原状返回 false。
    // 同步解码（120 帧约 100-200ms），状态切换频率低可接受。
    bool load(const std::string& path_utf8);
    bool isLoaded() const;
    void unload();

    // 每帧调用：按累计时间推进帧序号
    void update(float delta_seconds);
    // 画到当前 GL 视口（帧变化时上传纹理；正交 2D 四边形）
    void render();

    void setViewport(int x, int y, int w, int h);
    void setFlip(bool flip);
    bool isFlipped() const;
    // 垂直对齐：true=居中（单任务/相框模式），false=贴底（多任务模式，默认）
    void setCenterV(bool center) { center_v_ = center; }

    // 内容包围盒（视口像素，Y 向下）= 绘制矩形；头饰特效锚定用
    Rect contentRect() const;

    int frameCount() const;
    int width() const;
    int height() const;

private:
    struct Impl;
    Impl* impl_;
    bool center_v_ = false;  // 垂直居中（单任务/相框模式）
};

}  // namespace dutyon
