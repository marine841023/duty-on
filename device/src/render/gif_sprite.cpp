// GIF 自定义形象渲染器（Windows / WIC + OpenGL）。
//
// 解码：IWICBitmapDecoder 逐帧读取，按 GIF89a 处置方式（disposal）合成到
// 全画布 BGRA 缓冲 —— 浏览器 <img> 的等价行为（1.x 即浏览器渲染）。
//   disposal 0/1：帧保留在画布上（下一帧叠加）
//   disposal 2  ：帧区域恢复为透明（背景）
//   disposal 3  ：帧区域恢复为绘制前的内容
// 帧延迟 "/grctlext/Delay"（10ms 单位）；<=10ms 按浏览器惯例取 100ms。
//
// 内存优化：不再保存每帧全画布快照（480×480×4 ≈ 0.9MB/帧，百帧 GIF
// 可达 ~90MB），改为保存「相邻帧差分脏矩形」——播放时在一个工作画布上
// 依次应用各帧的差分即可还原任意帧。典型内容（背景静止、局部运动）
// 内存降 5~20 倍；全画面每帧都变的极端情形也只多一份画布（+0.9MB）。
//
// 渲染：单张 GL_TEXTURE_2D，首帧 glTexImage2D 全量上传，此后帧切换只
// glTexSubImage2D 脏矩形（帧间不变的区域零重传，降低 PCIe 带宽占用）。
// 固定管线正交投影绘制四边形（兼容配置文件 + 简单场景足够，与 Cubism
// 的 GL 状态互不干扰）。

#include "render/gif_sprite.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _WIN32

#include <GL/glew.h>

#include <objbase.h>
#include <wincodec.h>
#include <windows.h>
#include <compressapi.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "cabinet.lib")

namespace dutyon {

namespace {

// 读帧元数据（/imgdesc/... 与 /grctlext/...）；缺省值覆盖常规情形
struct FrameMeta {
    UINT32 left = 0, top = 0, width = 0, height = 0;  // 帧矩形
    UINT32 delay_ms = 100;                             // 显示时长
    UINT32 disposal = 1;                               // 处置方式
};

bool QueryUint(IWICMetadataQueryReader* qr, const wchar_t* name, UINT32* out) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    if (FAILED(qr->GetMetadataByName(name, &pv))) return false;
    bool ok = false;
    if (pv.vt == VT_UI2) { *out = pv.uiVal; ok = true; }
    else if (pv.vt == VT_UI4) { *out = pv.ulVal; ok = true; }
    else if (pv.vt == VT_I2) { *out = (UINT32)pv.iVal; ok = true; }
    else if (pv.vt == VT_I4) { *out = (UINT32)pv.lVal; ok = true; }
    else if (pv.vt == VT_UI1) { *out = pv.bVal; ok = true; }
    PropVariantClear(&pv);
    return ok;
}

FrameMeta ReadFrameMeta(IWICBitmapFrameDecode* frame) {
    FrameMeta m;
    IWICMetadataQueryReader* qr = nullptr;
    if (FAILED(frame->GetMetadataQueryReader(&qr)) || !qr) return m;
    QueryUint(qr, L"/imgdesc/Left", &m.left);
    QueryUint(qr, L"/imgdesc/Top", &m.top);
    QueryUint(qr, L"/imgdesc/Width", &m.width);
    QueryUint(qr, L"/imgdesc/Height", &m.height);
    UINT32 delay = 0;
    if (QueryUint(qr, L"/grctlext/Delay", &delay)) {
        // 10ms 单位；过小值按浏览器 <img> 惯例取 100ms
        m.delay_ms = delay * 10;
        if (m.delay_ms < 20) m.delay_ms = 100;
    }
    QueryUint(qr, L"/grctlext/Disposal", &m.disposal);
    qr->Release();
    return m;
}

}  // namespace

