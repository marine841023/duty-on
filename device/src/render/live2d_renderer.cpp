// Live2D Cubism Native 集成实现
// 结构参照官方 CubismNativeSamples（Samples/OpenGL 的 LAppModel / LAppAllocator）
//
// 前置条件：
//   1. 从 https://www.live2d.com/download/cubism-sdk/download-native/ 下载
//      Cubism SDK for Native（4.x R7+），解压后把 Core 和 Framework
//      放到 device/third_party/CubismNativeSdk/ 下：
//        third_party/CubismNativeSdk/Core/{include,lib}
//        third_party/CubismNativeSdk/Framework/src
//   2. stb_image.h 放入 third_party/stb/（纹理 PNG 解码）
//
// 目标宏：CSM_TARGET_WIN_GL (Windows) 或 CSM_TARGET_LINUX_GL (ARM Linux)
//          —— 由 CMakeLists 按平台定义

#include "render/live2d_renderer.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

// GL 头文件：PC 由 GLEW 引入桌面 OpenGL，设备用 OpenGL ES
#ifdef _WIN32
#define GLFW_INCLUDE_NONE   // 阻止 glfw3.h 包含 <GL/gl.h>（与 glew.h 冲突）
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#else
#include <GLES3/gl3.h>
#endif

// ---- Cubism Framework ----
#include <CubismFramework.hpp>
#include <ICubismAllocator.hpp>
#include <Model/CubismUserModel.hpp>
#include <CubismModelSettingJson.hpp>   // SDK 5 起移至 Framework/src 根目录
#include <Motion/CubismMotionManager.hpp>
#include <Physics/CubismPhysics.hpp>
#include <CubismDefaultParameterId.hpp>
#include <Utils/CubismString.hpp>
#include <Id/CubismIdManager.hpp>
// SDK 5 起 PC/设备统一为 OpenGLES2 渲染器（Windows 下经 CSM_TARGET_WIN_GL
// 走桌面 OpenGL，由 GLEW 提供函数指针）
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
using GLRenderer = Live2D::Cubism::Framework::Rendering::CubismRenderer_OpenGLES2;

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

using namespace Live2D::Cubism::Framework;
using namespace Live2D::Cubism::Framework::Rendering;

