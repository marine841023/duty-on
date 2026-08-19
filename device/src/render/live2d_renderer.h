#pragma once

#include <string>
#include <memory>

namespace dutyon {

// Live2D Cubism Native 渲染器封装
// 直接加载 .model3.json，与桌面版模型格式完全兼容
class Live2DRenderer {
public:
    Live2DRenderer();
    ~Live2DRenderer();

    // 加载模型目录（包含 .model3.json 的文件夹）
    bool loadModel(const std::string& model_dir, const std::string& model_name);

    // 播放指定动作组（状态切换时调用）
    void playMotion(const std::string& group);

    // 每帧调用：更新物理、眨眼、渲染
    void update(float delta_seconds);
    void render();

    void setViewport(int x, int y, int w, int h);

    bool isLoaded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dutyon