// XPRESS_HUFF 压缩（系统 cabinet.dll，零第三方依赖）。动画帧大面积平坦
// 像素（透明/纯色背景）压缩率通常 3~10x：帧间全变的全画布 GIF 差分从
// ~45MB 降到个位数 MB。
static bool CompressBuf(const std::vector<uint8_t>& src,
                        std::vector<uint8_t>& dst) {
    COMPRESSOR_HANDLE h = nullptr;
    if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &h))
        return false;
    // XPRESS 最坏输出略大于输入（块头+对齐）：直接分配输入+4KB 余量一次
    // 压缩（dst=NULL 的「查询大小」调用并不可靠，会导致全部回退不压缩）
    dst.resize(src.size() + 4096);
    SIZE_T out_size = 0;
    const bool ok = Compress(h, src.data(), src.size(), dst.data(), dst.size(),
                             &out_size);
    CloseCompressor(h);
    if (!ok || out_size == 0 || out_size >= src.size()) {
        dst.clear();
        return false;  // 压缩失败或无收益（随机数据）
    }
    dst.resize(out_size);
    dst.shrink_to_fit();  // resize 缩小不释放 capacity（否则每帧仍占全尺寸）
    return true;
}

static bool DecompressBuf(const std::vector<uint8_t>& src, size_t raw_size,
                          std::vector<uint8_t>& dst) {
    COMPRESSOR_HANDLE h = nullptr;
    if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &h))
        return false;
    dst.resize(raw_size);
    SIZE_T out_size = 0;
    const bool ok = Decompress(h, src.data(), src.size(), dst.data(),
                               dst.size(), &out_size) &&
                    out_size == raw_size;
    CloseDecompressor(h);
    return ok;
}

// 相邻显示帧之间的差分（脏矩形 + BGRA 像素，XPRESS 压缩存储）；w=0 表示
// 两帧完全相同
struct GifFrameDelta {
    int x = 0, y = 0, w = 0, h = 0;
    size_t raw = 0;               // 未压缩字节数（w*h*4）
    std::vector<uint8_t> px;      // 压缩字节（压缩失败时=原始数据）
    bool compressed = false;
};

// 计算两幅全画布快照的差分脏矩形（b 相对 a 变化的最小包围盒 + 像素）
static GifFrameDelta MakeDiff(const std::vector<uint8_t>& a,
                              const std::vector<uint8_t>& b, int w, int h) {
    GifFrameDelta d;
    int minx = w, miny = h, maxx = -1, maxy = -1;
    const size_t row_bytes = (size_t)w * 4;
    for (int y = 0; y < h; y++) {
        const uint8_t* ra = a.data() + y * row_bytes;
        const uint8_t* rb = b.data() + y * row_bytes;
        if (memcmp(ra, rb, row_bytes) == 0) continue;  // 整行相同快速跳过
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
        for (int x = 0; x < w; x++) {
            if (ra[x * 4 + 0] != rb[x * 4 + 0] || ra[x * 4 + 1] != rb[x * 4 + 1] ||
                ra[x * 4 + 2] != rb[x * 4 + 2] || ra[x * 4 + 3] != rb[x * 4 + 3]) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
            }
        }
    }
    if (maxx >= 0) {
        d.x = minx;
        d.y = miny;
        d.w = maxx - minx + 1;
        d.h = maxy - miny + 1;
        d.raw = (size_t)d.w * d.h * 4;
        std::vector<uint8_t> rawpx(d.raw);
        for (int y = 0; y < d.h; y++)
            memcpy(rawpx.data() + (size_t)y * d.w * 4,
                   b.data() + ((size_t)(d.y + y) * w + d.x) * 4,
                   (size_t)d.w * 4);
        // 压缩无收益（小块/随机噪声）时保留原始数据
        if (CompressBuf(rawpx, d.px)) {
            d.compressed = true;
        } else {
            d.px = std::move(rawpx);
        }
    }
    return d;
}

struct GifSprite::Impl {
    // frames[i]：从上一显示帧推进到第 i 帧要应用的差分
    //（i>=1 为 snap[i-1]→snap[i]；frames[0] 为循环回绕 snap[n-1]→snap[0]）
    std::vector<GifFrameDelta> frames;
    std::vector<float> delays;                 // 秒
    std::vector<uint8_t> canvas;               // 当前显示帧的合成画布（BGRA）
    std::vector<uint8_t> scratch;              // 帧解压缓冲（复用，避免每帧分配）
    int w = 0, h = 0;
    int cur = 0;
    double elapsed = 0.0;          // 当前帧已播放时长
    GLuint tex = 0;
    bool tex_valid = false;        // tex 已含 canvas 内容
    Rect dirty;                    // 待上传脏矩形（tex_valid 后累积）
    bool has_dirty = false;
    Rect content;                  // 绘制矩形（render 时更新）
    int vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
    bool flip = false;

