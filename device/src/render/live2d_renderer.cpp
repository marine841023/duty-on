#include "render/live2d_renderer.h"
#include <cstdio>

// 注意：此文件是骨架实现，实际 Cubism Native SDK 集成需要：
// 1. 下载 Cubism Native SDK 放入 third_party/CubismNativeSdk/
// 2. 包含 <CubismFramework.hpp> 和 <Model/CubismUserModel.hpp>
// 3. 实现模型加载、动作播放、物理更新、渲染调用
//
// 参考：Live2D 官方 CubismNativeSamples (OpenGL ES 部分)

namespace dutyon {

struct Live2DRenderer::Impl {
    bool loaded = false;
    std::string current_motion;
    // TODO: 实际集成时加入 Cubism 模型指针
    // Live2D::Cubism::Framework::CubismUserModel* model = nullptr;
};

Live2DRenderer::Live2DRenderer() : impl_(std::make_unique<Impl>()) {}
Live2DRenderer::~Live2DRenderer() = default;

bool Live2DRenderer::loadModel(const std::string& model_dir, const std::string& model_name) {
    printf("[Live2DRenderer] loadModel: %s/%s (骨架 - 待集成 Cubism SDK)\n",
           model_dir.c_str(), model_name.c_str());
    // TODO: 实际加载逻辑
    // 1. CubismFramework::Initialize()
    // 2. 读取 .model3.json
    // 3. 加载纹理 (.png)
    // 4. 加载动作 (.motion3.json)
    // 5. 加载物理 (.physics3.json)
    impl_->loaded = true;
    return true;
}

void Live2DRenderer::playMotion(const std::string& group) {
    if (!impl_->loaded || group.empty()) return;
    printf("[Live2DRenderer] playMotion: %s\n", group.c_str());
    impl_->current_motion = group;
    // TODO: 调用 Cubism 动作管理器播放对应动作组
}

void Live2DRenderer::update(float delta_seconds) {
    if (!impl_->loaded) return;
    // TODO:
    // model->Update();
    // model->GetModel()->UpdatePhysics(delta_seconds);
    // 眨眼、呼吸等自动更新
    (void)delta_seconds;
}

void Live2DRenderer::render() {
    if (!impl_->loaded) return;
    // TODO:
    // model->Draw(projection_matrix);
}

void Live2DRenderer::setViewport(int x, int y, int w, int h) {
    // TODO: 更新投影矩阵
    (void)x; (void)y; (void)w; (void)h;
}

bool Live2DRenderer::isLoaded() const { return impl_->loaded; }

} // namespace dutyon
