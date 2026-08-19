#pragma once

#include <string>
#include <memory>

namespace dutyon {

// Live2D Cubism Native 渲染器封装
// 直接加载 .model3.json，与桌面版模型格式完全兼容（同一套模型文件原样复用）
//
// 依赖：Cubism SDK for Native（4.x R7+ / 5.x），放入 device/third_party/CubismNativeSdk/
// 集成方式参照官方 CubismNativeSamples 的 LAppModel。
class Live2DRenderer {
public:
    Live2DRenderer();
    ~Live2DRenderer();

    // 全局初始化 Cubism Framework（整个进程调用一次）
    static bool frameworkInit();
    static void frameworkDispose();

    // 加载模型目录（包含 <name>.model3.json 的文件夹）
    bool loadModel(const std::string& model_dir, const std::string& model_name);

    // 播放指定动作组（状态切换时调用）
    void playMotion(const std::string& group, int index);

    // 每帧调用：更新动作/物理/眨眼/呼吸，然后渲染
    void update(float delta_seconds);
    void render();

    void setViewport(int x, int y, int w, int h);

    bool isLoaded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dutyon