    // 应用一帧差分到工作画布并累积脏矩形
    void ApplyDelta(const GifFrameDelta& d) {
        if (d.w <= 0) return;
        const uint8_t* src = d.px.data();
        if (d.compressed) {
            // 解压失败（数据损坏防御）：跳过该帧，画布保持旧内容
            if (!DecompressBuf(d.px, d.raw, scratch)) return;
            src = scratch.data();
        }
        for (int y = 0; y < d.h; y++)
            memcpy(canvas.data() + ((size_t)(d.y + y) * w + d.x) * 4,
                   src + (size_t)y * d.w * 4, (size_t)d.w * 4);
        if (has_dirty) {  // 并入已有脏区（重传少量不变像素可接受）
            const int x0 = dirty.x < d.x ? dirty.x : d.x;
            const int y0 = dirty.y < d.y ? dirty.y : d.y;
            const int x1 = (int)(dirty.x + dirty.w) > d.x + d.w
                               ? (int)(dirty.x + dirty.w) : d.x + d.w;
            const int y1 = (int)(dirty.y + dirty.h) > d.y + d.h
                               ? (int)(dirty.y + dirty.h) : d.y + d.h;
            dirty = Rect{(float)x0, (float)y0, (float)(x1 - x0), (float)(y1 - y0)};
        } else {
            dirty = Rect{(float)d.x, (float)d.y, (float)d.w, (float)d.h};
            has_dirty = true;
        }
    }

    ~Impl() {
        if (tex) { glDeleteTextures(1, &tex); tex = 0; }
    }
};

GifSprite::GifSprite() : impl_(new Impl) {}
GifSprite::~GifSprite() { delete impl_; }

bool GifSprite::isLoaded() const { return !impl_->frames.empty(); }
int GifSprite::frameCount() const { return (int)impl_->frames.size(); }
int GifSprite::width() const { return impl_->w; }
int GifSprite::height() const { return impl_->h; }
bool GifSprite::isFlipped() const { return impl_->flip; }
Rect GifSprite::contentRect() const { return impl_->content; }

void GifSprite::unload() {
    impl_->frames.clear();
    impl_->frames.shrink_to_fit();
    impl_->delays.clear();
    impl_->delays.shrink_to_fit();
    impl_->canvas.clear();
    impl_->canvas.shrink_to_fit();
    impl_->scratch.clear();
    impl_->scratch.shrink_to_fit();
    impl_->w = impl_->h = 0;
    impl_->cur = 0;
    impl_->elapsed = 0.0;
    impl_->tex_valid = false;
    impl_->has_dirty = false;
    if (impl_->tex) {
        glDeleteTextures(1, &impl_->tex);
        impl_->tex = 0;
    }
}