namespace dutyon {

// ---------------------------------------------------------------------------
// 内存分配器（Cubism Framework 要求注入）
// ---------------------------------------------------------------------------
class Allocator : public ICubismAllocator {
public:
    void* Allocate(const csmSizeType size) override {
        return malloc(size ? size : 1);
    }
    void Deallocate(void* memory) override { free(memory); }
    void* AllocateAligned(const csmSizeType size, const csmUint32 alignment) override {
        size_t offset, shift, addr;
        void** preamble;
        offset = alignment - 1 + sizeof(void*);
        shift = 0;
        void* allocated = malloc(size + static_cast<csmUint32>(offset));
        addr = reinterpret_cast<size_t>(allocated) + sizeof(void*);
        while (addr % alignment != 0) { addr++; shift++; }
        preamble = reinterpret_cast<void**>(addr);
        preamble[-1] = allocated;
        (void)shift;
        return reinterpret_cast<void*>(addr);
    }
    void DeallocateAligned(void* aligned_memory) override {
        void** preamble = static_cast<void**>(aligned_memory);
        free(preamble[-1]);
    }
};

// ---------------------------------------------------------------------------
// 读文件到内存（框架要求自行实现 IO）
// ---------------------------------------------------------------------------
static csmByte* CreateBuffer(const char* path, csmSizeInt* out_size) {
    FILE* fp = fopen(path, "rb");
    if (!fp) return nullptr;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    auto* buf = static_cast<csmByte*>(malloc(size));
    fread(buf, size, 1, fp);
    fclose(fp);
    *out_size = static_cast<csmSizeInt>(size);
    return buf;
}

static void DeleteBuffer(csmByte* buffer) { free(buffer); }

// ---------------------------------------------------------------------------
// PNG 纹理加载（stb_image -> GL texture）
// ---------------------------------------------------------------------------
static GLuint CreateTextureFromPng(const char* path, int* out_w, int* out_h) {
    int w, h, channels;
    unsigned char* data = stbi_load(path, &w, &h, &channels, STBI_rgb_alpha);
    if (!data) {
        fprintf(stderr, "[Live2D] texture load failed: %s\n", path);
        return 0;
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return tex;
}

// ---------------------------------------------------------------------------
// 模型封装（对应官方 Sample 的 LAppModel）
// ---------------------------------------------------------------------------
class PetModel : public CubismUserModel {
public:
    PetModel() : CubismUserModel(), _setting(nullptr) {}
    ~PetModel() override {
        if (_setting) { CSM_DELETE _setting; _setting = nullptr; }
    }

    // 从 <dir>/<name>.model3.json 加载全部资源
    bool LoadAssets(const std::string& dir, const std::string& file_name) {
        _modelHomeDir = dir;
        if (_modelHomeDir.back() != '/') _modelHomeDir += '/';

        std::string json_path = _modelHomeDir + file_name;
        csmSizeInt size = 0;
        csmByte* buffer = CreateBuffer(json_path.c_str(), &size);
        if (!buffer) {
            fprintf(stderr, "[Live2D] model json not found: %s\n", json_path.c_str());
            return false;
        }
        ICubismModelSetting* setting = new CubismModelSettingJson(buffer, size);
        DeleteBuffer(buffer);

        SetupModel(setting);
        SetupTextures();

        // 表情/物理/眨眼/姿势由 SetupModel 内部按 setting 加载
        return true;
    }

    // 每帧更新（动作 -> 物理 -> 眨眼 -> 姿势）
    void UpdateModel(float delta_seconds) {
        _deltaTimeSeconds = delta_seconds;

        // 动作（优先用户触发，否则待机）
        bool motion_updated = false;
        _model->LoadParameters();
        if (_motionManager->IsFinished()) {
            // 待机动作循环（Idle 组随机）
            StartRandomMotion("Idle", MotionPriorityIdle);
        } else {
            motion_updated = _motionManager->UpdateMotion(_model, delta_seconds);
        }
        _model->SaveParameters();
        if (!motion_updated && _eyeBlink) {
            _eyeBlink->UpdateParameters(_model, delta_seconds);
        }

        if (_physics) _physics->Evaluate(_model, delta_seconds);
        if (_breath)   _breath->UpdateParameters(_model, delta_seconds);
        if (_pose)     _pose->UpdateParameters(_model, delta_seconds);

        _model->Update();
    }

    // 播放指定组动作
    void PlayMotionGroup(const std::string& group, int index) {
        StartMotion(group.c_str(), index, MotionPriorityForce);
    }

    // 渲染（调用前确保 GL 上下文 current、viewport 已设）
    void Draw(int window_w, int window_h) {
        if (!_model) return;

        CubismMatrix44 projection;
        projection.LoadIdentity();

        // 以画布逻辑尺寸等比缩放，保持模型完整可见
        csmFloat32 scale = 1.0f;
        csmFloat32 model_w = _model->GetCanvasWidth();
        csmFloat32 model_h = _model->GetCanvasHeight();
        csmFloat32 aspect = static_cast<csmFloat32>(window_w) / window_h;
        if (model_h > 0.0f && model_w > 0.0f) {
            csmFloat32 model_aspect = model_w / model_h;
            if (aspect < model_aspect) {
                scale = static_cast<csmFloat32>(window_w) / model_w;
            } else {
                scale = static_cast<csmFloat32>(window_h) / model_h;
            }
        }
        projection.ScaleRelative(scale, scale);

        GLRenderer* renderer = GetRenderer<GLRenderer>();
        if (renderer) {
            renderer->SetMvpMatrix(&projection);
            renderer->DrawModel();
        }
    }

private:
    // 参照官方 LAppModel::SetupModel
    void SetupModel(ICubismModelSetting* setting) {
        _updating = true;
        _initialized = false;
        _setting = setting;

        // moc3
        {
            csmSizeInt size = 0;
            std::string path = _modelHomeDir + setting->GetModelFileName();
            csmByte* buffer = CreateBuffer(path.c_str(), &size);
            if (buffer) {
                LoadModel(buffer, size);
                DeleteBuffer(buffer);
            }
        }

        // 表情
        if (setting->GetExpressionCount() > 0) {
            const csmInt32 count = setting->GetExpressionCount();
            for (csmInt32 i = 0; i < count; i++) {
                csmString name = setting->GetExpressionName(i);
                std::string path = _modelHomeDir + setting->GetExpressionFileName(i);
                csmSizeInt size = 0;
                csmByte* buffer = CreateBuffer(path.c_str(), &size);
                if (buffer) {
                    ACubismMotion* motion = LoadExpression(buffer, size, name);
                    DeleteBuffer(buffer);
                    (void)motion;
                }
            }
        }

        // 物理
        if (strcmp(setting->GetPhysicsFileName(), "") != 0) {
            std::string path = _modelHomeDir + setting->GetPhysicsFileName();
            csmSizeInt size = 0;
            csmByte* buffer = CreateBuffer(path.c_str(), &size);
            if (buffer) {
                LoadPhysics(buffer, size);
                DeleteBuffer(buffer);
            }
        }

        // 姿势
        if (strcmp(setting->GetPoseFileName(), "") != 0) {
            std::string path = _modelHomeDir + setting->GetPoseFileName();
            csmSizeInt size = 0;
            csmByte* buffer = CreateBuffer(path.c_str(), &size);
            if (buffer) {
                LoadPose(buffer, size);
                DeleteBuffer(buffer);
            }
        }

        // 眨眼
        if (setting->GetEyeBlinkParameterCount() > 0) {
            _eyeBlink = CubismEyeBlink::Create(setting);
        }

        // 呼吸
        _breath = CubismBreath::Create();
        csmVector<CubismBreath::BreathParameterData> breath_params;
        breath_params.PushBack(CubismBreath::BreathParameterData(
            _idParamAngleX, 0.0f, 15.0f, 6.5345f, 0.5f));
        breath_params.PushBack(CubismBreath::BreathParameterData(
            _idParamAngleY, 0.0f, 8.0f, 3.5345f, 0.5f));
        breath_params.PushBack(CubismBreath::BreathParameterData(
            _idParamAngleZ, 0.0f, 10.0f, 5.5345f, 0.5f));
        breath_params.PushBack(CubismBreath::BreathParameterData(
            _idParamBodyAngleX, 0.0f, 4.0f, 15.5345f, 0.5f));
        breath_params.PushBack(CubismBreath::BreathParameterData(
            _idParamBreath, 0.0f, 0.5f, 3.2345f, 1.0f));
        _breath->SetParameters(breath_params);

        // 布局
        csmMap<csmString, csmFloat32> layout;
        setting->GetLayoutMap(layout);
        _modelMatrix->SetupFromLayout(layout);

        _model->SaveParameters();

        // 预载全部动作
        for (csmInt32 g = 0; g < setting->GetMotionGroupCount(); g++) {
            const csmChar* group = setting->GetMotionGroupName(g);
            PreloadMotionGroup(group);
        }
        _motionManager->StopAllMotions();

        _updating = false;
        _initialized = true;
    }

    // 参照官方 LAppModel::PreloadMotionGroup
    void PreloadMotionGroup(const csmChar* group) {
        const csmInt32 count = _setting->GetMotionCount(group);
        for (csmInt32 i = 0; i < count; i++) {
            csmString file_name = _setting->GetMotionFileName(group, i);
            std::string path = _modelHomeDir + file_name.GetRawString();
            csmSizeInt size = 0;
            csmByte* buffer = CreateBuffer(path.c_str(), &size);
            if (!buffer) continue;

            CubismMotion* motion = static_cast<CubismMotion*>(
                LoadMotion(buffer, size, nullptr));
            if (motion) {
                csmFloat32 fade = _setting->GetMotionFadeInTimeValue(group, i);
                if (fade >= 0.0f) motion->SetFadeInTime(fade);
                fade = _setting->GetMotionFadeOutTimeValue(group, i);
                if (fade >= 0.0f) motion->SetFadeOutTime(fade);
                motion->SetEffectIds(_eyeBlinkIds, _lipSyncIds);
                // 动作由 _motions 按 "group_index" 持有
            }
            DeleteBuffer(buffer);
        }
    }

    // 绑定 GL 纹理
    void SetupTextures() {
        CreateRenderer(2, CubismRenderer::CubismTextureColorFormat::CubismTextureFormat_RGBA8888);
        GLRenderer* renderer = GetRenderer<GLRenderer>();
        if (!renderer) return;

        const csmInt32 count = _setting->GetTextureCount();
        for (csmInt32 i = 0; i < count; i++) {
            std::string path = _modelHomeDir + _setting->GetTextureFileName(i);
            GLuint tex = CreateTextureFromPng(path.c_str(), nullptr, nullptr);
            if (tex) renderer->BindTexture(i, tex);
        }
        renderer->IsPremultipliedAlpha(false);
    }

    CubismModelSettingJson* _setting;
    std::string _modelHomeDir;
    csmFloat32 _deltaTimeSeconds = 0.0f;
};

// ---------------------------------------------------------------------------
// Live2DRenderer PIMPL 桥接
// ---------------------------------------------------------------------------
struct Live2DRenderer::Impl {
    PetModel* model = nullptr;
    int vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
    bool loaded = false;
};

static Allocator* g_allocator = nullptr;

bool Live2DRenderer::frameworkInit() {
    if (!g_allocator) g_allocator = new Allocator();

    CubismFramework::Option option;
    option.LogLevel = CubismFramework::Option::LogLevel_Warning;
    CubismFramework::StartUp(g_allocator, &option);
    CubismFramework::Initialize();
    return true;
}

void Live2DRenderer::frameworkDispose() {
    CubismFramework::Dispose();
    // allocator 由 CubismFramework::CleanUp 释放；此处简化不重复释放
}

Live2DRenderer::Live2DRenderer() : impl_(std::make_unique<Impl>()) {}

Live2DRenderer::~Live2DRenderer() {
    if (impl_->model) {
        CSM_DELETE impl_->model;
        impl_->model = nullptr;
    }
}

bool Live2DRenderer::loadModel(const std::string& model_dir, const std::string& model_name) {
    if (impl_->model) {
        CSM_DELETE impl_->model;
        impl_->model = nullptr;
    }

    impl_->model = CSM_NEW PetModel();
    std::string json = model_name + ".model3.json";
    if (!impl_->model->LoadAssets(model_dir, json)) {
        CSM_DELETE impl_->model;
        impl_->model = nullptr;
        impl_->loaded = false;
        return false;
    }
    impl_->loaded = true;
    printf("[Live2D] model loaded: %s/%s\n", model_dir.c_str(), json.c_str());
    return true;
}

void Live2DRenderer::playMotion(const std::string& group, int index) {
    if (!impl_->loaded || group.empty()) return;
    impl_->model->PlayMotionGroup(group, index);
}

void Live2DRenderer::update(float delta_seconds) {
    if (!impl_->loaded) return;
    impl_->model->UpdateModel(delta_seconds);
}

void Live2DRenderer::render() {
    if (!impl_->loaded) return;
    glViewport(impl_->vp_x, impl_->vp_y, impl_->vp_w, impl_->vp_h);
    impl_->model->Draw(impl_->vp_w, impl_->vp_h);
}

void Live2DRenderer::setViewport(int x, int y, int w, int h) {
    impl_->vp_x = x; impl_->vp_y = y; impl_->vp_w = w; impl_->vp_h = h;
}

bool Live2DRenderer::isLoaded() const { return impl_->loaded; }

} // namespace dutyon
