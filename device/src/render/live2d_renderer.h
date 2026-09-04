#pragma once

#include <string>
#include <utility>
#include <vector>
#include <memory>

#include "render/types.h"  // Rect

namespace dutyon {

// 就地将 RGBA 图像按 2×2 box filter 减半（mipmap 同款算法，无额外内存
// 分配）。用于贴图降采样以降低显存占用（模型贴图 ≤1024、缩略图 ≤128）。
// 尺寸已为 1 时无法再减半，返回 false；成功时 data 指针不变，*w/*h 减半。
bool HalveRGBA(unsigned char* data, int* w, int* h);

// 动作目录条目：动作组名 + 组内动作数
struct MotionGroupInfo {
    std::string group;
    int count = 0;
};

// Live2D Cubism Native 渲染器封装
// 直接加载 .model3.json，与桌面版模型格式完全兼容（同一套模型文件原样复用）
//
// 依赖：Cubism SDK for Native（4.x R7+ / 5.x），放入 device/third_party/CubismNativeSdk/
// 集成方式参照官方 CubismNativeSamples 的 LAppModel。
//
// 模型适配算法对齐 1.x 前端（renderer.js refineContentFit）：
//   scale = min(画布宽/内容宽, 画布高/内容高) * 0.72
//   内容底边贴画布底边，水平居中 —— 保证角色大小/位置与 1.x 一致。
class Live2DRenderer {
public:
    Live2DRenderer();
    ~Live2DRenderer();

    // 全局初始化 Cubism Framework（整个进程调用一次）
    static bool frameworkInit();
    static void frameworkDispose();

    // 加载模型目录（包含 <name>.model3.json 的文件夹）；重复调用 = 切换模型
    bool loadModel(const std::string& model_dir, const std::string& model_name);

    // 直接按目录 + model3.json 文件名加载（用户模型目录结构不定，
    // 由 UserConfigStore::listModels 枚举后传入）
    bool loadModelFile(const std::string& model_dir, const std::string& json_name);

    // 播放指定动作组（一次性，播完回到循环动作）
    void playMotion(const std::string& group, int index);

    // 设置状态循环动作（动作结束后自动重播；对应 1.x playStateMotion）
    void setLoopMotion(const std::string& group, int index);

    // 每帧调用：更新动作/物理/眨眼/呼吸，然后渲染
    void update(float delta_seconds);
    void render();

    void setViewport(int x, int y, int w, int h);
    // 垂直对齐：true=居中（单任务/相框模式），false=贴底（多任务模式，默认）
    void setCenterV(bool center) { center_v_ = center; }

    // 左右翻转（对应 1.x flipHorizontal）
    void setFlip(bool flip);
    bool isFlipped() const;

    // 当前模型的动作目录（播放动作/动作设定菜单数据源）
    std::vector<MotionGroupInfo> motionGroups() const;

    // 当前内容包围盒（视口坐标系像素，Y 向下）；头饰特效/命中测试用
    Rect contentRect() const;

    bool isLoaded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool center_v_ = false;  // 垂直居中（单任务/相框模式）
};

} // namespace dutyon