bool GifSprite::load(const std::string& path_utf8) {
    if (path_utf8.empty()) return false;

    // COM：主线程首次使用 WIC 时初始化（S_FALSE = 已初始化，同样要配对释放）
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_ok = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE = 别的模式，不归我们释放

    bool ok = false;
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))))
            break;

        // UTF-8 → UTF-16（路径可能含中文/用户名）
        const int wlen = MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1,
                                             nullptr, 0);
        if (wlen <= 0) break;
        std::wstring wpath((size_t)wlen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, &wpath[0], wlen);

        if (FAILED(factory->CreateDecoderFromFilename(
                wpath.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                &decoder)))
            break;

        UINT count = 0;
        decoder->GetFrameCount(&count);
        if (count == 0) break;

        // 画布尺寸 = GIF 逻辑屏幕尺寸（浏览器 <img> 的显示尺寸）。
        // 帧矩形可以小于并偏移于画布（如本例：画布 480x480，帧 461x467
        // @ (11,13)）—— 画布若误用帧尺寸，贴帧条件 left+width<=w 永远
        // 失败，所有帧都不上画布，整幅 GIF 全透明（本次 bug 根因）。
        UINT w = 0, h = 0;
        {
            IWICMetadataQueryReader* dqr = nullptr;
            if (SUCCEEDED(decoder->GetMetadataQueryReader(&dqr)) && dqr) {
                QueryUint(dqr, L"/logscrdesc/Width", &w);
                QueryUint(dqr, L"/logscrdesc/Height", &h);
                dqr->Release();
            }
        }
        if (w == 0 || h == 0) {
            // 无逻辑屏幕信息：假设帧矩形 == 画布（整幅单帧 GIF 常见）
            IWICBitmapFrameDecode* f0 = nullptr;
            if (SUCCEEDED(decoder->GetFrame(0, &f0))) {
                f0->GetSize(&w, &h);
                f0->Release();
            }
        }
        if (w == 0 || h == 0) break;

        // 画布 + 帧序列（差分构建：加载期临时保存相邻两帧快照做 diff，
        // 提交后只保留工作画布 + 各帧差分，内存远低于全帧快照）
        std::vector<GifFrameDelta> deltas;   // deltas[i] = snap[i-1]→snap[i]（i>=1）
        std::vector<float> delays;
        std::vector<uint8_t> canvas((size_t)w * h * 4, 0);
        std::vector<uint8_t> prev;           // 上一帧快照
        std::vector<uint8_t> first;          // 首帧快照（工作画布初值）
        std::vector<uint8_t> saved;   // disposal 3 的恢复缓冲
        IWICBitmapFrameDecode* frame = nullptr;
        FrameMeta prevMeta{};             // 上一帧元数据（处置其区域）
        bool have_prev = false;
        UINT32 prev_left = 0, prev_top = 0, prev_w = 0, prev_h = 0;

        for (UINT i = 0; i < count; i++) {
            if (FAILED(decoder->GetFrame(i, &frame))) break;
            FrameMeta fm = ReadFrameMeta(frame);
            // imgdesc 元数据缺失时兜底：帧矩形 = 帧尺寸 @ (0,0)
            // （整幅单帧 GIF / 元数据查询失败的防御）
            if (fm.width == 0 || fm.height == 0) {
                UINT fw = 0, fh = 0;
                frame->GetSize(&fw, &fh);
                if (fm.width == 0) fm.width = fw;
                if (fm.height == 0) fm.height = fh;
            }

            // 1) 处置上一帧（disposal 在下一帧绘制前生效）。
            //    处置区域裁剪到画布内 —— 防御帧矩形越界的异常 GIF。
            if (have_prev) {
                const UINT32 dl = prev_left < w ? prev_left : w;
                const UINT32 dt = prev_top < h ? prev_top : h;
                const UINT32 dr = (prev_left + prev_w) < w ? (prev_left + prev_w) : w;
                const UINT32 db = (prev_top + prev_h) < h ? (prev_top + prev_h) : h;
                if (dr > dl && db > dt) {
                    if (prevMeta.disposal == 2) {
                        // 恢复透明
                        for (UINT32 y = dt; y < db; y++)
                            memset(canvas.data() + ((size_t)y * w + dl) * 4, 0,
                                   (size_t)(dr - dl) * 4);
                    } else if (prevMeta.disposal == 3 && saved.size() == canvas.size()) {
                        // 恢复绘制前内容
                        for (UINT32 y = dt; y < db; y++)
                            memcpy(canvas.data() + ((size_t)y * w + dl) * 4,
                                   saved.data() + ((size_t)y * w + dl) * 4,
                                   (size_t)(dr - dl) * 4);
                    }
                }
            }

            // 2) disposal 3：绘制前保存区域内容
            if (fm.disposal == 3) {
                if (saved.size() != canvas.size()) saved.resize(canvas.size());
                memcpy(saved.data(), canvas.data(), canvas.size());
            }

            // 3) 解码帧 → BGRA → 贴到画布矩形（帧可偏移/小于画布）。
            //    converter 方案已经独立程序验证正确（含透明度）。
            if (fm.width && fm.height &&
                fm.left + fm.width <= w && fm.top + fm.height <= h) {
                IWICFormatConverter* conv = nullptr;
                if (SUCCEEDED(factory->CreateFormatConverter(&conv)) && conv) {
                    if (SUCCEEDED(conv->Initialize(
                            frame, GUID_WICPixelFormat32bppBGRA,
                            WICBitmapDitherTypeNone, nullptr, 0.0,
                            WICBitmapPaletteTypeCustom))) {
                        std::vector<uint8_t> buf((size_t)fm.width * fm.height * 4);
                        if (SUCCEEDED(conv->CopyPixels(
                                nullptr, fm.width * 4,
                                (UINT)buf.size(), buf.data()))) {
                            for (UINT32 y = 0; y < fm.height; y++) {
                                memcpy(canvas.data() +
                                           ((size_t)(fm.top + y) * w + fm.left) * 4,
                                       buf.data() + (size_t)y * fm.width * 4,
                                       (size_t)fm.width * 4);
                            }
                        }
                    }
                    conv->Release();
                }
            }

            // 4) 该帧合成完毕：与上一帧快照做差分（首帧记录快照，
            //    末帧之后补回绕差分 snap[n-1]→snap[0]）
            if (i == 0) {
                first.assign(canvas.begin(), canvas.end());
            } else {
                deltas.push_back(MakeDiff(prev, canvas, (int)w, (int)h));
            }
            delays.push_back((float)fm.delay_ms / 1000.0f);

            prev.assign(canvas.begin(), canvas.end());
            prevMeta = fm;
            prev_left = fm.left; prev_top = fm.top;
            prev_w = fm.width; prev_h = fm.height;
            have_prev = true;
            frame->Release();
        }

        if (delays.empty() || first.empty()) break;

        // 提交（成功才替换旧内容）：frames[0] = 回绕差分，frames[i>=1] =
        // 顺序差分；工作画布 = 首帧快照
        unload();
        impl_->frames = std::move(deltas);
        impl_->frames.insert(impl_->frames.begin(),
                             MakeDiff(prev, first, (int)w, (int)h));
        impl_->delays = std::move(delays);
        impl_->canvas = std::move(first);
        impl_->w = (int)w;
        impl_->h = (int)h;
        impl_->cur = 0;
        impl_->elapsed = 0.0;
        ok = true;
        size_t delta_bytes = 0, raw_bytes = 0;
        for (const auto& d : impl_->frames) {
            delta_bytes += d.px.size();
            raw_bytes += d.raw;
        }
        printf("[GIF] loaded: %s (%dx%d, %d frames, %.0f KB / raw %.0f KB)\n",
               path_utf8.c_str(), impl_->w, impl_->h,
               (int)impl_->frames.size(), (double)delta_bytes / 1024.0,
               (double)raw_bytes / 1024.0);
    } while (false);

    if (decoder) decoder->Release();
    if (factory) factory->Release();
    if (com_ok && hr != RPC_E_CHANGED_MODE) CoUninitialize();
    return ok;
}

