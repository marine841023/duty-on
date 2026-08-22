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

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>

// GL 头文件：PC 由 GLEW 引入桌面 OpenGL，设备用 OpenGL ES
#ifdef _WIN32
#define GLFW_INCLUDE_NONE   // 阻止 glfw3.h 包含 <GL/gl.h>（与 glew.h 冲突）
#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <windows.h>   // GetModuleFileNameA（FrameworkLoadFile 解析 shader 路径）
#else
#include <GLES3/gl3.h>
#endif

// ---- Cubism Framework ----
#include <CubismFramework.hpp>
#include <ICubismAllocator.hpp>
#include <Model/CubismUserModel.hpp>
#include <CubismModelSettingJson.hpp>   // SDK 5 起移至 Framework/src 根目录
#include <Motion/CubismMotionManager.hpp>
#include <Motion/CubismMotion.hpp>
#include <Physics/CubismPhysics.hpp>
#include <CubismDefaultParameterId.hpp>
#include <Utils/CubismString.hpp>
#include <Id/CubismIdManager.hpp>
#include <Id/CubismId.hpp>
#include <Type/csmMap.hpp>
#include <Type/csmVector.hpp>
// SDK 5 起 PC/设备统一为 OpenGLES2 渲染器（Windows 下经 CSM_TARGET_WIN_GL
// 走桌面 OpenGL，由 GLEW 提供函数指针）
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>
using GLRenderer = Live2D::Cubism::Framework::Rendering::CubismRenderer_OpenGLES2;

// 注意：不定义 STBI_ONLY_PNG —— 缩略图路径可能指向 GIF（形象首帧），
// 需要保留 GIF 解码能力
#define STB_IMAGE_IMPLEMENTATION
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
//
// 显存优化：模型贴图常为 2048×2048（RGBA + mipmap ≈ 21MB/张），而角色
// 显示视口仅 ~300px（含 DPI 缩放最大 ~500px）。上传前按 2 的幂减半到
// ≤1024：显存约降到 1/4（16MB → 4MB/张），且仍为显示尺寸的 2~3 倍，
// 视觉无差异；mipmap 保留（1024 下开销仅 1.3MB，防动画边缘闪烁）。
// ---------------------------------------------------------------------------
// 模型贴图最大边长（显示视口 ~500px 的 2 倍余量）
static constexpr int kMaxTextureSize = 1024;

// 就地 2×2 box filter 减半：目标像素写入位置恒不越过其读取的源像素
// （首像素 (0,0) 先聚合后写入），因此可直接在原缓冲上前半段原地覆盖，
// 零额外分配。奇数边长时末行/末列被丢弃（与 mipmap 一致）。
bool HalveRGBA(unsigned char* data, int* w, int* h) {
    if (!data || !w || !h) return false;
    const int sw = *w, sh = *h;
    if (sw <= 1 && sh <= 1) return false;
    const int nw = sw > 1 ? sw / 2 : 1;
    const int nh = sh > 1 ? sh / 2 : 1;
    for (int y = 0; y < nh; y++) {
        const int y0 = y * 2, y1 = y0 + 1 < sh ? y0 + 1 : y0;
        unsigned char* dst = data + (size_t)y * nw * 4;
        const unsigned char* r0 = data + (size_t)y0 * sw * 4;
        const unsigned char* r1 = data + (size_t)y1 * sw * 4;
        for (int x = 0; x < nw; x++) {
            const int x0 = x * 2 * 4, x1 = x0 + 4 < sw * 4 ? x0 + 4 : x0;
            for (int c = 0; c < 4; c++) {
                // 四舍五入的平均（(a+b+c+d+2)/4），与 GL mipmap 精度接近
                dst[c] = (unsigned char)((r0[x0 + c] + r0[x1 + c] + r1[x0 + c] +
                                          r1[x1 + c] + 2) >> 2);
            }
            dst += 4;
        }
    }
    *w = nw;
    *h = nh;
    return true;
}