void GifSprite::update(float delta_seconds) {
    if (impl_->frames.empty()) return;
    impl_->elapsed += delta_seconds;
    // 帧时长异常（0）按 100ms 兜底
    float dur = impl_->delays[impl_->cur];
    if (dur <= 0.0f) dur = 0.1f;
    while (impl_->elapsed >= dur) {
        impl_->elapsed -= dur;
        impl_->cur = (impl_->cur + 1) % (int)impl_->frames.size();
        // 差分链必须逐帧应用（跳帧会导致画布状态错误），一 tick 跨多帧
        // 时依次应用沿途各帧的差分
        impl_->ApplyDelta(impl_->frames[impl_->cur]);
        dur = impl_->delays[impl_->cur];
        if (dur <= 0.0f) dur = 0.1f;
    }
}

void GifSprite::setViewport(int x, int y, int w, int h) {
    impl_->vp_x = x;
    impl_->vp_y = y;
    impl_->vp_w = w;
    impl_->vp_h = h;
}

void GifSprite::setFlip(bool flip) { impl_->flip = flip; }

void GifSprite::render() {
    if (impl_->frames.empty() || impl_->vp_w <= 0 || impl_->vp_h <= 0) return;

    // 纹理：首帧全量上传；此后只上传帧间脏矩形（BGRA 与 WIC 字节序一致）
    if (!impl_->tex) {
        glGenTextures(1, &impl_->tex);
        glBindTexture(GL_TEXTURE_2D, impl_->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, impl_->w, impl_->h, 0, GL_BGRA_EXT,
                     GL_UNSIGNED_BYTE, impl_->canvas.data());
        impl_->tex_valid = true;
        impl_->has_dirty = false;
    } else {
        glBindTexture(GL_TEXTURE_2D, impl_->tex);
        if (impl_->tex_valid && impl_->has_dirty) {
            // 脏行跨度与画布同行 → UNPACK_ROW_LENGTH 设为画布宽，
            // 指针偏移到脏区起点（上传完恢复默认，避免影响其它上传路径）
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, impl_->w);
            glTexSubImage2D(GL_TEXTURE_2D, 0, (int)impl_->dirty.x,
                            (int)impl_->dirty.y, (int)impl_->dirty.w,
                            (int)impl_->dirty.h, GL_BGRA_EXT, GL_UNSIGNED_BYTE,
                            impl_->canvas.data() +
                                ((size_t)(int)impl_->dirty.y * impl_->w +
                                 (int)impl_->dirty.x) * 4);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            impl_->has_dirty = false;
        }
    }

    // 适配（contain）：最大占画布 80%（88% 偏大 → 72% 偏小 → 定格 80%）；
    // 水平居中、垂直贴底（无余量，与 Live2D 内容底边贴画布底边一致）
    // —— 与下方任务列表的间距由区块 gap 统一控制。
    // 顶点坐标为视口内相对坐标（glViewport 已负责定位；与 Live2D Draw
    // 的 window_w/h 语义一致）—— 之前误加 vp_x/vp_y 导致偏移两次，
    // 四边形画到画布下方的面板区（被 ImGui 盖住），画布区全透明。
    const float max_w = impl_->vp_w * 0.80f;
    const float max_h = impl_->vp_h * 0.80f;
    float scale = max_w / (float)impl_->w;
    if ((float)impl_->h * scale > max_h) scale = max_h / (float)impl_->h;
    const float dw = (float)impl_->w * scale;
    const float dh = (float)impl_->h * scale;
    const float dx = (impl_->vp_w - dw) * 0.5f;
    const float dy = impl_->vp_h - dh;
    impl_->content = Rect{dx, dy, dw, dh};

    // 正交投影（Y 向下，与客户区一致）+ 文本四边形
    glViewport(impl_->vp_x, impl_->vp_y, impl_->vp_w, impl_->vp_h);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, impl_->vp_w, impl_->vp_h, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);

    glBegin(GL_QUADS);
    const float u0 = impl_->flip ? 1.0f : 0.0f;
    const float u1 = impl_->flip ? 0.0f : 1.0f;
    glTexCoord2f(u0, 0.0f); glVertex2f(dx, dy);
    glTexCoord2f(u1, 0.0f); glVertex2f(dx + dw, dy);
    glTexCoord2f(u1, 1.0f); glVertex2f(dx + dw, dy + dh);
    glTexCoord2f(u0, 1.0f); glVertex2f(dx, dy + dh);
    glEnd();

    glDisable(GL_TEXTURE_2D);
    glDisable(GL_BLEND);
}

}  // namespace dutyon

#else  // 非 Windows 设备端：stb_image + GLES2 实现
//
// ARM Linux 版：stb_image 解码动画 GIF（内部按 disposal 合成为全画布
// RGBA 帧序列，语义与 Windows/WIC 版一致），GLES2 shader 绘制贴图四边形
// （无固定管线，参考 prompt_banner 的渲染方式）。
// 帧数据驻留 RAM，仅当前帧上传纹理（帧切换时全量 glTexSubImage2D；
// 480×480 一帧 0.9MB，USB/SoC 带宽足够）。

#include <GLES3/gl3.h>
#include <stb_image.h>

#include <fstream>
#include <string>
#include <vector>

namespace dutyon {

namespace {

// 正交 2D 贴图（GLES2 语法；Y 向下：与 Windows 版 glOrtho(0,w,h,0) 一致）
const char* kVertSrc =
    "attribute vec2 a_pos;\n"      // 视口内像素坐标（原点左上）
    "attribute vec2 a_uv;\n"
    "uniform vec2 u_screen;\n"     // 视口宽高
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 ndc = vec2(a_pos.x / u_screen.x * 2.0 - 1.0,\n"
    "                  1.0 - a_pos.y / u_screen.y * 2.0);\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

const char* kFragSrc =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "[GifSprite] shader compile: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

}  // namespace

struct GifSprite::Impl {
    std::vector<unsigned char> frames;  // 全帧 RGBA（w*h*4*n，stb 已合成）
    std::vector<float> delays;          // 每帧时长（秒）
    int w = 0, h = 0, n = 0;
    int cur = 0;
    int uploaded = -1;  // 已上传纹理的帧序号
    float elapsed = 0.0f;
    GLuint tex = 0;
    GLuint program = 0;
    int vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
    bool flip = false;
    Rect content;