static GLuint CreateTextureFromPng(const char* path, int* out_w, int* out_h) {
    int w, h, channels;
    unsigned char* data = stbi_load(path, &w, &h, &channels, STBI_rgb_alpha);
    if (!data) {
        fprintf(stderr, "[Live2D] texture load failed: %s\n", path);
        return 0;
    }
    while (w > kMaxTextureSize || h > kMaxTextureSize) {
        if (!HalveRGBA(data, &w, &h)) break;  // 已到 1px：按当前尺寸上传
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
// 动作优先级（对应官方 Sample LAppDefine 的 MotionPriority 常量）
enum MotionPriority {
    MotionPriorityNone = 0,
    MotionPriorityIdle = 1,
    MotionPriorityNormal = 2,
    MotionPriorityForce = 3
};

class PetModel : public CubismUserModel {
public:
    PetModel() : CubismUserModel(), _setting(nullptr) {}
    ~PetModel() override {
        // SDK 5 的 csmMap 只有 const_iterator（Begin/End 均为 const）
        for (csmMap<csmString, ACubismMotion*>::const_iterator it = _motions.Begin(); it != _motions.End(); ++it) {
            ACubismMotion::Delete((*it).Second);
        }
        _motions.Clear();
        for (csmMap<csmString, ACubismMotion*>::const_iterator it = _expressions.Begin(); it != _expressions.End(); ++it) {
            ACubismMotion::Delete((*it).Second);
        }
        _expressions.Clear();
        if (_setting) { CSM_DELETE(_setting); _setting = nullptr; }
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

        // 动作（状态循环动作优先，否则待机 Idle 随机）
        _model->LoadParameters();
        if (_motionManager->IsFinished()) {
            if (_loopGroup.empty()) {
                StartRandomMotion("Idle", MotionPriorityIdle);
            } else {
                // 状态循环动作播完自动重播（对应 1.x playStateMotion 的
                // motionFinish 重触发）
                StartMotion(_loopGroup.c_str(), _loopIndex, MotionPriorityIdle);
            }
        }
        const bool motion_updated = _motionManager->UpdateMotion(_model, delta_seconds);
        _model->SaveParameters();
        if (!motion_updated && _eyeBlink) {
            _eyeBlink->UpdateParameters(_model, delta_seconds);
        }

        if (_physics) _physics->Evaluate(_model, delta_seconds);
        if (_breath)   _breath->UpdateParameters(_model, delta_seconds);
        if (_pose)     _pose->UpdateParameters(_model, delta_seconds);

        _model->Update();

        // 首帧姿态就绪后计算内容包围盒（对应 1.x refineContentFit 的一次性测量）
        if (!_contentReady && _model) {
            ComputeContentBounds();
            _contentReady = true;
        }
    }

    // 设置状态循环动作（组不存在时回退 Idle）
    void SetLoopMotion(const std::string& group, int index) {
        if (_setting && _setting->GetMotionCount(group.c_str()) <= 0) {
            _loopGroup.clear();
            _loopIndex = 0;
            return;
        }
        _loopGroup = group;
        _loopIndex = index;
    }

    // 动作目录（播放动作/动作设定菜单数据源，对应 1.x refreshMotionGroups）
    std::vector<std::pair<std::string, int>> GetMotionGroups() const {
        std::vector<std::pair<std::string, int>> out;
        if (!_setting) return out;
        for (csmInt32 g = 0; g < _setting->GetMotionGroupCount(); g++) {
            const csmChar* group = _setting->GetMotionGroupName(g);
            const csmInt32 count = _setting->GetMotionCount(group);
            if (count > 0) out.emplace_back(std::string(group), count);
        }
        return out;
    }

    // 播放指定组动作
    void PlayMotionGroup(const std::string& group, int index) {
        StartMotion(group.c_str(), index, MotionPriorityForce);
    }

    // 参照官方 LAppModel::StartMotion（动作已在 PreloadMotionGroup 全部加载）
    CubismMotionQueueEntryHandle StartMotion(const csmChar* group, csmInt32 index, csmInt32 priority) {
        // 组名与模型解耦：请求的组不存在/索引越界时回退到 Idle 组
        // （不同模型的动作组命名不同，如 ni-j 用 FlickLeft，miku 用 Flick）
        if (_setting->GetMotionCount(group) <= 0 ||
            index >= _setting->GetMotionCount(group)) {
            group = "Idle";
            const csmInt32 idle_count = _setting->GetMotionCount(group);
            if (idle_count <= 0) return InvalidMotionQueueEntryHandleValue;
            index = rand() % idle_count;
        }

        if (priority == MotionPriorityForce) {
            _motionManager->SetReservePriority(priority);
        } else if (!_motionManager->ReserveMotion(priority)) {
            return InvalidMotionQueueEntryHandleValue;
        }

        const csmString name = Utils::CubismString::GetFormatedString("%s_%d", group, index);
        CubismMotion* motion = static_cast<CubismMotion*>(_motions[name.GetRawString()]);
        if (!motion) {
            fprintf(stderr, "[Live2D] motion not found: %s\n", name.GetRawString());
            return InvalidMotionQueueEntryHandleValue;
        }
        return _motionManager->StartMotionPriority(motion, false, priority);
    }

    // 组内随机播放（SDK 5 的 CubismMotionManager 移除了 StartRandomMotion，自行实现）
    CubismMotionQueueEntryHandle StartRandomMotion(const csmChar* group, csmInt32 priority) {
        const csmInt32 count = _setting->GetMotionCount(group);
        if (count <= 0) return InvalidMotionQueueEntryHandleValue;
        return StartMotion(group, rand() % count, priority);
    }

    // 渲染（调用前确保 GL 上下文 current、viewport 已设）
    // 适配算法对齐 1.x refineContentFit：
    //   scale = min(视口宽/内容宽, 视口高/内容高) * 0.72
    //   内容底边贴视口底边，水平居中
    void Draw(int window_w, int window_h, bool flip, Rect* out_content_rect) {
        if (!_model || window_w <= 0 || window_h <= 0) return;

        const csmFloat32 cw = _contentMaxX - _contentMinX;   // 内容宽（模型单位）
        const csmFloat32 ch = _contentMaxY - _contentMinY;   // 内容高
        // 退化保护：内容为空（未测量）时按整个画布适配
        csmFloat32 src_min_y = _contentMinY, src_cx = (_contentMinX + _contentMaxX) * 0.5f;
        csmFloat32 use_cw = cw, use_ch = ch;
        if (use_cw <= 0.0f || use_ch <= 0.0f) {
            const csmFloat32 canvas_w = _model->GetCanvasWidth();
            const csmFloat32 canvas_h = _model->GetCanvasHeight();
            use_cw = canvas_w > 0.0f ? canvas_w : 1.0f;
            use_ch = canvas_h > 0.0f ? canvas_h : 1.0f;
            src_min_y = -use_ch * 0.5f;
            src_cx = 0.0f;
        }

        // 像素/模型单位；0.72 系数与 1.x 一致（留出边距，角色不满贴画布）
        const csmFloat32 scale_px =
            (std::min)(static_cast<csmFloat32>(window_w) / use_cw,
                       static_cast<csmFloat32>(window_h) / use_ch) * 0.72f;

        // 逻辑坐标系（视口高 = 2）下的缩放：1 逻辑单位 = 视口高/2 像素
        const csmFloat32 scale_log = scale_px * 2.0f / static_cast<csmFloat32>(window_h);

        // 画布高（逻辑单位）—— SetHeight 保持纵横比，宽度随之确定
        const csmFloat32 canvas_h_log =
            (_model->GetCanvasHeight() > 0.0f ? _model->GetCanvasHeight() : 1.0f) * scale_log;

        // 画布中心逻辑坐标：内容底边 -> -1（视口底），内容中心 X -> 0（水平居中）
        const csmFloat32 center_x_log = -src_cx * scale_log;
        const csmFloat32 center_y_log = -1.0f - src_min_y * scale_log;

        _modelMatrix->SetHeight(canvas_h_log);
        // 平移：画布原点(0,0) -> (center_x_log, center_y_log)。
        // 不能用 SetCenterPosition —— 它按「画布盒 [0..w]x[0..h]」语义做
        // TranslateX(x - w/2)，而模型顶点是画布中心原点坐标，会把内容
        // 整体向左下推半个画布（FitDBG 实测：底边 NDC -2.0、中心 X 0.98，
        // 角色被右/下边缘裁掉）。直接写平移分量：
        //   内容底边 src_min_y*s + ty = -1，内容中心 src_cx*s + tx = 0
        csmFloat32* mm = _modelMatrix->GetArray();
        mm[12] = center_x_log;
        mm[13] = center_y_log;

        // 投影：逻辑坐标（等比、Y∈[-1,1]）-> NDC；翻转 = X 取负
        CubismMatrix44 projection;
        projection.Scale((flip ? -1.0f : 1.0f) *
                             static_cast<csmFloat32>(window_h) / window_w,
                         1.0f);

        // 官方 LAppModel::Draw：projection × modelMatrix
        projection.MultiplyByMatrix(_modelMatrix);

        GLRenderer* renderer = GetRenderer<GLRenderer>();
        if (renderer) {
            renderer->SetMvpMatrix(&projection);
            renderer->DrawModel();
        }

        // 内容包围盒（视口像素，Y 向下）供头饰特效/命中测试使用。
        // 翻转 = 内容绕竖直中轴镜像：左右边缘对调（特效锚点跟随）
        if (out_content_rect) {
            const float left_px =
                window_w * 0.5f + (_contentMinX - src_cx) * scale_px;
            const float content_h_px = use_ch * scale_px;
            out_content_rect->x =
                flip ? window_w - left_px - use_cw * scale_px : left_px;
            out_content_rect->y = static_cast<float>(window_h) - content_h_px;
            out_content_rect->w = use_cw * scale_px;
            out_content_rect->h = content_h_px;
        }
    }

private:
    // 内容包围盒（模型单位，Y 向上，原点为画布中心）。
    // 遍历全部 drawable 的顶点取极值 —— 与 1.x pixi getBounds 等价，
    // 画布边缘常有大片空白（如 shizuku），按内容适配才能得到与 1.x 一致的大小。
    void ComputeContentBounds() {
        _contentMinX = _contentMinY = FLT_MAX;
        _contentMaxX = _contentMaxY = -FLT_MAX;
        const csmInt32 count = _model->GetDrawableCount();
        bool any = false;
        for (csmInt32 i = 0; i < count; i++) {
            const csmInt32 vertex_count = _model->GetDrawableVertexCount(i);
            // Cubism 5：顶点数组是 Core::csmVector2（X/Y 为 float 字段）
            const auto* positions = _model->GetDrawableVertexPositions(i);
            if (!positions || vertex_count <= 0) continue;
            any = true;
            for (csmInt32 v = 0; v < vertex_count; v++) {
                const csmFloat32 x = positions[v].X;
                const csmFloat32 y = positions[v].Y;
                if (x < _contentMinX) _contentMinX = x;
                if (x > _contentMaxX) _contentMaxX = x;
                if (y < _contentMinY) _contentMinY = y;
                if (y > _contentMaxY) _contentMaxY = y;
            }
        }
        if (!any) {
            _contentMinX = _contentMinY = -1.0f;
            _contentMaxX = _contentMaxY = 1.0f;
        }
        printf("[Live2D] content bounds: x[%f..%f] y[%f..%f]\n",
               _contentMinX, _contentMaxX, _contentMinY, _contentMaxY);
    }
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
                    ACubismMotion* motion = LoadExpression(buffer, size, name.GetRawString());
                    if (motion) {
                        _expressions[name] = motion;
                    }
                    DeleteBuffer(buffer);
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

        // 呼吸参数 ID（已 using namespace Live2D::Cubism::Framework，可直接用嵌套命名空间）
        _idParamAngleX    = CubismFramework::GetIdManager()->GetId(DefaultParameterId::ParamAngleX);
        _idParamAngleY    = CubismFramework::GetIdManager()->GetId(DefaultParameterId::ParamAngleY);
        _idParamAngleZ    = CubismFramework::GetIdManager()->GetId(DefaultParameterId::ParamAngleZ);
        _idParamBodyAngleX= CubismFramework::GetIdManager()->GetId(DefaultParameterId::ParamBodyAngleX);
        _idParamBreath    = CubismFramework::GetIdManager()->GetId(DefaultParameterId::ParamBreath);

        // 眨眼/唇形同步参数 ID（动作播放时用于排除对应参数）
        _eyeBlinkIds.PushBack(CubismFramework::GetIdManager()->GetId(DefaultParameterId::ParamEyeLOpen));
        _eyeBlinkIds.PushBack(CubismFramework::GetIdManager()->GetId(DefaultParameterId::ParamEyeROpen));
        _lipSyncIds.PushBack(CubismFramework::GetIdManager()->GetId(DefaultParameterId::ParamMouthOpenY));

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
            // 动作在 _motions 中以 "group_i" 为 key 持有
            csmString name = Utils::CubismString::GetFormatedString("%s_%d", group, i);
            csmString file_name = _setting->GetMotionFileName(group, i);
            std::string path = _modelHomeDir + file_name.GetRawString();
            csmSizeInt size = 0;
            csmByte* buffer = CreateBuffer(path.c_str(), &size);
            if (!buffer) continue;

            CubismMotion* motion = static_cast<CubismMotion*>(
                LoadMotion(buffer, size, name.GetRawString()));
            if (motion) {
                csmFloat32 fade = _setting->GetMotionFadeInTimeValue(group, i);
                if (fade >= 0.0f) motion->SetFadeInTime(fade);
                fade = _setting->GetMotionFadeOutTimeValue(group, i);
                if (fade >= 0.0f) motion->SetFadeOutTime(fade);
                motion->SetEffectIds(_eyeBlinkIds, _lipSyncIds);
                if (_motions[name] != nullptr) ACubismMotion::Delete(_motions[name]);
                _motions[name] = motion;
            }
            DeleteBuffer(buffer);
        }
    }

    // 绑定 GL 纹理
    void SetupTextures() {
        CreateRenderer(2);
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

    CubismIdHandle _idParamAngleX = nullptr;
    CubismIdHandle _idParamAngleY = nullptr;
    CubismIdHandle _idParamAngleZ = nullptr;
    CubismIdHandle _idParamBodyAngleX = nullptr;
    CubismIdHandle _idParamBreath = nullptr;
    csmVector<CubismIdHandle> _eyeBlinkIds;
    csmVector<CubismIdHandle> _lipSyncIds;
    csmMap<csmString, ACubismMotion*> _motions;
    csmMap<csmString, ACubismMotion*> _expressions;

    // 状态循环动作（对应 1.x STATE_MOTIONS；空 = Idle 随机循环）
    std::string _loopGroup;
    int _loopIndex = 0;

    // 内容包围盒（模型单位）与就绪标志
    bool _contentReady = false;
    csmFloat32 _contentMinX = 0.0f, _contentMaxX = 0.0f;
    csmFloat32 _contentMinY = 0.0f, _contentMaxY = 0.0f;

    ICubismModelSetting* _setting;
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
    bool flip = false;
    Rect content_rect;   // 视口坐标系（Y 向下），render() 每帧刷新
};

static Allocator* g_allocator = nullptr;

// ---------------------------------------------------------------------------
// Cubism Framework 文件加载回调（SDK 5 的 OpenGL shader 在运行时从文件加载：
// "FrameworkShaders/VertShaderSrc.vert" 等相对路径，这里解析到 exe 同目录）
// ---------------------------------------------------------------------------
static csmByte* FrameworkLoadFile(const std::string path, csmSizeInt* out_size) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // 候选：原样（相对工作目录）-> exe 同目录
    if (fs::exists(path, ec)) {
        return CreateBuffer(path.c_str(), out_size);
    }
#ifdef _WIN32
    char exe_path[1024];
    DWORD len = GetModuleFileNameA(nullptr, exe_path, sizeof(exe_path));
    if (len > 0 && len < sizeof(exe_path)) {
        fs::path full = fs::path(exe_path).parent_path() / path;
        if (fs::exists(full, ec)) {
            return CreateBuffer(full.generic_string().c_str(), out_size);
        }
    }
#endif
    fprintf(stderr, "[Live2D] framework file not found: %s\n", path.c_str());
    return nullptr;
}

static void FrameworkReleaseBytes(csmByte* data) { DeleteBuffer(data); }

// SDK 日志转发（不注册的话 CubismLogError 全被吞掉，shader/GL 函数加载失败无从得知）
static void FrameworkLog(const csmChar* message) {
    fprintf(stderr, "[Cubism] %s\n", message);
}

bool Live2DRenderer::frameworkInit() {
    if (!g_allocator) g_allocator = new Allocator();

    // 注意：CubismFramework::StartUp 只保存 Option 指针（内部 s_option 不拷贝），
    // 传入栈上局部变量会在函数返回后悬空——必须用静态/全局存储
    // （官方 Sample 里 Option 是 LAppDelegate 的成员变量）。
    static CubismFramework::Option option;
    option.LogFunction = FrameworkLog;
    option.LoggingLevel = CubismFramework::Option::LogLevel_Verbose;
    // shader 文件加载回调——不注册的话所有 ShaderProgram 为 0，DrawModel 静默不画
    option.LoadFileFunction = FrameworkLoadFile;
    option.ReleaseBytesFunction = FrameworkReleaseBytes;
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
        CSM_DELETE(impl_->model);
        impl_->model = nullptr;
    }
}

// 在 root 下解析名为 name 的模型，返回 (目录, json文件名)。
// 兼容布局：
//   1. <root>/<name>.model3.json            （扁平）
//   2. <root>/<name>/<name>.model3.json
//   3. <root>/<name>/runtime/*.model3.json  （官方样本布局，json 名与目录名不同）
//   4. <root>/<name>/*.model3.json
static bool ResolveModelJson(const std::string& root, const std::string& name,
                             std::string& out_dir, std::string& out_file) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string json_name = name + ".model3.json";

    const fs::path flat = fs::path(root) / json_name;
    if (fs::exists(flat, ec)) {
        out_dir = root;
        out_file = json_name;
        return true;
    }
    const fs::path nested = fs::path(root) / name / json_name;
    if (fs::exists(nested, ec)) {
        out_dir = (fs::path(root) / name).generic_string();
        out_file = json_name;
        return true;
    }
    for (const fs::path sub : {fs::path(root) / name / "runtime", fs::path(root) / name}) {
        if (!fs::is_directory(sub, ec)) continue;
        for (const auto& entry : fs::directory_iterator(sub, ec)) {
            const std::string fn = entry.path().filename().string();
            const std::string suffix = ".model3.json";
            if (fn.size() >= suffix.size() &&
                fn.compare(fn.size() - suffix.size(), suffix.size(), suffix) == 0) {
                out_dir = sub.generic_string();
                out_file = fn;
                return true;
            }
        }
    }
    return false;
}