    void ensureProgram() {
        if (program) return;
        GLuint vs = compileShader(GL_VERTEX_SHADER, kVertSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragSrc);
        if (!vs || !fs) return;
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glBindAttribLocation(program, 0, "a_pos");
        glBindAttribLocation(program, 1, "a_uv");
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512] = {};
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            fprintf(stderr, "[GifSprite] link: %s\n", log);
            glDeleteProgram(program);
            program = 0;
        }
    }
};

GifSprite::GifSprite() : impl_(new Impl) {}
GifSprite::~GifSprite() {
    if (impl_->tex) glDeleteTextures(1, &impl_->tex);
    if (impl_->program) glDeleteProgram(impl_->program);
    delete impl_;
}

bool GifSprite::load(const std::string& path_utf8) {
    if (path_utf8.empty()) return false;
    std::ifstream in(path_utf8, std::ios::binary);
    if (!in) {
        fprintf(stderr, "GIF load failed: %s\n", path_utf8.c_str());
        return false;
    }
    std::vector<unsigned char> buf((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
    if (buf.empty()) {
        fprintf(stderr, "GIF load failed (empty): %s\n", path_utf8.c_str());
        return false;
    }
    // stb 与浏览器/WIC 的语义差异：首帧未覆盖的像素会被
    // GIF 逻辑屏幕背景色强制不透明填充（绿幕 GIF 因此出现绿色边框）。
    // 置 bgindex=0 让 stb 跳过该填充，与 PC 端 WIC 渲染（背景视为透明）一致。
    if (buf.size() > 11 && buf[0] == 'G' && buf[1] == 'I' && buf[2] == 'F' &&
        buf[11] != 0)
        buf[11] = 0;
    int w = 0, h = 0, z = 0;
    int* delays_ms = nullptr;
    // req_comp=4：统一 RGBA；stb 内部按 disposal 逐帧合成到全画布
    unsigned char* data = stbi_load_gif_from_memory(
        buf.data(), (int)buf.size(), &delays_ms, &w, &h, &z, nullptr, 4);
    if (!data || z <= 0) {
        fprintf(stderr, "GIF load failed: %s (%s)\n", path_utf8.c_str(),
                stbi_failure_reason());
        if (delays_ms) stbi_image_free(delays_ms);
        return false;
    }
    // 换动画：清旧帧（失败保持原状的语义由调用方"先 load 成功才切换"保证，
    // 这里 load 失败直接返回，不动旧状态）
    const size_t frame_bytes = (size_t)w * h * 4;
    impl_->frames.assign(data, data + frame_bytes * z);
    impl_->delays.resize(z);
    for (int i = 0; i < z; ++i) {
        const int ms = delays_ms ? delays_ms[i] : 0;
        // <=10ms 按浏览器惯例兜底 100ms（同 Windows 版）
        impl_->delays[i] = ms > 10 ? ms / 1000.0f : 0.1f;
    }
    stbi_image_free(data);  // 释放 stb 缓冲（含 delays）
    impl_->w = w;
    impl_->h = h;
    impl_->n = z;
    impl_->cur = 0;
    impl_->elapsed = 0.0f;
    impl_->uploaded = -1;  // 触发下一帧 render 全量上传
    printf("[GifSprite] loaded %dx%d %d frames: %s\n", w, h, z,
           path_utf8.c_str());
    return true;
}

bool GifSprite::isLoaded() const { return !impl_->frames.empty(); }

void GifSprite::unload() {
    impl_->frames.clear();
    impl_->frames.shrink_to_fit();
    impl_->delays.clear();
    impl_->delays.shrink_to_fit();
    impl_->w = impl_->h = impl_->n = 0;
    impl_->cur = 0;
    impl_->uploaded = -1;
    impl_->elapsed = 0.0f;
    if (impl_->tex) {
        glDeleteTextures(1, &impl_->tex);
        impl_->tex = 0;
    }
}

void GifSprite::update(float delta_seconds) {
    if (impl_->frames.empty()) return;
    impl_->elapsed += delta_seconds;
    float dur = impl_->delays[impl_->cur];
    while (impl_->elapsed >= dur) {
        impl_->elapsed -= dur;
        impl_->cur = (impl_->cur + 1) % impl_->n;
        dur = impl_->delays[impl_->cur];
    }
}

void GifSprite::setViewport(int x, int y, int w, int h) {
    impl_->vp_x = x;
    impl_->vp_y = y;
    impl_->vp_w = w;
    impl_->vp_h = h;
}

void GifSprite::setFlip(bool flip) { impl_->flip = flip; }
bool GifSprite::isFlipped() const { return impl_->flip; }

void GifSprite::render() {
    if (impl_->frames.empty() || impl_->vp_w <= 0 || impl_->vp_h <= 0) return;
    impl_->ensureProgram();
    if (!impl_->program) return;

    // 纹理：帧切换时全量上传（RGBA，stb 行序自顶向下与 uv 一致）
    if (!impl_->tex) {
        glGenTextures(1, &impl_->tex);
        glBindTexture(GL_TEXTURE_2D, impl_->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, impl_->w, impl_->h, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, impl_->frames.data());
        impl_->uploaded = impl_->cur;
    } else if (impl_->uploaded != impl_->cur) {
        glBindTexture(GL_TEXTURE_2D, impl_->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(
            GL_TEXTURE_2D, 0, 0, 0, impl_->w, impl_->h, GL_RGBA,
            GL_UNSIGNED_BYTE,
            impl_->frames.data() + (size_t)impl_->cur * impl_->w * impl_->h * 4);
        impl_->uploaded = impl_->cur;
    }

    // 适配（contain）：最大占画布 80%、水平居中、垂直贴底（多任务）/
    // 垂直居中（单任务/相框，setCenterV）—— Y 向下坐标系
    const float max_w = impl_->vp_w * 0.80f;
    const float max_h = impl_->vp_h * 0.80f;
    float scale = max_w / (float)impl_->w;
    if ((float)impl_->h * scale > max_h) scale = max_h / (float)impl_->h;
    const float dw = (float)impl_->w * scale;
    const float dh = (float)impl_->h * scale;
    const float dx = (impl_->vp_w - dw) * 0.5f;
    const float dy = center_v_ ? (impl_->vp_h - dh) * 0.5f
                               : impl_->vp_h - dh;
    impl_->content = Rect{dx, dy, dw, dh};

    glViewport(impl_->vp_x, impl_->vp_y, impl_->vp_w, impl_->vp_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 四边形（视口内相对坐标；uv 的 v=0 是图像首行=屏幕顶部，天然正立）
    const float u0 = impl_->flip ? 1.0f : 0.0f;
    const float u1 = impl_->flip ? 0.0f : 1.0f;
    const float verts[] = {
        dx,      dy,      u0, 0.0f,
        dx + dw, dy,      u1, 0.0f,
        dx + dw, dy + dh, u1, 1.0f,
        dx,      dy + dh, u0, 1.0f,
    };

    glUseProgram(impl_->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, impl_->tex);
    glUniform1i(glGetUniformLocation(impl_->program, "u_tex"), 0);
    glUniform2f(glGetUniformLocation(impl_->program, "u_screen"),
                (float)impl_->vp_w, (float)impl_->vp_h);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts + 2);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    glDisable(GL_BLEND);
}

Rect GifSprite::contentRect() const { return impl_->content; }
int GifSprite::frameCount() const { return impl_->n; }
int GifSprite::width() const { return impl_->w; }
int GifSprite::height() const { return impl_->h; }

}  // namespace dutyon

#endif  // _WIN32