bool Live2DRenderer::loadModel(const std::string& model_dir, const std::string& model_name) {
    std::string dir, json;
    if (!ResolveModelJson(model_dir, model_name, dir, json)) {
        fprintf(stderr, "[Live2D] model not found: %s/%s(.model3.json)\n",
                model_dir.c_str(), model_name.c_str());
        impl_->loaded = false;
        return false;
    }
    return loadModelFile(dir, json);
}

bool Live2DRenderer::loadModelFile(const std::string& model_dir, const std::string& json_name) {
    if (impl_->model) {
        CSM_DELETE(impl_->model);
        impl_->model = nullptr;
    }
    impl_->model = CSM_NEW PetModel();
    if (!impl_->model->LoadAssets(model_dir, json_name)) {
        CSM_DELETE(impl_->model);
        impl_->model = nullptr;
        impl_->loaded = false;
        return false;
    }
    impl_->loaded = true;
    printf("[Live2D] model loaded: %s/%s\n", model_dir.c_str(), json_name.c_str());
    return true;
}

void Live2DRenderer::playMotion(const std::string& group, int index) {
    if (!impl_->loaded || group.empty()) return;
    impl_->model->PlayMotionGroup(group, index);
}

void Live2DRenderer::setLoopMotion(const std::string& group, int index) {
    if (!impl_->loaded) return;
    impl_->model->SetLoopMotion(group, index);
}

void Live2DRenderer::update(float delta_seconds) {
    if (!impl_->loaded) return;
    impl_->model->UpdateModel(delta_seconds);
}

void Live2DRenderer::render() {
    if (!impl_->loaded) return;
#ifdef _WIN32
    // ImGui 初始化/渲染会留下自己的 VAO 绑定，而 Cubism 渲染器使用
    // client-side 顶点/索引数组（仅在默认 VAO 下合法）——若不解绑，
    // glDrawElements 会得到 GL_INVALID_OPERATION，模型一个像素都画不出来
    glBindVertexArray(0);
#endif
    glViewport(impl_->vp_x, impl_->vp_y, impl_->vp_w, impl_->vp_h);
    impl_->model->Draw(impl_->vp_w, impl_->vp_h, impl_->flip, &impl_->content_rect);
}

void Live2DRenderer::setViewport(int x, int y, int w, int h) {
    impl_->vp_x = x; impl_->vp_y = y; impl_->vp_w = w; impl_->vp_h = h;
}

void Live2DRenderer::setFlip(bool flip) { impl_->flip = flip; }
bool Live2DRenderer::isFlipped() const { return impl_->flip; }

std::vector<MotionGroupInfo> Live2DRenderer::motionGroups() const {
    std::vector<MotionGroupInfo> out;
    if (!impl_->loaded) return out;
    for (const auto& [group, count] : impl_->model->GetMotionGroups()) {
        out.push_back(MotionGroupInfo{group, count});
    }
    return out;
}

Rect Live2DRenderer::contentRect() const { return impl_->content_rect; }

bool Live2DRenderer::isLoaded() const { return impl_->loaded; }

} // namespace dutyon
