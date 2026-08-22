// 2.0 原生 UI 层实现
//
// PC (Windows): ImGui + GLFW + OpenGL3 后端
// 文案 / 配色 / 布局逐项对齐 1.x 前端（frontend/index.html + styles.css）：
//   - 状态栏（角色画布下方）：状态头（圆点 + 状态文字 + ☰ 菜单按钮）+ 项目列表
//   - 系统监控面板（状态栏下方，窗口底部）：标题行(收起/恢复) + 彩色标签行 + 折线图
//   - 项目列表：左边框状态色 / 状态点闪烁 / IDE 徽标 / 悬停高亮 / 确认行呼吸红光
//   - 头顶特效：ZZZ 上浮 / 工作点 / 叹号弹跳（动画，锚定模型包围盒）
//   - 右键菜单：1.x #context-menu 同款样式 + 视图切换（ImGui 自绘）
// ARM Linux: 占位实现

#include "ui/ui_renderer.h"
#include "ui/i18n.h"
#include "api/client.h"
#include "render/live2d_renderer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>  // GetWindowsDirectoryA（中文字体定位）
#define GLFW_INCLUDE_NONE  // 阻止 glfw3.h 包含 <GL/gl.h>（与 glew.h 冲突）
#include <GL/glew.h>       // glGenTextures（形象缩略图纹理）
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_freetype.h>  // ImGuiFreeTypeBuilderFlags_*（字体 hinting 模式）
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <stb_image.h>      // 缩略图解码（PNG / GIF 首帧）；降采样用
                            // live2d_renderer.h 的 HalveRGBA
#define STB_TRUETYPE_IMPLEMENTATION  // 头顶特效字形光栅化（烘焙精灵；
#include <stb_truetype.h>            // imgui 内置副本已改名 imstbtt_* 无冲突）
#include <map>
#endif

#include <algorithm>

namespace dutyon {

#ifdef _WIN32

// ---------------------------------------------------------------------------
// 颜色工具（1.x styles.css 同款配色）
// ---------------------------------------------------------------------------
static ImVec4 Rgb(int r, int g, int b, int a = 255) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}
static ImU32 Rgb32(int r, int g, int b, int a = 255) {
    return IM_COL32(r, g, b, a);
}

// 文本缩放：ImGui 默认 stb_truetype 光栅化（无 FreeType）比 1.x WebView
// (DirectWrite) 渲染的同标称字号视觉上明显偏小偏细，所有字号统一放大
// ~25% 对齐 1.x 观感（只作用字号，布局几何仍按 1.x CSS px）
static constexpr float kTextScale = 1.25f;

// 像素对齐文字绘制：坐标四舍五入到整数像素（分数坐标使字形位图被
// 双线性采样到相邻像素 → 整体发糊），字号与图集光栅尺寸一致（差
// <0.75px 时直接取光栅尺寸，避免 ImGui 对字形位图做缩放采样）。
// 跨分辨率联合缩放下 S 常为 0.75/0.9375 等小数，若不对齐，小字号
// CJK（~9-11px）核心笔画会被完全涂抹成灰色（实测中间调占比 96%）
static void AddTextS(ImDrawList* dl, ImFont* font, float size,
                     const ImVec2& pos, ImU32 col, const char* text,
                     const char* text_end = NULL, float wrap_width = 0.0f) {
    const float d = size - font->FontSize;
    if (d > -0.75f && d < 0.75f) size = font->FontSize;
    dl->AddText(font, size,
                ImVec2((float)(int)(pos.x + 0.5f),
                       (float)(int)(pos.y + 0.5f)),
                col, text, text_end, wrap_width);
}

// 文字垂直居中（列表行/标题行用）：按整串字形的实际墨迹盒（Y0..Y1）
// 中点定位，而非 size*0.5 行高中点。FontSize 行框含上伸/下伸余量，
// 雅黑等 CJK 字体的墨迹整体偏行框下半部，按行高居中会系统性偏上，
// 视觉"不居中"（用户反馈）。返回值直接作为 AddTextS 的 pos.y。
static float TextCenteredY(ImFont* font, float size, const char* text,
                           float midy) {
    const float scale = size / font->FontSize;
    float top = 1e9f, bot = -1e9f;
    const char* s = text;
    while (s && *s) {
        // UTF-8 解码（自实现，避免依赖 imgui_internal.h）
        unsigned int c = (unsigned char)*s;
        int adv = 1;
        if (c >= 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
            (s[3] & 0xC0) == 0x80) {
            c = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
                ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
            adv = 4;
        } else if (c >= 0xE0 && (s[1] & 0xC0) == 0x80 &&
                   (s[2] & 0xC0) == 0x80) {
            c = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            adv = 3;
        } else if (c >= 0xC0 && (s[1] & 0xC0) == 0x80) {
            c = ((c & 0x1F) << 6) | (s[1] & 0x3F);
            adv = 2;
        }
        s += adv;
        if (c == '\n') break;
        const ImFontGlyph* g = font->FindGlyph((ImWchar)c);
        if (!g || !g->Visible) continue;
        const float y0 = g->Y0 * scale, y1 = g->Y1 * scale;
        if (y0 < top) top = y0;
        if (y1 > bot) bot = y1;
    }
    if (top > bot) return midy - size * 0.5f;  // 空串回退
    return midy - (top + bot) * 0.5f;
}

// ---------------------------------------------------------------------------
// 格式化工具（对齐 1.x fmtC / fmtRate 的紧凑风格）
// ---------------------------------------------------------------------------
static void FormatRateCompact(unsigned long long b, char* out, size_t len) {
    if (b >= 1024ULL * 1024 * 1024)
        snprintf(out, len, "%.1fG", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024ULL * 1024)
        snprintf(out, len, "%.1fM", b / (1024.0 * 1024));
    else if (b >= 1024ULL)
        snprintf(out, len, "%.0fK", b / 1024.0);
    else
        snprintf(out, len, "%lluB", b);
}

static void FormatBytesCompact(unsigned long long b, char* out, size_t len) {
    double gb = b / (1024.0 * 1024 * 1024);
    if (gb >= 10.0)
        snprintf(out, len, "%.0fGB", gb);
    else if (gb >= 1.0)
        snprintf(out, len, "%.1fGB", gb);
    else if (b >= 1024ULL * 1024)
        snprintf(out, len, "%.0fMB", b / (1024.0 * 1024));
    else
        snprintf(out, len, "%lluKB", b / 1024);
}

// CSS ease 闪烁（pulse-dot：0%/100% 不透明 1，50% 0.3 的线性插值）
static float PulseAlpha(float t, float period) {
    if (period <= 0.0f) return 1.0f;
    float x = fmodf(t, period) / period;  // 0..1
    float tri = x < 0.5f ? 1.0f - 1.4f * x : 0.3f + 1.4f * (x - 0.5f);
    return tri;
}

// 0→1→0 三角波（alternate 动画的往返相位）
static float TriWave(float t, float period) {
    float x = fmodf(t, period) / period;
    return 1.0f - fabsf(2.0f * x - 1.0f);
}

// ---- 头顶特效精灵烘焙（复刻 1.x 浏览器渲染管线）----
// 1.x 的 ZZZ/叹号/工作点之所以好看：① 真实字体设计字形（DirectWrite
// 光栅化 Segoe UI Bold，weight 800/900）；② text-shadow 是真高斯模糊
// （连续衰减）；③ 白色硬偏移影 + 彩色辉光 + 字身三层 src-over 合成。
// 几何描边模拟（放大轮廓/原位加粗）两次反馈"差点意思"——离散条带
// 永远出不了连续 blur 的柔感，手拼字形也到不了设计字形的形态。
// 正确做法：启动时用 stb_truetype 从同款字体文件光栅化字形覆盖图，
// CPU 做真高斯模糊 + 三层合成 → 小纹理；每帧画 1 个四边形（比几何
// 绘制 45 图元更便宜，低端 ARM 无压力）。3x 超采样烘焙 + 双线性缩放。
// 可分离高斯模糊（先行后列两遍）
static void FxBlur(float* img, int w, int h, float sigma) {
    if (sigma <= 0.1f || w <= 1 || h <= 1) return;
    const int r = (int)(sigma * 2.5f + 0.5f);
    std::vector<float> kern(2 * r + 1);
    float sum = 0.0f;
    for (int i = -r; i <= r; i++) {
        kern[i + r] = expf(-(float)(i * i) / (2.0f * sigma * sigma));
        sum += kern[i + r];
    }
    for (auto& v : kern) v /= sum;
    std::vector<float> tmp((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float acc = 0.0f;
            for (int i = -r; i <= r; i++) {
                int xx = x + i;
                if (xx < 0) xx = 0; else if (xx >= w) xx = w - 1;
                acc += img[y * w + xx] * kern[i + r];
            }
            tmp[y * w + x] = acc;
        }
    for (int x = 0; x < w; x++)
        for (int y = 0; y < h; y++) {
            float acc = 0.0f;
            for (int i = -r; i <= r; i++) {
                int yy = y + i;
                if (yy < 0) yy = 0; else if (yy >= h) yy = h - 1;
                acc += tmp[yy * w + x] * kern[i + r];
            }
            img[y * w + x] = acc;
        }
}

// ---- 状态面板边框/光晕（1.x #status-bar / #monitor-panel.state-*）----
// 忙碌/提醒时面板边框发对应状态色的淡淡光晕（box-shadow 0 4px 20px）：
//   sleeping 蓝 rgba(100,150,255)、working 黄 rgba(255,200,50)、
//   alert 红 rgba(255,68,68) + alert-pulse 1s 呼吸（0.15↔0.4）
// 返回边框色（1.x border alpha：sleeping 0.2 / working 0.3 / alert 0.4）
static ImU32 StateBorderColor(const char* state) {
    if (!state) return IM_COL32(255, 255, 255, 20);
    if (strcmp(state, "sleeping") == 0) return IM_COL32(100, 150, 255, 51);
    if (strcmp(state, "working") == 0) return IM_COL32(255, 200, 50, 77);
    if (strcmp(state, "alert") == 0) return IM_COL32(255, 68, 68, 102);
    return IM_COL32(255, 255, 255, 20);
}

// 光晕画在背景绘制层（窗口之下）：多层重叠描边模拟柔和 box-shadow。
// 层与层必须重叠（厚 > 步进），否则呈现离散同心圆环；透明度平方衰减
// 近似高斯模糊的渐变。只露出窗口边界外的部分（窗口内被 0.85 不透明
// 面板底盖住，露出约 15%，恰好形成 1.x 那种内外一致的淡光晕）
static void DrawStateGlow(ImDrawList* bg, const ImVec2& bmin, const ImVec2& bmax,
                          float rounding, const char* state, double now, float S) {
    if (!bg || !state) return;
    unsigned r, g, b;
    float base;   // 1.x 阴影基准强度
    bool pulse = false;
    if (strcmp(state, "sleeping") == 0)      { r = 100; g = 150; b = 255; base = 0.10f; }
    else if (strcmp(state, "working") == 0)  { r = 255; g = 200; b = 50;  base = 0.15f; }
    else if (strcmp(state, "alert") == 0)    { r = 255; g = 68;  b = 68;  base = 0.15f; pulse = true; }
    else return;
    if (pulse) base = 0.15f + 0.25f * TriWave((float)now, 2.0f);  // 呼吸 1s/方向
    const int layers = 5;
    const float step = 2.0f * S, thick = 3.4f * S;  // 重叠 1.4S：连续光带
    for (int i = 0; i < layers; i++) {
        const float e = i * step;
        const float f = 1.0f - (float)i / (float)layers;
        const ImU32 col = IM_COL32(r, g, b, (int)(255.0f * base * f * f));
        bg->AddRect(ImVec2(bmin.x - e, bmin.y - e), ImVec2(bmax.x + e, bmax.y + e),
                    col, rounding + e, 0, thick);
    }
}

// ---------------------------------------------------------------------------
// 折线图历史缓冲（环形，右端为最新样本）
// 采样约 1.5s 一次 × 64 点 ≈ 96 秒趋势（同 1.x "近1分钟趋势"）
// ---------------------------------------------------------------------------
static constexpr int kSparkPoints = 64;
struct RingHistory {
    float data[kSparkPoints] = {0};
    int count = 0;
    int head = 0;  // 下一个写入位置
    void Push(float v) {
        data[head] = v;
        head = (head + 1) % kSparkPoints;
        if (count < kSparkPoints) count++;
    }
    // 按时间顺序（旧 → 新）取第 i 个样本
    float At(int i) const {
        return data[(head - count + i + kSparkPoints * 2) % kSparkPoints];
    }
};

// 迷你折线图（同 1.x drawSparkline：右对齐，最新样本贴右缘）
// fixed_max <= 0 时自适应缩放（内存/网络变化范围窄，固定轴会压成直线）
static void DrawSpark(ImDrawList* dl, const ImVec2& a, const ImVec2& b,
                      const RingHistory* h, float fixed_max, ImU32 col) {
    const int n = h->count;
    if (n < 2) return;
    float max = fixed_max;
    if (max <= 0.0f) {
        max = 1.0f;
        for (int i = 0; i < n; i++) {
            if (h->At(i) > max) max = h->At(i);
        }
    }
    const float w = b.x - a.x;
    const float hh = b.y - a.y;
    const float step = w / (float)(kSparkPoints - 1);
    const float x0 = a.x + w - (n - 1) * step;
    float px = 0.0f, py = 0.0f;
    for (int i = 0; i < n; i++) {
        float v = h->At(i);
        if (v > max) v = max;
        if (v < 0.0f) v = 0.0f;
        const float x = x0 + i * step;
        const float y = b.y - 1.0f - (v / max) * (hh - 2.0f);
        if (i > 0) dl->AddLine(ImVec2(px, py), ImVec2(x, y), col, 1.0f);
        px = x;
        py = y;
    }
}

// ---------------------------------------------------------------------------
// 字体文件共享缓冲：同一轮 BuildFonts 中 4 个字号档 + 符号/谚文合并共用
// 一份文件数据。AddFontFromFileTTF 默认让图集驻留每个字号档一份完整字体
// 文件（msyh.ttc 19MB × 4 档 + 符号/谚文 ≈ 165MB 常驻）；改为 FontDataOwned
// Atlas=false + 共享读入，图集光栅化完成后统一释放（峰值 ~32MB → 常驻 0）
// ---------------------------------------------------------------------------
static std::vector<uint8_t>* FontFileBuf(
    std::map<std::string, std::vector<uint8_t>>& bufs, const char* path) {
    auto it = bufs.find(path);
    if (it != bufs.end()) return &it->second;
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf;
    if (sz > 0) {
        buf.resize((size_t)sz);
        if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) sz = -1;
    }
    fclose(f);
    if (sz <= 0) return nullptr;
    return &bufs.emplace(path, std::move(buf)).first->second;
}

// ---------------------------------------------------------------------------
// 字体集加载：主字体 Segoe UI（拉丁/数字，对齐 1.x styles.css
// font-family 'Segoe UI','Microsoft YaHei' 的渲染顺序）+ 按语言合并
// CJK 字体（zh→msyh / ja→meiryo / ko→malgun；浏览器按码位逐字回退，
// ImGui 用 MergeMode 等价复刻：主字体只烘焙拉丁区间，CJK 字体只合并
// 表意文字/假名/谚文区间，拉丁始终走 Segoe 的字形 —— 此前全量 msyh
// 导致拉丁字形发胖，与 1.x 观感差异大）
// ---------------------------------------------------------------------------
static ImFont* LoadFontSet(ImGuiIO& io, float size_px, const ImWchar* latin_ranges,
                           const ImWchar* cjk_ranges,
                           std::map<std::string, std::vector<uint8_t>>& fbufs,
                           bool mono_small) {
    char win_dir[MAX_PATH];
    UINT len = GetWindowsDirectoryA(win_dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH - 64) return nullptr;

    // PixelSnapH：字形水平对齐像素边界（配合 AddTextS 的整数坐标，
    // 小字号 CJK 不再被双线性涂抹成灰色）
    // 光栅化走 FreeType（IMGUI_ENABLE_FREETYPE），hinting 按屏幕/字号自适应：
    //   低分屏（ui_scale<1）且 <13px：MonoHinting|Monochrome —— 无 AA 的
    //     1px 实心笔画。8~11px 灰度 AA 必然把 CJK 笔画涂抹成灰雾（用户
    //     在 1080p@100% 副屏两次反馈"很模糊/不清晰"）
    //   其余：LightHinting + 灰度 AA —— 轻量竖向 hinting 保留字形原貌，
    //     最接近浏览器 DirectWrite 的渲染形态（ForceAutoHint 会让笔画
    //     变形发虚，高分屏用户反馈"字体好丑"）
    // 调试：DUTYON_FT_FLAGS 强制覆盖（十六进制）
    static const unsigned ft_flags_env = [] {
        const char* e = getenv("DUTYON_FT_FLAGS");
        return e ? (unsigned)strtoul(e, nullptr, 0) : 0u;
    }();
    const unsigned ft_flags =
        ft_flags_env ? ft_flags_env
                     : (mono_small
                            ? (unsigned)(ImGuiFreeTypeBuilderFlags_MonoHinting |
                                         ImGuiFreeTypeBuilderFlags_Monochrome)
                            : (unsigned)ImGuiFreeTypeBuilderFlags_LightHinting);

    // ---- 主字体：Segoe UI（拉丁）----
    ImFont* font = nullptr;
    {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\Fonts\\segoeui.ttf", win_dir);
        if (std::vector<uint8_t>* fb = FontFileBuf(fbufs, path)) {
            ImFontConfig fcfg;
            fcfg.PixelSnapH = true;
            fcfg.FontBuilderFlags = ft_flags;
            fcfg.FontDataOwnedByAtlas = false;
            font = io.Fonts->AddFontFromMemoryTTF(fb->data(), (int)fb->size(),
                                                  size_px, &fcfg, latin_ranges);
            if (font) printf("[UI] font loaded: segoeui.ttf @ %.0fpx\n", size_px);
        }
    }
    // Segoe 缺失（LTSC/精简系统）兜底：CJK 字体当主字体（拉丁回落雅黑）
    if (!font) {
        const char* fallbacks[] = {"msyh.ttc", "msyh.ttf", "simhei.ttf",
                                   "simsun.ttc"};
        for (const char* c : fallbacks) {
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s\\Fonts\\%s", win_dir, c);
            if (std::vector<uint8_t>* fb = FontFileBuf(fbufs, path)) {
                ImFontConfig fcfg;
                fcfg.PixelSnapH = true;
                fcfg.FontBuilderFlags = ft_flags;
                fcfg.FontDataOwnedByAtlas = false;
                font = io.Fonts->AddFontFromMemoryTTF(fb->data(), (int)fb->size(),
                                                      size_px, &fcfg, latin_ranges);
                if (font) {
                    printf("[UI] font loaded: %s (segoe fallback) @ %.0fpx\n", c,
                           size_px);
                    break;
                }
            }
        }
    }
    if (!font) return nullptr;

    // ---- CJK 合并：按语言选字形风格（ja 汉字用日本字形 / ko 谚文）----
    {
        const std::string& lang = I18n::lang();
        std::vector<std::string> candidates;
        if (lang == "ko") {
            candidates.insert(candidates.end(), {"malgun.ttf", "malgunbd.ttf"});
        } else if (lang == "ja") {
            candidates.insert(candidates.end(),
                              {"meiryo.ttc", "YuGothM.ttc", "msgothic.ttc"});
        }
        candidates.insert(candidates.end(),
                          {"msyh.ttc", "msyh.ttf", "simhei.ttf", "simsun.ttc", "msjh.ttc"});
        for (const auto& c : candidates) {
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s\\Fonts\\%s", win_dir, c.c_str());
            if (std::vector<uint8_t>* fb = FontFileBuf(fbufs, path)) {
                ImFontConfig cfg;
                cfg.MergeMode = true;
                cfg.PixelSnapH = true;
                cfg.FontBuilderFlags = ft_flags;
                cfg.DstFont = font;
                cfg.FontDataOwnedByAtlas = false;
                ImFont* merged = io.Fonts->AddFontFromMemoryTTF(
                    fb->data(), (int)fb->size(), size_px, &cfg, cjk_ranges);
                printf("[UI] cjk merge: %s %s\n", c.c_str(),
                       merged ? "ok" : "FAILED");
                break;  // 第一个可用字体即止（候选序即优先级）
            }
        }
    }

    // 合并符号字体：Segoe UI/CJK 字体普遍缺 ☰↺▸▾✓ 等字形（渲染成 ?），
    // 用 Segoe UI Symbol 补齐（Win10+ 系统必有；缺失仅降级不致命）
    static const ImWchar kSymRanges[] = {
        0x21BA, 0x21BA,  // ↺ 恢复默认
        0x25B6, 0x25B6,  // ▶ 子菜单
        0x25B8, 0x25B8,  // ▸ 已收起
        0x25BE, 0x25BE,  // ▾ 收起
        0x25C0, 0x25C0,  // ◀ 返回
        0x2630, 0x2630,  // ☰ 菜单按钮
        0x2713, 0x2713,  // ✓ 勾选
        0,
    };
    {
        char sym_path[MAX_PATH];
        snprintf(sym_path, sizeof(sym_path), "%s\\Fonts\\seguisym.ttf", win_dir);
        if (std::vector<uint8_t>* sb = FontFileBuf(fbufs, sym_path)) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.PixelSnapH = true;
            cfg.FontBuilderFlags = ft_flags;
            cfg.DstFont = font;
            cfg.FontDataOwnedByAtlas = false;
            ImFont* sym = io.Fonts->AddFontFromMemoryTTF(
                sb->data(), (int)sb->size(), size_px, &cfg, kSymRanges);
            printf("[UI] symbol merge: %s (config entries: %d)\n",
                   sym ? "ok" : "FAILED", io.Fonts->ConfigData.Size);
        }
    }
    // 韩文语言名兜底：非韩语界面时 cjk_ranges 已含谚文（精确字符集），
    // 此合并仅在字符集未覆盖时兜底 malgun 的 한국어 三码位
    if (I18n::lang() != "ko") {
        static const ImWchar kKoRanges[] = {
            0xD55C, 0xD55C,  // 한
            0xAD6D, 0xAD6D,  // 국
            0xC5B4, 0xC5B4,  // 어
            0,
        };
        char ko_path[MAX_PATH];
        snprintf(ko_path, sizeof(ko_path), "%s\\Fonts\\malgun.ttf", win_dir);
        if (std::vector<uint8_t>* kb = FontFileBuf(fbufs, ko_path)) {
            ImFontConfig kcfg;
            kcfg.MergeMode = true;
            kcfg.PixelSnapH = true;
            kcfg.FontBuilderFlags = ft_flags;
            kcfg.DstFont = font;
            kcfg.FontDataOwnedByAtlas = false;
            ImFont* ko = io.Fonts->AddFontFromMemoryTTF(
                kb->data(), (int)kb->size(), size_px, &kcfg, kKoRanges);
            printf("[UI] hangul merge: %s\n", ko ? "ok" : "skipped");
        }
    }
    return font;
}

// 指定字号的文本宽（AddText 缩放绘制配套测量）
// CalcTextSizeA 非 const 成员，此处必须收 ImFont*
static float TextW(ImFont* font, float size, const char* text) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
}

// ---------------------------------------------------------------------------
// UTF-8 文本按最大宽度截断（超出部分以 "…" 结尾）
// ---------------------------------------------------------------------------
static void TruncateUtf8(const char* text, float max_w, char* out, size_t out_len,
                         ImFont* font, float size) {
    if (out_len == 0) return;
    float w = TextW(font, size, text);
    if (w <= max_w) {
        snprintf(out, out_len, "%s", text);
        return;
    }
    size_t pos = 0;
    while (text[pos] != '\0') {
        // 前进一个 UTF-8 码点
        unsigned char ch = (unsigned char)text[pos];
        size_t cp_len = 1;
        if (ch >= 0xF0) cp_len = 4;
        else if (ch >= 0xE0) cp_len = 3;
        else if (ch >= 0xC0) cp_len = 2;
        size_t next = pos + cp_len;
        if (next >= out_len - 4) break;
        // 试探包含到 next 的宽度（含省略号）
        char probe[128];
        snprintf(probe, sizeof(probe), "%.*s…", (int)next, text);
        if (TextW(font, size, probe) > max_w) break;
        pos = next;
    }
    snprintf(out, out_len, "%.*s…", (int)pos, text);
}

// 解码一个 UTF-8 码点并前进（返回码点；seq_len 输出该码点字节数）
static unsigned DecodeUtf8(const char* s, size_t* seq_len) {
    const unsigned char c0 = (unsigned char)s[0];
    if (c0 < 0x80) {
        *seq_len = 1;
        return c0;
    }
    unsigned cp = 0;
    size_t n = 1;
    if ((c0 & 0xE0) == 0xC0) { cp = c0 & 0x1F; n = 2; }
    else if ((c0 & 0xF0) == 0xE0) { cp = c0 & 0x0F; n = 3; }
    else if ((c0 & 0xF8) == 0xF0) { cp = c0 & 0x07; n = 4; }
    else { *seq_len = 1; return c0; }
    for (size_t i = 1; i < n; i++) {
        const unsigned char ci = (unsigned char)s[i];
        if ((ci & 0xC0) != 0x80) { *seq_len = 1; return c0; }
        cp = (cp << 6) | (ci & 0x3F);
    }
    *seq_len = n;
    return cp;
}

// 码点重新编码为 UTF-8
static void EncodeUtf8(unsigned cp, char* out, size_t out_len) {
    if (cp < 0x80) snprintf(out, out_len, "%c", (char)cp);
    else if (cp < 0x800)
        snprintf(out, out_len, "%c%c", (char)(0xC0 | (cp >> 6)),
                 (char)(0x80 | (cp & 0x3F)));
    else if (cp < 0x10000)
        snprintf(out, out_len, "%c%c%c", (char)(0xE0 | (cp >> 12)),
                 (char)(0x80 | ((cp >> 6) & 0x3F)),
                 (char)(0x80 | (cp & 0x3F)));
    else
        snprintf(out, out_len, "%c%c%c%c", (char)(0xF0 | (cp >> 18)),
                 (char)(0x80 | ((cp >> 12) & 0x3F)),
                 (char)(0x80 | ((cp >> 6) & 0x3F)),
                 (char)(0x80 | (cp & 0x3F)));
}

static unsigned UpperCodepoint(unsigned cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 'a' + 'A';
    return cp;
}

// 名称首字符大写（1.x char.name.charAt(0).toUpperCase()；缩略图占位字母）
static std::string FirstUtf8CharUpper(const std::string& text) {
    if (text.empty()) return "?";
    size_t n = 0;
    const unsigned cp = DecodeUtf8(text.c_str(), &n);
    char out[8] = {};
    EncodeUtf8(UpperCodepoint(cp), out, sizeof(out));
    return out;
}

// 项目两字缩写（1.x projectInitials：按空白/-_./\ 分词取前两词首字母；
// 单词则取前两个字符；均大写）—— 吸附条徽标文字
static std::string ProjectInitials(const std::string& name) {
    std::string trimmed = name;
    // 去首尾空白
    size_t b = trimmed.find_first_not_of(" \t\r\n");
    size_t e = trimmed.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return "--";
    trimmed = trimmed.substr(b, e - b + 1);

    // 分词（分隔符：空白 - _ . / \）
    std::vector<std::string> words;
    std::string cur;
    for (size_t i = 0; i < trimmed.size();) {
        size_t n = 0;
        const unsigned cp = DecodeUtf8(trimmed.c_str() + i, &n);
        const bool sep = cp == ' ' || cp == '\t' || cp == '-' || cp == '_' ||
                         cp == '.' || cp == '/' || cp == '\\';
        if (sep) {
            if (!cur.empty()) words.push_back(cur);
            cur.clear();
        } else {
            cur.append(trimmed, i, n);
        }
        i += n;
    }
    if (!cur.empty()) words.push_back(cur);

    char out[16] = {};
    if (words.size() >= 2) {
        size_t n1 = 0, n2 = 0;
        const unsigned c1 = DecodeUtf8(words[0].c_str(), &n1);
        const unsigned c2 = DecodeUtf8(words[1].c_str(), &n2);
        char p1[8] = {}, p2[8] = {};
        EncodeUtf8(UpperCodepoint(c1), p1, sizeof(p1));
        EncodeUtf8(UpperCodepoint(c2), p2, sizeof(p2));
        snprintf(out, sizeof(out), "%s%s", p1, p2);
    } else {
        // 前两个码点
        char p[8] = {};
        size_t used = 0;
        for (size_t i = 0; i < trimmed.size() && used < 2;) {
            size_t n = 0;
            const unsigned cp = DecodeUtf8(trimmed.c_str() + i, &n);
            char enc[8] = {};
            EncodeUtf8(UpperCodepoint(cp), enc, sizeof(enc));
            if (used == 0)
                snprintf(out, sizeof(out), "%s", enc);
            else
                snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s", enc);
            used++;
            i += n;
        }
        if (used == 0) snprintf(out, sizeof(out), "--");
        (void)p;
    }
    return out;
}

// ---------------------------------------------------------------------------
// 菜单视图（对齐 1.x #context-menu 的各 view）
// ---------------------------------------------------------------------------
enum MenuView {
    kMenuMain = 0,
    kMenuModels,
    kMenuMotionPlay,
    kMenuMotionAssign,
    kMenuSettings,
    kMenuLanguage,
    kMenuVisibility,
};

struct UIRenderer::Impl {
    GLFWwindow* window = nullptr;
    bool imgui_ready = false;
    int win_w = 0, win_h = 0;

    // DPI 缩放（1.x 是 WebView 逻辑像素：260 CSS px @125% = 325 物理 px；
    // 原生窗口必须按监视器缩放放大，否则整体比 1.x 小一圈）
    float scale = 1.0f;

    ImFont* font_12 = nullptr;  // 12px：状态文字 / 项目名 / 菜单项
    ImFont* font_11 = nullptr;  // 11px：动作名 / 卡片名 / 停靠条项目名
    ImFont* font_10 = nullptr;  // 10px：监控行 / 菜单 label / 提示
    ImFont* font_9 = nullptr;   // 9px：IDE 徽标字母 / mini 状态文字

    // ---- 头顶特效烘焙精灵（见文件头 FxBlur 注释；懒构建）----
    struct FxSprite {
        GLuint tex = 0;
        int w = 0, h = 0;
        float ax = 0.0f, ay = 0.0f;  // 锚点在画布中坐标（字形底中/圆底中）
    };
    FxSprite fx_z[3];           // Z 13/16/19px（1.x zzz-effect 三档）
    FxSprite fx_bang;           // ! 28px（1.x alert-effect）
    FxSprite fx_dot;            // 6px 工作点 + 光晕（1.x working-effect）
    bool fx_built = false;

    // 当前整体状态（renderStatus 每帧更新；监控面板光晕取色用）
    std::string cur_state;

    // 字形范围表：AddFontFromMemoryTTF 只保存指针，数组必须与字体同生命周期
    // （局部作用域的 ranges 在 init 返回后释放，图集首帧构建即悬空崩溃）
    ImVector<ImWchar> latin_ranges;  // 主字体（Segoe）区间：ASCII/Latin-1/Ext-A/标点/符号
    ImVector<ImWchar> cjk_ranges;    // 合并字体区间：表意/假名/谚文 + 精确字符集
    // 字体文件共享缓冲（4 档字号 + 合并字体共用一份；图集光栅化完成后
    // 在 endFrame 释放 —— 释放过早会让首帧 Build 读悬空指针）
    std::map<std::string, std::vector<uint8_t>> font_file_bufs;
    bool fonts_dirty = false;   // 语言切换后置位，帧末重建图集

    // 布局（主循环注入，物理像素）
    float canvas_h = 260.0f;  // 角色区高
    float panel_x = 10.0f;    // 状态/监控面板左边
    float panel_w = 240.0f;   // 面板宽
    float panel_gap = 4.0f;   // 画布↔面板 / 面板↔面板 间距
    bool mini = false;

    // 上一帧实测面板高度（主循环据此调整窗口总高度）
    float status_h = 66.0f;
    float monitor_h = 0.0f;

    // 折线图历史
    RingHistory cpu, ram, gpu, vram, net;

    // 头顶特效锚定（窗口客户区坐标，Y 向下）
    Rect model_rect;
    bool has_model_rect = false;
    bool model_bounds_tight = true;  // Live2D=紧贴内容；GIF=整图框（含留白）
    float effect_k = 0.0f;  // 平滑缩放（对齐 1.x updateHeadEffectAnchor._k）

    // 预览提醒效果（头顶 ! 特效显示到该时刻）
    double alert_preview_until = -1.0;

    // 右键菜单状态
    bool menu_open = false;
    bool menu_left = false;  // 向左展开（右侧屏幕空间不足，对齐 1.x menu-left）
    int menu_view = kMenuMain;
    std::string assign_state;  // 动作设定的目标状态（sleeping/working/alert）
    int hover_motion = -1;     // 动作列表悬停项（预览）
    float menu_h = 0.0f;       // 上一帧菜单实测高（点击关闭区域判定）

    // ---- 字体图集构建（init / 语言切换重建共用）----
    void BuildFonts() {
        ImGuiIO& io = ImGui::GetIO();
        const float S = scale;
        font_file_bufs.clear();  // 语言可能变化：重新解析候选字体文件

        // 两套区间：拉丁归主字体（Segoe），CJK 归合并字体（按语言）。
        // 浏览器按码位逐字回退 font-family 栈，ImGui 等价复刻该语义
        ImFontGlyphRangesBuilder latin, cjk;
        latin.AddRanges(io.Fonts->GetGlyphRangesDefault());  // ASCII + Latin-1
        // Latin Ext-A（œ Œ ł š ž 等）+ 引号弯引号/破折号 + €：
        // fr/de/es 变音字母大多在 Latin-1，这几个容易漏
        static const ImWchar kLatinExtra[] = {
            0x0100, 0x017F,  // Latin Extended-A
            0x2013, 0x2014,  // – —
            0x2018, 0x201F,  // ' ' " "（含单双弯引号区间）
            0x20AC, 0x20AC,  // €
            0,
        };
        latin.AddRanges(kLatinExtra);
        latin.AddChar(0x2191);  // ↑
        latin.AddChar(0x2193);  // ↓
        latin.AddChar(0x25CF);  // ●
        latin.AddChar(0x2026);  // …（Latin-1 无，主字体命中）
        latin.AddChar(0x2022);  // •
        latin.AddChar(0x00B7);  // ·
        latin.AddChar(0x2014);  // —
        latin.AddChar(0x2630);  // ☰（状态栏菜单按钮；Segoe 缺由 seguisym 补）
        latin.AddChar(0x25B6);  // ▶（子菜单箭头）
        latin.AddChar(0x25C0);  // ◀（返回）
        latin.AddChar(0x21BA);  // ↺（恢复默认显示）
        latin.AddChar(0x25BE);  // ▾（收起）
        latin.AddChar(0x25B8);  // ▸（已收起）
        latin.AddChar(0x2713);  // ✓（菜单勾选项前缀 .menu-item.active::before）

        cjk.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon()); // 常用简体 + CJK 标点 + 假名
        const std::string& lang = I18n::lang();
        if (lang == "zh-TW")
            cjk.AddRanges(io.Fonts->GetGlyphRangesChineseFull());  // 繁体全覆盖
        else if (lang == "ja")
            cjk.AddRanges(io.Fonts->GetGlyphRangesJapanese());     // 假名 + 常用汉字
        else if (lang == "ko")
            cjk.AddRanges(io.Fonts->GetGlyphRangesKorean());       // 谚文

        // 精确字符集：遍历全部 8 种语言的全部 UI 字符串，把实际用到的
        // 非 ASCII 码位全部烘焙（≥0x2E80 归 CJK：汉字/假名/谚文/CJK 标点/
        // 全角；其余归拉丁）。标准区间表（简体常用/日文/谚文）不能保证
        // 覆盖每个翻译的生僻字 —— 此前切语言出现缺字问号的根因
        I18n::forEach([&latin, &cjk](const char* s) {
            for (const unsigned char* p = (const unsigned char*)s; *p;) {
                unsigned cp = *p++;
                if (cp >= 0xF0 && p[0] && p[1] && p[2]) {
                    cp = ((cp & 0x07u) << 18) | ((p[0] & 0x3Fu) << 12) |
                         ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
                    p += 3;
                } else if (cp >= 0xE0 && p[0] && p[1]) {
                    cp = ((cp & 0x0Fu) << 12) | ((p[0] & 0x3Fu) << 6) | (p[1] & 0x3Fu);
                    p += 2;
                } else if (cp >= 0xC0 && p[0]) {
                    cp = ((cp & 0x1Fu) << 6) | (p[0] & 0x3Fu);
                    p += 1;
                }
                if (cp < 0x80) continue;  // ASCII 已在 Default
                if (cp >= 0x2E80) cjk.AddChar((ImWchar)cp);
                else latin.AddChar((ImWchar)cp);
            }
        });

        // ranges 必须与字体同生命周期（存入 Impl，不能是局部变量）
        latin.BuildRanges(&latin_ranges);
        cjk.BuildRanges(&cjk_ranges);

        // 四档字号各自光栅（12/11/10/9）：所有绘制站点用同档字体绘制，
        // 字号与光栅尺寸一致（AddTextS 兜底对齐），杜绝字形位图缩放。
        // 字号取整：分数字号（如 11.25px）使行高/基线落在半像素上。
        // 低分屏（S<1）小字号保留点阵光栅（清晰度优先），其余平滑 AA
        auto load = [&](float base, ImFont** slot) {
            const float px = (float)(int)(base * S * kTextScale + 0.5f);
            *slot = LoadFontSet(io, px, latin_ranges.Data, cjk_ranges.Data,
                                font_file_bufs, S < 1.0f && px < 13.0f);
        };
        load(12.0f, &font_12);
        load(11.0f, &font_11);
        load(10.0f, &font_10);
        load(9.0f, &font_9);
        if (!font_12) {
            fprintf(stderr, "[UI] CJK font not found, fallback to default\n");
            font_12 = io.Fonts->AddFontDefault();
        }
        if (!font_11) font_11 = font_12;
        if (!font_10) font_10 = font_12;
        if (!font_9) font_9 = font_10;
    }

    // ---- 头顶特效精灵烘焙（Impl 成员；需 GL 上下文，首帧懒构建）----
    // 覆盖图（0..1）→ 纹理：白影（shadow_px 硬偏移，无模糊）→ 辉光
    // （blur_px 高斯模糊 × glow_a）→ 主体，src-over 自下而上合成。
    // blur/shadow 单位为 1x CSS px，内部 ×bake 放大。锚点 = cov 中
    // (anchor_x, anchor_y) + pad
    bool FxBake(FxSprite& out, const unsigned char* bmp, int bw, int bh,
                float blur_px, const float glow_rgb[3], float glow_a,
                float shadow_px, const float body_rgb[3], float bake,
                int anchor_x, int anchor_y) {
        const float sigma = blur_px * bake * 0.5f;  // CSS blur 半径 ≈ 2σ
        const int pad =
            (int)(blur_px * bake * 1.6f + shadow_px * bake + 3.0f);
        const int W = bw + pad * 2, H = bh + pad * 2;
        std::vector<float> cov((size_t)W * H, 0.0f);
        for (int y = 0; y < bh; y++)
            for (int x = 0; x < bw; x++)
                cov[(size_t)(y + pad) * W + (x + pad)] =
                    bmp[y * bw + x] / 255.0f;
        std::vector<float> glow(cov);
        FxBlur(glow.data(), W, H, sigma);
        const int so = (int)(shadow_px * bake + 0.5f);
        std::vector<uint8_t> rgba((size_t)W * H * 4);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const size_t i = (size_t)y * W + x;
                float dr = 0, dg = 0, db = 0, da = 0;
                auto over = [&](float cr, float cg, float cb, float ca) {
                    dr = cr * ca + dr * (1.0f - ca);
                    dg = cg * ca + dg * (1.0f - ca);
                    db = cb * ca + db * (1.0f - ca);
                    da = ca + da * (1.0f - ca);
                };
                if (so > 0) {  // 白影：整图右下位移 so 像素
                    const int sx = x - so, sy = y - so;
                    const float a =
                        (sx >= 0 && sx < W && sy >= 0 && sy < H)
                            ? cov[(size_t)sy * W + sx]
                            : 0.0f;
                    if (a > 0.0f) over(1, 1, 1, a);
                }
                {
                    const float a = glow[i] * glow_a;
                    if (a > 0.0f)
                        over(glow_rgb[0], glow_rgb[1], glow_rgb[2], a);
                }
                {
                    const float a = cov[i];
                    if (a > 0.0f)
                        over(body_rgb[0], body_rgb[1], body_rgb[2], a);
                }
                uint8_t* px = &rgba[i * 4];
                px[0] = (uint8_t)(dr * 255.0f + 0.5f);
                px[1] = (uint8_t)(dg * 255.0f + 0.5f);
                px[2] = (uint8_t)(db * 255.0f + 0.5f);
                px[3] = (uint8_t)(da * 255.0f + 0.5f);
            }
        GLuint t = 0;
        glGenTextures(1, &t);
        if (!t) return false;
        glBindTexture(GL_TEXTURE_2D, t);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
        out.tex = t;
        out.w = W;
        out.h = H;
        out.ax = (float)(pad + anchor_x);
        out.ay = (float)(pad + anchor_y);
        return true;
    }

    // 字形精灵：stb_truetype 光栅化真字体字形（1.x 同款 Segoe UI Bold）
    bool FxBakeGlyph(FxSprite& out, const std::vector<uint8_t>& font, int ch,
                     float px, float blur_px, const float glow_rgb[3],
                     float glow_a, float shadow_px, const float body_rgb[3]) {
        const float bake = 3.0f;
        stbtt_fontinfo fi;
        if (!stbtt_InitFont(&fi, font.data(),
                            stbtt_GetFontOffsetForIndex(font.data(), 0)))
            return false;
        const int gi = stbtt_FindGlyphIndex(&fi, ch);
        if (gi == 0) return false;
        const float scale = stbtt_ScaleForPixelHeight(&fi, px * bake);
        int w = 0, h = 0, xo = 0, yo = 0;
        unsigned char* bmp =
            stbtt_GetGlyphBitmap(&fi, 0, scale, gi, &w, &h, &xo, &yo);
        if (!bmp || w <= 0 || h <= 0) {
            if (bmp) stbtt_FreeBitmap(bmp, nullptr);
            return false;
        }
        const bool ok = FxBake(out, bmp, w, h, blur_px, glow_rgb, glow_a,
                               shadow_px, body_rgb, bake, w / 2, h);
        stbtt_FreeBitmap(bmp, nullptr);
        return ok;
    }

    // 工作点精灵：6px 实心圆 + box-shadow 0 0 6px rgba(255,200,50,0.8)
    bool FxBakeDot(FxSprite& out) {
        const float bake = 3.0f;
        const float r = 3.0f * bake;
        const int s = (int)(r * 2 + 2);
        std::vector<unsigned char> bmp((size_t)s * s, 0);
        for (int y = 0; y < s; y++)
            for (int x = 0; x < s; x++) {
                const float dx = x + 0.5f - s * 0.5f;
                const float dy = y + 0.5f - s * 0.5f;
                const float d = sqrtf(dx * dx + dy * dy);
                float a = 1.0f - (d - (r - 1.0f)) * 0.5f;  // 2px 抗锯齿边
                if (a < 0.0f) a = 0.0f;
                if (a > 1.0f) a = 1.0f;
                bmp[(size_t)y * s + x] = (unsigned char)(a * 255.0f + 0.5f);
            }
        const float glow[] = {1.0f, 0.784f, 0.196f};  // #ffc832
        const float body[] = {1.0f, 0.784f, 0.196f};
        return FxBake(out, bmp.data(), s, s, 6.0f, glow, 0.8f, 0.0f, body,
                      bake, s / 2, s);
    }

    void BuildFxSprites() {
        fx_built = true;
        char wdir[MAX_PATH];
        if (!GetWindowsDirectoryA(wdir, MAX_PATH)) return;
        char p1[MAX_PATH], p2[MAX_PATH];
        snprintf(p1, sizeof(p1), "%s\\Fonts\\segoeuib.ttf", wdir);
        snprintf(p2, sizeof(p2), "%s\\Fonts\\msyhbd.ttc", wdir);
        std::map<std::string, std::vector<uint8_t>> bufs;
        std::vector<uint8_t>* font = FontFileBuf(bufs, p1);
        if (!font) font = FontFileBuf(bufs, p2);
        if (!font) return;
        // 1.x styles.css 同参数：Z 13/16/19 weight 800，
        // text-shadow 0 0 6px rgba(100,150,255,.6) + 1px 1px 0 #fff
        const float gz[] = {0.392f, 0.588f, 1.0f};  // #6496FF
        const float bz[] = {0.608f, 0.722f, 1.0f};  // #9BB8FF
        static const float kZ[3] = {13.0f, 16.0f, 19.0f};
        for (int i = 0; i < 3; i++)
            FxBakeGlyph(fx_z[i], *font, 'Z', kZ[i], 6.0f, gz, 0.6f, 1.0f, bz);
        // ! 28 weight 900，text-shadow 0 0 10px rgba(255,68,68,.7)+2px 2px #fff
        const float ga[] = {1.0f, 0.267f, 0.267f};  // #FF4444
        FxBakeGlyph(fx_bang, *font, '!', 28.0f, 10.0f, ga, 0.7f, 2.0f, ga);
        FxBakeDot(fx_dot);
        printf("[UI] fx sprites: z=%d%d%d bang=%d dot=%d\n", fx_z[0].tex ? 1 : 0,
               fx_z[1].tex ? 1 : 0, fx_z[2].tex ? 1 : 0, fx_bang.tex ? 1 : 0,
               fx_dot.tex ? 1 : 0);
    }

    // 画精灵：锚点对齐 (cx,cy)，disp_scale 为 1x CSS px 尺度显示比例
    // （含 k/S/动画缩放；精灵按 3x 烘焙故除 3）
    void DrawFx(ImDrawList* dl, const FxSprite& s, float cx, float cy,
                float disp_scale, float alpha) {
        if (!s.tex || alpha <= 0.01f) return;
        const float f = disp_scale / 3.0f;
        const ImVec2 a(cx - s.ax * f, cy - s.ay * f);
        dl->AddImage((ImTextureID)(intptr_t)s.tex, a,
                     ImVec2(a.x + s.w * f, a.y + s.h * f), ImVec2(0, 0),
                     ImVec2(1, 1),
                     IM_COL32(255, 255, 255, (int)(alpha * 255.0f)));
    }

    // ---- 监控行（[彩色标签] [数值] [迷你折线图]）----
    // 紧凑监控行：无分隔线（用户反馈"每条都有线"观感杂乱，1.x 的
    // 0.04 分隔线在此 DPI 下反而显眼），靠均匀行距本身建立节奏
    void MonitorRow(const char* label, ImU32 label_col, const char* value,
                    const RingHistory* hist, float fixed_max, ImU32 spark_col) {
        const float S = scale;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 rmin = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetWindowSize().x;
        // 行高 = 文字高 + 4px 合计余量（每边 ~2px，CJK 不裁切）。
        // 行距推进用 SetCursorScreenPos 而非 Dummy：Dummy 项与项之间
        // 还会叠加 ItemSpacing.y（≈4×S），公式与实际行距不一致且随
        // DPI 变化——之前"行距偏松且不均"的根源
        const float font = 10.0f * S * kTextScale;
        const float row_h = font + 4.0f * S;
        const float midy = rmin.y + row_h * 0.5f;

        // 标签（.m-txt 10px 600，min-width 28）——墨迹居中
        AddTextS(dl,font_10, font,
                    ImVec2(rmin.x + 8.0f * S,
                           TextCenteredY(font_10, font, label, midy)),
                    label_col, label);
        // 数值（.monitor-value 10px 600 白 0.92）。x 跟随标签实际宽度：
        // 各语言标签长度差异大（Mémoire/네트워크/プロジェクト…），
        // 固定 44px 会让长标签叠到数值上（44/130 为中文两字标签的基准）
        const float label_w = TextW(font_10, font, label);
        const float value_x =
            (std::max)(44.0f * S, 8.0f * S + label_w + 6.0f * S);
        AddTextS(dl,font_10, font,
                    ImVec2(rmin.x + value_x,
                           TextCenteredY(font_10, font, value, midy)),
                    IM_COL32(255, 255, 255, 235), value);
        // 折线图（.monitor-spark 高 12）：起点跟随数值宽度，至少留 20px
        // 宽（↓↑双速率等长数值在窄语言下可能挤掉折线，此时不画）
        if (hist && hist->count > 1) {
            const float value_w = TextW(font_10, font, value);
            const float spark_x0 =
                (std::max)(130.0f * S, value_x + value_w + 6.0f * S);
            if (rmin.x + w - 8.0f * S - spark_x0 > 20.0f * S) {
                const ImVec2 a(rmin.x + spark_x0, midy - 6.0f * S);
                const ImVec2 b(rmin.x + w - 8.0f * S, midy + 6.0f * S);
                DrawSpark(dl, a, b, hist, fixed_max, spark_col);
            }
        }
        // 精确推进到行底（无 ItemSpacing 参与，行距严格一致）
        ImGui::SetCursorScreenPos(ImVec2(rmin.x, rmin.y + row_h));
    }

    // ---- 菜单条目（.menu-item：12px 白 0.8，padding 7x12，圆角 4，
    // hover 白 10% 底全白；active 蓝 #6496ff + ✓ 前缀；danger 红）----
    struct MenuRowResult {
        bool clicked = false;
        bool hovered = false;
    };

    MenuRowResult MenuRow(const char* id, const char* label, bool active, bool danger,
                          const char* hint, bool has_arrow) {
        const float S = scale;
        const float font_size = 12.0f * S * kTextScale;
        const float font_hint = 10.0f * S * kTextScale;
        const float pad_v = 7.0f * S;
        const float pad_h = 12.0f * S;
        const float line = font_size + 3.0f * S;
        const float w = ImGui::GetWindowSize().x - 8.0f * S;  // 窗口 padding 4x2
        const ImVec2 size(w, pad_v * 2.0f + line);

        MenuRowResult r;
        r.clicked = ImGui::InvisibleButton(id, size);
        r.hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 mmin = ImGui::GetItemRectMin();
        const ImVec2 mmax = ImGui::GetItemRectMax();

        // hover 背景（danger: rgba(255,68,68,0.15)，普通: rgba(255,255,255,0.1)）
        if (r.hovered)
            dl->AddRectFilled(mmin, mmax, danger ? IM_COL32(255, 68, 68, 38)
                                                 : IM_COL32(255, 255, 255, 26),
                              4.0f * S);

        // 文字颜色 + active ✓ 前缀（.menu-item.active::before '✓ '）
        char text[160];
        if (active)
            snprintf(text, sizeof(text), "\xE2\x9C\x93 %s", label);
        else
            snprintf(text, sizeof(text), "%s", label);
        ImU32 col;
        if (danger)
            col = r.hovered ? IM_COL32(0xFF, 0x66, 0x66, 255) : IM_COL32(255, 100, 100, 204);
        else if (active)
            col = IM_COL32(0x64, 0x96, 0xFF, 255);
        else
            col = r.hovered ? IM_COL32(255, 255, 255, 255) : IM_COL32(255, 255, 255, 204);
        // 行内文字按墨迹垂直居中（CJK 墨迹偏行框下半，行高居中会偏上）
        const float row_mid = (mmin.y + mmax.y) * 0.5f;
        AddTextS(dl,font_12, font_size,
                    ImVec2(mmin.x + pad_h,
                           TextCenteredY(font_12, font_size, text, row_mid)),
                    col, text);

        // 右侧提示（.menu-hint 10px 白 0.35）
        if (hint && hint[0]) {
            const float hw = TextW(font_10, 10.0f * S * kTextScale, hint);
            float hx = mmax.x - pad_h;
            if (has_arrow) hx -= 18.0f * S;
            AddTextS(dl,font_10, 10.0f * S * kTextScale,
                        ImVec2(hx - hw,
                               TextCenteredY(font_10, 10.0f * S * kTextScale,
                                             hint, row_mid)),
                        IM_COL32(255, 255, 255, 89), hint);
        }
        // 子菜单箭头（▶ 10px 白 0.4，hover 白 0.95 + 右移 2px）
        if (has_arrow) {
            const char* arrow = "\xE2\x96\xB6";
            const float aw = TextW(font_10, 10.0f * S * kTextScale, arrow);
            AddTextS(dl,font_10, 10.0f * S * kTextScale,
                        ImVec2(mmax.x - pad_h - aw + (r.hovered ? 2.0f * S : 0.0f),
                               TextCenteredY(font_10, 10.0f * S * kTextScale,
                                             arrow, row_mid)),
                        r.hovered ? IM_COL32(255, 255, 255, 242)
                                  : IM_COL32(255, 255, 255, 102),
                        arrow);
        }
        return r;
    }

    // 菜单分区标签（.menu-label：10px 白 0.4，padding 6 12 2）
    void MenuLabel(const char* text) {
        const float S = scale;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 mmin = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetWindowSize().x - 8.0f * S;
        const float h = 10.0f * S + 8.0f * S;
        AddTextS(dl,font_10, 10.0f * S * kTextScale, ImVec2(mmin.x + 12.0f * S, mmin.y + 6.0f * S),
                    IM_COL32(255, 255, 255, 102), text);
        ImGui::Dummy(ImVec2(w, h));
    }

    // 分隔线（.menu-divider：1px 白 0.08，上下 margin 4）
    void MenuDivider() {
        const float S = scale;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 mmin = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetWindowSize().x - 8.0f * S;
        dl->AddLine(ImVec2(mmin.x, mmin.y + 4.0f * S),
                    ImVec2(mmin.x + w, mmin.y + 4.0f * S),
                    IM_COL32(255, 255, 255, 20), 1.0f);
        ImGui::Dummy(ImVec2(w, 8.0f * S + 1.0f));
    }

    // 动作网格单元（#motion-list .menu-item：padding 6x4，居中，12px）
    // 返回点击/悬停；grid 3 列由调用方排布
    MenuRowResult MotionCell(const char* id, const char* label, bool active) {
        const float S = scale;
        const float font_size = 12.0f * S * kTextScale;
        const float pad_v = 6.0f * S;
        const float w = ImGui::GetWindowSize().x - 8.0f * S;
        const float cell_w = (w - 4.0f * S) / 3.0f;
        const ImVec2 size(cell_w, pad_v * 2.0f + font_size);

        MenuRowResult r;
        r.clicked = ImGui::InvisibleButton(id, size);
        r.hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 mmin = ImGui::GetItemRectMin();
        const ImVec2 mmax = ImGui::GetItemRectMax();
        if (r.hovered)
            dl->AddRectFilled(mmin, mmax, IM_COL32(255, 255, 255, 26), 4.0f * S);

        // 超宽截断（grid 单元 ~96px，动作名如 "FlickLeft 12"）
        char name[96];
        TruncateUtf8(label, cell_w - 8.0f * S, name, sizeof(name), font_12, font_size);
        char text[104];
        if (active)
            snprintf(text, sizeof(text), "\xE2\x9C\x93 %s", name);
        else
            snprintf(text, sizeof(text), "%s", name);
        const float tw = TextW(font_12, font_size, text);
        AddTextS(dl,font_12, font_size,
                    ImVec2(mmin.x + (cell_w - tw) * 0.5f,
                           TextCenteredY(font_12, font_size, text,
                                         (mmin.y + mmax.y) * 0.5f)),
                    active ? IM_COL32(0x64, 0x96, 0xFF, 255)
                           : (r.hovered ? IM_COL32(255, 255, 255, 255)
                                        : IM_COL32(255, 255, 255, 204)),
                    text);
        return r;
    }

    // 动作设定状态行（.menu-item.settings-row：状态名 flex:1 左侧，
    // 当前动作名 11px 白 0.5 右侧 + ▶ 箭头；hover 白 10% 底）
    MenuRowResult SettingsRow(const char* id, const char* label,
                              const char* motion) {
        const float S = scale;
        const float font_size = 12.0f * S * kTextScale;
        const float font_motion = 11.0f * S * kTextScale;
        const float pad_v = 7.0f * S;
        const float pad_h = 12.0f * S;
        const float line = font_size + 3.0f * S;
        const float w = ImGui::GetWindowSize().x - 8.0f * S;
        const ImVec2 size(w, pad_v * 2.0f + line);

        MenuRowResult r;
        r.clicked = ImGui::InvisibleButton(id, size);
        r.hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 mmin = ImGui::GetItemRectMin();
        const ImVec2 mmax = ImGui::GetItemRectMax();

        if (r.hovered)
            dl->AddRectFilled(mmin, mmax, IM_COL32(255, 255, 255, 26), 4.0f * S);

        // 行内文字按墨迹垂直居中
        const float row_mid = (mmin.y + mmax.y) * 0.5f;
        // 状态名（.settings-state-label flex:1，普通 menu-item 配色）
        AddTextS(dl,font_12, font_size,
                    ImVec2(mmin.x + pad_h,
                           TextCenteredY(font_12, font_size, label, row_mid)),
                    r.hovered ? IM_COL32(255, 255, 255, 255)
                              : IM_COL32(255, 255, 255, 204),
                    label);
        // ▶ 箭头（最右）
        const char* arrow = "\xE2\x96\xB6";
        const float aw = TextW(font_10, 10.0f * S * kTextScale, arrow);
        AddTextS(dl,font_10, 10.0f * S * kTextScale,
                    ImVec2(mmax.x - pad_h - aw + (r.hovered ? 2.0f * S : 0.0f),
                           TextCenteredY(font_10, 10.0f * S * kTextScale,
                                         arrow, row_mid)),
                    r.hovered ? IM_COL32(255, 255, 255, 242)
                              : IM_COL32(255, 255, 255, 102),
                    arrow);
        // 当前动作名（.settings-state-name 11px 白 0.5，右对齐到箭头左侧 gap 4）
        if (motion && motion[0]) {
            char name[96];
            TruncateUtf8(motion, mmax.x - pad_h - aw - 4.0f * S -
                            (mmin.x + pad_h + TextW(font_12, font_size, label)) -
                            8.0f * S,
                         name, sizeof(name), font_11, font_motion);
            const float mw = TextW(font_11, font_motion, name);
            AddTextS(dl,font_11, font_motion,
                        ImVec2(mmax.x - pad_h - aw - 4.0f * S - mw,
                               TextCenteredY(font_11, font_motion, name, row_mid)),
                        IM_COL32(255, 255, 255, 128), name);
        }
        return r;
    }

    // ---- 形象缩略图纹理（1.x .char-thumb；stb 解码 PNG/GIF 首帧 → GL 纹理，
    // 按路径缓存；加载失败缓存 0 用首字母占位）----
    // 显存优化：缩略图显示尺寸仅 64×S px，解码后按 2 的幂减半到 ≤128
    // 再上传（GIF 首帧可达 480² ≈ 0.9MB 纹理 → 128² ≈ 64KB）
    struct ThumbTex {
        GLuint tex = 0;
        int w = 0, h = 0;
    };
    std::map<std::string, ThumbTex> thumb_cache;

    const ThumbTex& ThumbTexture(const std::string& path) {
        auto it = thumb_cache.find(path);
        if (it != thumb_cache.end()) return it->second;
        ThumbTex t;
        int channels = 0;
        unsigned char* data = stbi_load(path.c_str(), &t.w, &t.h, &channels,
                                         STBI_rgb_alpha);
        if (data && t.w > 0 && t.h > 0) {
            // 就地减半到 ≤128（缩略图显示尺寸 64px×DPI，128 已有 2 倍余量）
            while (t.w > 128 || t.h > 128) {
                if (!HalveRGBA(data, &t.w, &t.h)) break;
            }
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.w, t.h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, data);
            t.tex = tex;
        }
        if (data) stbi_image_free(data);
        return thumb_cache.emplace(path, t).first->second;
    }

    // ---- 形象卡片（1.x .char-card：缩略图 64 圆角 6 + 名称；active 蓝底蓝框
    // + ✓ 右上角；hover 白 10% 底）。返回是否点击 ----
    bool CharCard(const char* id, const MenuEntry& e, float card_w) {
        const float S = scale;
        const float thumb = 64.0f * S;
        const float pad_t = 8.0f * S, pad_s = 6.0f * S, pad_b = 6.0f * S;
        const float name_font = 11.0f * S * kTextScale;
        const float name_h = name_font + 2.0f * S;
        const ImVec2 size(card_w, pad_t + thumb + 6.0f * S + name_h + pad_b);

        const bool clicked = ImGui::InvisibleButton(id, size);
        const bool hov = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 cmin = ImGui::GetItemRectMin();
        const ImVec2 cmax = ImGui::GetItemRectMax();

        // 卡片底色/描边（.char-card / :hover / .active）
        ImU32 bg = IM_COL32(255, 255, 255, 10);       // rgba(255,255,255,0.04)
        ImU32 border = IM_COL32(255, 255, 255, 15);   // rgba(255,255,255,0.06)
        if (e.checked) {
            bg = IM_COL32(0x64, 0x96, 0xFF, 41);      // rgba(100,150,255,0.16)
            border = IM_COL32(0x64, 0x96, 0xFF, 153); // rgba(100,150,255,0.6)
        } else if (hov) {
            bg = IM_COL32(255, 255, 255, 26);         // 0.1
            border = IM_COL32(255, 255, 255, 46);     // 0.18
        }
        dl->AddRectFilled(cmin, cmax, bg, 6.0f * S);
        dl->AddRect(cmin, cmax, border, 6.0f * S, 0, 1.0f * S);

        // 缩略图（64x64 圆角 6；object-fit: cover 居中裁剪）
        const ImVec2 tmin(cmin.x + (card_w - thumb) * 0.5f, cmin.y + pad_t);
        const ImVec2 tmax(tmin.x + thumb, tmin.y + thumb);
        const ThumbTex& t =
            e.thumb.empty() ? ThumbTex{} : ThumbTexture(e.thumb);
        if (t.tex) {
            // cover：宽图裁左右 / 高图裁上下（UV 居中裁剪）
            ImVec2 uv0(0.0f, 0.0f), uv1(1.0f, 1.0f);
            const float img_ar = (float)t.w / (float)t.h;
            if (img_ar > 1.0f) {
                const float crop = 1.0f - 1.0f / img_ar;
                uv0.x = crop * 0.5f;
                uv1.x = 1.0f - crop * 0.5f;
            } else if (img_ar < 1.0f) {
                const float crop = 1.0f - img_ar;
                uv0.y = crop * 0.5f;
                uv1.y = 1.0f - crop * 0.5f;
            }
            dl->AddImageRounded((ImTextureID)(intptr_t)t.tex, tmin, tmax, uv0,
                                uv1, IM_COL32(255, 255, 255, 255), 6.0f * S);
        } else {
            // 首字母占位（26px 白 0.55 居中，淡底）
            dl->AddRectFilled(tmin, tmax, IM_COL32(255, 255, 255, 15), 6.0f * S);
            const std::string letter = FirstUtf8CharUpper(e.label);
            const float fs = 26.0f * S;
            const float lw = TextW(font_12, fs, letter.c_str());
            AddTextS(dl,font_12, fs,
                        ImVec2(tmin.x + (thumb - lw) * 0.5f,
                               tmin.y + (thumb - fs) * 0.5f),
                        IM_COL32(255, 255, 255, 140), letter.c_str());
        }

        // 名称（11px 白 0.85，居中截断省略号）
        {
            char name[96];
            TruncateUtf8(e.label.c_str(), card_w - pad_s * 2.0f, name,
                         sizeof(name), font_11, name_font);
            const float nw = TextW(font_11, name_font, name);
            AddTextS(dl,font_11, name_font,
                        ImVec2(cmin.x + (card_w - nw) * 0.5f, cmax.y - pad_b - name_h),
                        IM_COL32(255, 255, 255, 217), name);
        }

        // active ✓ 右上角（11px #6496ff 700）
        if (e.checked) {
            const char* check = "\xE2\x9C\x93";
            const float fs = font_11->FontSize;
            const float cw = TextW(font_11, fs, check);
            AddTextS(dl,font_11, fs,
                        ImVec2(cmax.x - 6.0f * S - cw, cmin.y + 4.0f * S),
                        IM_COL32(0x64, 0x96, 0xFF, 255), check);
        }
        return clicked;
    }
};

UIRenderer::UIRenderer() : impl_(new Impl()) {}
UIRenderer::~UIRenderer() { shutdown(); delete impl_; }

bool UIRenderer::init(GLFWwindow* window) {
    impl_->window = window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // 不写 imgui.ini（只读软件，不落垃圾文件）

    // ---- DPI：1.x WebView 逻辑像素 -> 原生物理像素 ----
    {
        float xs = 1.0f, ys = 1.0f;
        glfwGetWindowContentScale(window, &xs, &ys);
        if (xs >= 0.5f) impl_->scale = xs;
    }
    const float S = impl_->scale;
    printf("[UI] dpi scale: %.2f\n", S);

    // ---- 样式：对齐 1.x 面板观感（深色半透明 + 大圆角 + 细白描边）----
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 12.0f * S;
    style.WindowBorderSize = 1.0f;
    style.WindowPadding = ImVec2(10.0f * S, 8.0f * S);
    style.ItemSpacing = ImVec2(6.0f * S, 4.0f * S);
    style.ScrollbarSize = 4.0f * S;      // 1.x 4px 细滚动条
    style.ScrollbarRounding = 2.0f * S;
    style.ChildRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg] = Rgb(30, 30, 40);        // 1.x #status-bar 背景
    style.Colors[ImGuiCol_Border] = Rgb(255, 255, 255, 20);   // rgba(255,255,255,0.08)
    style.Colors[ImGuiCol_Text] = Rgb(255, 255, 255, 235);
    style.Colors[ImGuiCol_TextDisabled] = Rgb(150, 152, 160);
    style.Colors[ImGuiCol_ScrollbarBg] = Rgb(0, 0, 0, 0);
    style.Colors[ImGuiCol_ScrollbarGrab] = Rgb(255, 255, 255, 38);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = Rgb(255, 255, 255, 64);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = Rgb(255, 255, 255, 64);

    // ---- 字体（按当前语言的字形范围构建；含箭头/圆点/菜单符号等）----
    impl_->BuildFonts();
    impl_->status_h = 66.0f * S;

    ImGui_ImplGlfw_InitForOpenGL(window, false);  // 不接管事件（我们自己的回调）
    ImGui_ImplOpenGL3_Init("#version 330 core");
    impl_->imgui_ready = true;
    printf("[UI] ImGui initialized (Windows)\n");
    return true;
}

bool UIRenderer::init(int display_w, int display_h) {
    (void)display_w;
    (void)display_h;
    printf("[UI] headless overlay\n");
    return true;
}

void UIRenderer::beginFrame() {
    if (impl_->imgui_ready) {
        glfwGetWindowSize(impl_->window, &impl_->win_w, &impl_->win_h);
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }
}

// 窗口层 GLFW 回调转发（install_callbacks=false，由窗口层喂事件给 ImGui）
void UIRenderer::forwardMouseButton(int button, int action, int mods) {
    if (impl_->imgui_ready)
        ImGui_ImplGlfw_MouseButtonCallback(impl_->window, button, action, mods);
}

void UIRenderer::forwardCursorPos(double x, double y) {
    if (impl_->imgui_ready)
        ImGui_ImplGlfw_CursorPosCallback(impl_->window, x, y);
}

void UIRenderer::forwardScroll(double x, double y) {
    if (impl_->imgui_ready)
        ImGui_ImplGlfw_ScrollCallback(impl_->window, x, y);
}

void UIRenderer::forwardKey(int key, int scancode, int action, int mods) {
    if (impl_->imgui_ready)
        ImGui_ImplGlfw_KeyCallback(impl_->window, key, scancode, action, mods);
    if (key == 256 /*GLFW_KEY_ESC*/ && action == 1 /*GLFW_PRESS*/ && impl_->menu_open)
        closeMenu();
}

void UIRenderer::pushMetrics(const SysMetrics& m) {
    impl_->cpu.Push(m.cpu_usage);
    impl_->ram.Push(m.mem_total
                        ? (float)((double)m.mem_used / (double)m.mem_total * 100.0)
                        : 0.0f);
    impl_->gpu.Push(m.gpu_usage);
    impl_->vram.Push(m.vram_total
                         ? (float)((double)m.vram_used / (double)m.vram_total * 100.0)
                         : 0.0f);
    impl_->net.Push((float)(m.net_rx_rate + m.net_tx_rate));
}

float UIRenderer::statusBarHeight() const { return impl_->status_h; }
float UIRenderer::monitorHeight() const { return impl_->monitor_h; }
float UIRenderer::dpiScale() const { return impl_->scale; }

float UIRenderer::menuExtraWidth() const {
    if (!impl_->menu_open) return 0.0f;
    // 1.x #context-menu min-width 210 + padding 4x2 + 与角色区间距 8；
    // 切换形象视图加宽到 280（两列缩略图卡片）
    const float menu_w =
        (impl_->menu_view == kMenuModels ? 280.0f : 218.0f) * impl_->scale;
    return 8.0f * impl_->scale + menu_w;
}

void UIRenderer::setModelRect(const Rect& r, bool tight_bounds) {
    impl_->model_rect = r;
    impl_->model_bounds_tight = tight_bounds;
    impl_->has_model_rect = r.w > 0.0f && r.h > 0.0f;
}

void UIRenderer::setLayout(float canvas_h, float panel_x, float panel_w,
                           float panel_gap, bool mini) {
    impl_->canvas_h = canvas_h;
    impl_->panel_x = panel_x;
    impl_->panel_w = panel_w;
    impl_->panel_gap = panel_gap;
    impl_->mini = mini;
}

void UIRenderer::openMenu() {
    impl_->menu_open = true;
    impl_->menu_view = kMenuMain;
    impl_->hover_motion = -1;
    // 通知主程序刷新 autostart / hook / 模型目录等缓存（避免菜单内每帧 HTTP）
    if (on_menu_open) on_menu_open();
}

void UIRenderer::closeMenu() {
    impl_->menu_open = false;
    impl_->hover_motion = -1;
}

void UIRenderer::openMenuView(const std::string& view) {
    openMenu();
    if (view == "models") impl_->menu_view = kMenuModels;
    else if (view == "motion-play") impl_->menu_view = kMenuMotionPlay;
    else if (view == "motion-assign") impl_->menu_view = kMenuMotionAssign;
    else if (view == "settings") impl_->menu_view = kMenuSettings;
    else if (view == "language") impl_->menu_view = kMenuLanguage;
    else if (view == "visibility") impl_->menu_view = kMenuVisibility;
    else impl_->menu_view = kMenuMain;  // "main" / 未知值
}

bool UIRenderer::isMenuOpen() const { return impl_->menu_open; }

bool UIRenderer::isMotionViewActive() const {
    return impl_->menu_open &&
           (impl_->menu_view == kMenuMotionPlay ||
            impl_->menu_view == kMenuMotionAssign);
}

void UIRenderer::setMenuLeft(bool left) { impl_->menu_left = left; }

bool UIRenderer::isPointInMenu(float x, float y) const {
    if (!impl_->menu_open) return false;
    const float S = impl_->scale;
    const float menu_w =  // 1.x min-width 210（切换形象视图 280）
        (impl_->menu_view == kMenuModels ? 280.0f : 218.0f) * S;
    const float base_w = impl_->mini ? 130.0f * S : 260.0f * S;   // 常规窗口宽
    const float mx = impl_->menu_left ? 8.0f * S : base_w + 8.0f * S;
    return x >= mx && x <= mx + menu_w && y >= 4.0f * S &&
           y <= (float)impl_->win_h - 4.0f * S;
}

bool UIRenderer::isPointClickable(float x, float y) const {
    // 1.x setupClickThrough 的 clickable regions：模型 bounds（GIF 模式
    // 主循环注入整个画布）+ 状态栏 + 监控面板（可见且未收起为 0 高）。
    // 菜单区不含——由调用方按 isPointInMenu 另判（可点但不拖拽）。
    const Impl* p = impl_;
    if (p->has_model_rect) {
        const Rect& b = p->model_rect;
        if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h)
            return true;
    }
    const float sb_top = p->canvas_h + p->panel_gap;
    const float x0 = p->panel_x, x1 = p->panel_x + p->panel_w;
    if (x >= x0 && x <= x1 && y >= sb_top && y <= sb_top + p->status_h)
        return true;
    if (p->monitor_h > 0.0f) {
        const float mon_top = sb_top + p->status_h + p->panel_gap;
        if (x >= x0 && x <= x1 && y >= mon_top && y <= mon_top + p->monitor_h)
            return true;
    }
    return false;
}

std::vector<Rect> UIRenderer::clickRegions() const {
    // 对齐 1.x collectRegions：模型 bounds + 状态栏 + 监控（可见时）+
    // 菜单（打开时）；坐标为客户区物理像素
    std::vector<Rect> out;
    const Impl* p = impl_;
    if (p->has_model_rect) out.push_back(p->model_rect);
    const float sb_top = p->canvas_h + p->panel_gap;
    if (p->status_h > 0.0f)
        out.push_back(Rect{p->panel_x, sb_top, p->panel_w, p->status_h});
    if (p->monitor_h > 0.0f) {
        const float mon_top = sb_top + p->status_h + p->panel_gap;
        out.push_back(Rect{p->panel_x, mon_top, p->panel_w, p->monitor_h});
    }
    if (p->menu_open) {
        const float S = p->scale;
        const float menu_w =  // 1.x min-width 210（切换形象视图 280）
            (p->menu_view == kMenuModels ? 280.0f : 218.0f) * S;
        const float base_w = p->mini ? 130.0f * S : 260.0f * S;
        const float mx = p->menu_left ? 8.0f * S : base_w + 8.0f * S;
        out.push_back(Rect{mx, 4.0f * S, menu_w,
                           (float)p->win_h - 8.0f * S});
    }
    return out;
}

void UIRenderer::reloadFonts() { impl_->fonts_dirty = true; }

void UIRenderer::setScale(float s) {
    if (s < 0.5f || s > 4.0f) return;
    if (s == impl_->scale) return;
    impl_->scale = s;
    impl_->fonts_dirty = true;  // 字号随 scale 重建图集（帧末生效）
}

void UIRenderer::previewAlert() {
    impl_->alert_preview_until = ImGui::GetTime() + 3.0;
}

// ---------------------------------------------------------------------------
// 状态颜色（1.x 配色常量）
// ---------------------------------------------------------------------------
namespace {
// 状态头圆点 / 头顶特效
constexpr ImU32 kColSleepDot = IM_COL32(0x64, 0x96, 0xFF, 255);
constexpr ImU32 kColWorkDot = IM_COL32(0xFF, 0xC8, 0x32, 255);
constexpr ImU32 kColAlertDot = IM_COL32(0xFF, 0x44, 0x44, 255);

// 项目状态（styles.css .project-dot / .project-item.status-*）
struct ProjectStyle {
    ImU32 border;    // 左边框色
    ImU32 dot;       // 状态点
    float dot_period;    // 点闪烁周期（0 = 不闪）
    float glow_period;   // 行背景呼吸周期（0 = 静态）
    ImU32 accent;    // 文字强调色（working/thinking/tool-use 名称+状态文字）
    bool name_accent;     // 名称是否用强调色
    bool name_red;        // confirmation-needed 名称红
};
const ProjectStyle& StyleFor(const SessionInfo& s) {
    static const ProjectStyle kIdle = {
        IM_COL32(0x64, 0x96, 0xFF, 102), IM_COL32(0x64, 0x96, 0xFF, 128),
        0.0f, 0.0f, 0, false, false};
    static const ProjectStyle kWorking = {
        IM_COL32(0xFF, 0xC8, 0x32, 153), IM_COL32(0xFF, 0xC8, 0x32, 255),
        1.0f, 0.0f, IM_COL32(0xFF, 0xC8, 0x32, 255), true, false};
    static const ProjectStyle kThinking = {
        IM_COL32(0x9B, 0x8C, 0xFF, 153), IM_COL32(0x9B, 0x8C, 0xFF, 255),
        1.0f, 0.0f, IM_COL32(0x9B, 0x8C, 0xFF, 255), true, false};
    static const ProjectStyle kToolUse = {
        IM_COL32(0xFF, 0x9B, 0x32, 153), IM_COL32(0xFF, 0x9B, 0x32, 255),
        0.7f, 0.0f, IM_COL32(0xFF, 0x9B, 0x32, 255), true, false};
    static const ProjectStyle kConfirm = {
        IM_COL32(0xFF, 0x44, 0x44, 204), IM_COL32(0xFF, 0x44, 0x44, 255),
        0.5f, 1.5f, IM_COL32(0xFF, 0x66, 0x66, 255), false, true};
    if (s.status == "working") return kWorking;
    if (s.status == "thinking") return kThinking;
    if (s.status == "tool-use") return kToolUse;
    if (s.status == "confirmation-needed") return kConfirm;
    return kIdle;
}

// 项目状态文字（1.x status.*）
const char* StatusText(const SessionInfo& s) {
    if (s.status == "confirmation-needed") return I18n::t("status.confirmationNeeded");
    if (s.status == "thinking") return I18n::t("status.thinking");
    if (s.status == "tool-use") return I18n::t("status.toolUse");
    if (s.status == "working") return I18n::t("status.busy");
    return I18n::t("status.idle");
}

// IDE 徽标（styles.css .project-ide：14x14 圆角 3 徽标片）
struct IdeBadge {
    char letter;
    ImU32 bg;
    ImU32 fg;
};
IdeBadge BadgeFor(const SessionInfo& s) {
    if (s.ide == "qoder") return {'Q', IM_COL32(0xBA, 0x8C, 0xFF, 56), IM_COL32(0xCF, 0xAA, 0xFF, 255)};
    if (s.ide == "cursor") return {'C', IM_COL32(0x50, 0xD2, 0xBE, 56), IM_COL32(0x8F, 0xE8, 0xD8, 255)};
    if (s.ide == "codex") return {'X', IM_COL32(0xFF, 0xAA, 0x50, 56), IM_COL32(0xFF, 0xC8, 0x80, 255)};
    if (s.ide == "opencode") return {'O', IM_COL32(0xFF, 0x78, 0xA0, 56), IM_COL32(0xFF, 0x9F, 0xB5, 255)};
    return {'T', IM_COL32(0x64, 0x96, 0xFF, 56), IM_COL32(0x9D, 0xB9, 0xFF, 255)};
}
} // namespace

// ---------------------------------------------------------------------------
// 底部状态栏（对齐 1.x #status-bar：状态头 + 项目列表；角色画布下方）
// ---------------------------------------------------------------------------
void UIRenderer::renderStatus(const PetStatus& s) {
    if (!impl_->imgui_ready) return;
    auto* p = impl_;
    const float S = p->scale;
    const bool mini = p->mini;
    const double now = ImGui::GetTime();
    p->cur_state = s.overall_state;  // 监控面板边框/光晕取色

    // 状态头（#pet-state-text + ::before 圆点；working/alert 圆点闪烁）
    const char* state_text = I18n::t("state.sleeping");
    ImU32 dot_col = kColSleepDot;
    float dot_period = 0.0f;
    if (s.overall_state == "working") {
        state_text = I18n::t("state.working");
        dot_col = kColWorkDot;
        dot_period = 1.0f;
    } else if (s.overall_state == "alert") {
        state_text = I18n::t("state.alert");
        dot_col = kColAlertDot;
        dot_period = 0.5f;
    }

    // 面板位置：顶部 = 画布底 + 间距（mini 2px，普通 4px）
    const float pad_h = mini ? 8.0f * S : 12.0f * S;    // 头/行左右内边距
    const float pad_v = mini ? 4.0f * S : 8.0f * S;     // 头上下内边距
    // 项目列表/状态头文字乘全局字缩放（此前仅菜单生效，视觉偏小）
    const float font_state = (mini ? 10.0f : 12.0f) * S * kTextScale;
    const float font_name = (mini ? 10.0f : 12.0f) * S * kTextScale;
    const float font_stxt = (mini ? 9.0f : 11.0f) * S * kTextScale;
    // mini 用小一档「同档光栅」字体绘制，避免字形位图缩放模糊
    ImFont* const f_state = mini ? p->font_10 : p->font_12;
    ImFont* const f_name = mini ? p->font_10 : p->font_12;
    ImFont* const f_stxt = mini ? p->font_9 : p->font_11;
    const float dot_d = mini ? 6.0f * S : 8.0f * S;
    const float row_pad_v = mini ? 3.0f * S : 4.0f * S;
    const float row_h = row_pad_v * 2.0f +
                        (mini ? 15.0f * S : 18.0f * S);  // 行高（名称行主导）

    ImGui::SetNextWindowPos(ImVec2(p->panel_x, p->canvas_h + p->panel_gap),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(p->panel_w, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Rgb(30, 30, 40));
    // 状态边框（1.x #status-bar.state-*：sleeping 蓝/working 黄/alert 红）
    ImGui::PushStyleColor(ImGuiCol_Border,
                          StateBorderColor(s.overall_state.c_str()));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, mini ? 8.0f * S : 12.0f * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * S);
    // 窗口圆角半径（行底色/颜色条按它裁剪，对齐 1.x overflow:hidden）
    const float win_round = mini ? 8.0f * S : 12.0f * S;
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar;

    if (ImGui::Begin("##status", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wmin = ImGui::GetWindowPos();
        const float w = ImGui::GetWindowSize().x;
        const float win_bottom = wmin.y + ImGui::GetWindowSize().y;
        // 状态光晕（画在背景层，位于面板之下；1.x box-shadow 等效）
        DrawStateGlow(ImGui::GetBackgroundDrawList(), wmin,
                      ImVec2(wmin.x + w, win_bottom), win_round,
                      s.overall_state.c_str(), now, S);

        // ---- 状态头（#status-header：圆点 + 状态文字 + ☰ 菜单按钮）----
        const float text_h = p->font_12->FontSize;  // ≈ 12*S
        const float header_h = pad_v * 2.0f + (mini ? 13.0f * S : text_h);
        // 头部底色 + 底部 1px 分隔线（rgba(255,255,255,0.03/0.06)）；
        // 顶部圆角与窗口一致（方角底色会戳出窗口圆弧，隐约露出直角）
        dl->AddRectFilled(wmin, ImVec2(wmin.x + w, wmin.y + header_h),
                          IM_COL32(255, 255, 255, 8),
                          win_round, ImDrawFlags_RoundCornersTop);
        dl->AddLine(ImVec2(wmin.x, wmin.y + header_h),
                    ImVec2(wmin.x + w, wmin.y + header_h),
                    IM_COL32(255, 255, 255, 15), 1.0f * S);

        // 圆点（垂直居中）
        const float cy = wmin.y + header_h * 0.5f;
        ImU32 dot = dot_col;
        if (dot_period > 0.0f) {
            dot = (dot_col & ~IM_COL32_A_MASK) |
                  (ImU32)(255.0f * PulseAlpha((float)now, dot_period))
                      << IM_COL32_A_SHIFT;
        }
        dl->AddCircleFilled(ImVec2(wmin.x + pad_h + dot_d * 0.5f, cy),
                            dot_d * 0.5f, dot, 12);

        // 状态文字（#pet-state-text：白 0.9，12px，600）——墨迹居中
        AddTextS(dl,f_state, font_state,
                    ImVec2(wmin.x + pad_h + dot_d + 6.0f * S,
                           TextCenteredY(f_state, font_state, state_text, cy)),
                    IM_COL32(255, 255, 255, 230), state_text);

        // ☰ 菜单按钮（#menu-btn：白 8% 底 + 15% 边框，圆角 5）
        {
            const float glyph = mini ? 11.0f * S : 15.0f * S;
            const float bpad_h = mini ? 6.0f * S : 9.0f * S;
            const float bpad_v = mini ? 2.0f * S : 3.0f * S;
            const float gw = TextW(p->font_12, glyph, "\xE2\x98\xB0");  // ☰
            const ImVec2 bsz(gw + bpad_h * 2.0f, glyph + bpad_v * 2.0f);
            const ImVec2 bmin(wmin.x + w - pad_h - bsz.x, cy - bsz.y * 0.5f);
            ImGui::SetCursorScreenPos(bmin);
            ImGui::PushID("menubtn");
            if (ImGui::InvisibleButton("b", bsz) && !p->menu_open) openMenu();
            const bool bhov = ImGui::IsItemHovered();
            ImGui::PopID();
            dl->AddRectFilled(bmin, ImVec2(bmin.x + bsz.x, bmin.y + bsz.y),
                              IM_COL32(255, 255, 255, bhov ? 46 : 20), 5.0f * S);
            dl->AddRect(bmin, ImVec2(bmin.x + bsz.x, bmin.y + bsz.y),
                        IM_COL32(255, 255, 255, bhov ? 77 : 38), 5.0f * S, 0, 1.0f * S);
            AddTextS(dl,p->font_12, glyph,
                        ImVec2(bmin.x + bpad_h, bmin.y + bpad_v),
                        IM_COL32(255, 255, 255, bhov ? 255 : 217),
                        "\xE2\x98\xB0");
        }

        // ---- 项目列表（#project-list；空态斜体灰字）----
        if (showProjects) {
            const int n = (int)s.sessions.size();
            ImGui::SetCursorScreenPos(ImVec2(wmin.x, wmin.y + header_h));
            if (n == 0) {
                // .project-item.empty：白 0.35 斜体（用字体无法斜体，色调近似）
                const float h = 4.0f * S * 2.0f + 16.0f * S;
                ImGui::BeginChild("##empty", ImVec2(w, h), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar);
                ImVec2 cmin = ImGui::GetCursorScreenPos();
                AddTextS(ImGui::GetWindowDrawList(),
                         mini ? p->font_10 : p->font_12,
                         (mini ? 10.0f : 12.0f) * S * kTextScale,
                         ImVec2(cmin.x + pad_h, cmin.y + 4.0f * S),
                         IM_COL32(255, 255, 255, 89), I18n::t("status.waiting"));
                ImGui::EndChild();
            } else {
                // 列表区高度上限 8 行（1.x overflow-y: auto 滚动）
                const int max_rows = 8;
                const int visible = n < max_rows ? n : max_rows;
                const float list_h = visible * row_h + 4.0f * S;  // padding 4px 0
                ImGui::BeginChild("##projects", ImVec2(w, list_h),
                                  ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoScrollbar);
                ImDrawList* cdl = ImGui::GetWindowDrawList();
                for (int i = 0; i < n; i++) {
                    const SessionInfo& sess = s.sessions[i];
                    const ProjectStyle& st = StyleFor(sess);

                    // 行矩形（child 顶部有 4px 列表 padding 的近似：首行偏移）
                    const float y = 4.0f * S + (float)i * row_h -
                                    ImGui::GetScrollY();
                    if (y + row_h < 0 || y > list_h) continue;
                    const ImVec2 rmin(wmin.x, ImGui::GetCursorScreenPos().y - 0.0f);
                    const ImVec2 rmin_fixed(wmin.x, ImGui::GetWindowPos().y + y);
                    const ImVec2 rmax(rmin_fixed.x + w, rmin_fixed.y + row_h);
                    (void)rmin;
                    // 末行底边贴窗口底：底色/颜色条按窗口圆角裁剪
                    //（1.x 容器 overflow:hidden；否则颜色条戳出圆弧外）
                    const bool last_row = (i == visible - 1);
                    auto row_fill = [&](ImU32 col) {
                        cdl->AddRectFilled(rmin_fixed, rmax, col,
                                           last_row ? win_round : 0.0f,
                                           last_row ? ImDrawFlags_RoundCornersBottom
                                                    : ImDrawFlags_None);
                    };

                    // 整行点击（bringToFront）
                    ImGui::SetCursorScreenPos(rmin_fixed);
                    ImGui::PushID(i);
                    bool clicked = ImGui::InvisibleButton("row", ImVec2(w, row_h));
                    const bool hov = ImGui::IsItemHovered();
                    ImGui::PopID();

                    // 行背景：状态色淡底 + hover 高亮（rgba(255,255,255,0.06)）
                    if (st.glow_period > 0.0f) {
                        // item-glow：确认行红底呼吸 0.05↔0.12
                        const float a = 0.05f + 0.07f * TriWave((float)now, st.glow_period);
                        row_fill(IM_COL32(255, 68, 68, (int)(a * 255.0f)));
                    } else if (sess.status == "working") {
                        row_fill(IM_COL32(255, 200, 50, 13));
                    } else if (sess.status == "thinking") {
                        row_fill(IM_COL32(155, 140, 255, 13));
                    } else if (sess.status == "tool-use") {
                        row_fill(IM_COL32(255, 155, 50, 13));
                    }
                    if (hov) row_fill(IM_COL32(255, 255, 255, 15));

                    // 左边框 3px 状态色（末行下端沿窗口圆弧裁剪）
                    {
                        const float bw = 3.0f * S;
                        const float x0 = rmin_fixed.x, x1 = x0 + bw;
                        const float cy0 = win_bottom - win_round;  // 角区起点
                        if (!last_row || rmax.y <= cy0 ||
                            win_round <= bw) {
                            cdl->AddRectFilled(rmin_fixed, ImVec2(x1, rmax.y),
                                               st.border);
                        } else {
                            // 角区以上全宽
                            cdl->AddRectFilled(rmin_fixed, ImVec2(x1, cy0),
                                               st.border);
                            // 角区：右缘下行到弧线交点，再沿左下圆弧回到左缘
                            //（ImGui 只有矩形裁剪，弧形按 Path 手工构造）
                            const float cx = x0 + win_round;  // 弧心
                            const float dx1 = x1 - cx;        // <0
                            const float arc_y1 = sqrtf(win_round * win_round -
                                                       dx1 * dx1);
                            const float y_end = rmax.y - cy0 < arc_y1
                                                      ? rmax.y - cy0
                                                      : arc_y1;
                            cdl->PathLineTo(ImVec2(x1, cy0));
                            cdl->PathLineTo(ImVec2(x1, cy0 + y_end));
                            float ax = dx1;
                            if (y_end < arc_y1) {
                                ax = -sqrtf(win_round * win_round -
                                            y_end * y_end);
                                cdl->PathLineTo(ImVec2(cx + ax, cy0 + y_end));
                            }
                            cdl->PathArcTo(ImVec2(cx, cy0), win_round,
                                           atan2f(y_end, ax), 3.14159265f);
                            cdl->PathFillConvex(st.border);
                        }
                    }

                    // 内容基线（垂直居中）
                    const float midy = rmin_fixed.y + row_h * 0.5f;
                    float x = rmin_fixed.x + pad_h;

                    // 状态点（.project-dot 8px；闪烁）
                    ImU32 dotc = st.dot;
                    if (sess.status == "idle") dotc = (st.dot & ~IM_COL32_A_MASK) | (128u << IM_COL32_A_SHIFT);
                    else if (st.dot_period > 0.0f)
                        dotc = (st.dot & ~IM_COL32_A_MASK) |
                               (ImU32)(255.0f * PulseAlpha((float)now, st.dot_period)) << IM_COL32_A_SHIFT;
                    const float dd = mini ? 6.0f * S : 8.0f * S;
                    cdl->AddCircleFilled(ImVec2(x + dd * 0.5f, midy), dd * 0.5f, dotc, 12);
                    x += dd + (mini ? 5.0f : 8.0f) * S;

                    // IDE 徽标（.project-ide 14x14 圆角 3，9px 700）
                    if (!sess.ide.empty()) {
                        const IdeBadge b = BadgeFor(sess);
                        const float bs = mini ? 11.0f * S : 14.0f * S;
                        const float bfont = p->font_9->FontSize;
                        cdl->AddRectFilled(ImVec2(x, midy - bs * 0.5f),
                                           ImVec2(x + bs, midy + bs * 0.5f), b.bg,
                                           3.0f * S);
                        const float lw = TextW(p->font_9, bfont,
                                               std::string(1, b.letter).c_str());
                        AddTextS(cdl,p->font_9, bfont,
                                     ImVec2(x + (bs - lw) * 0.5f,
                                            TextCenteredY(p->font_9, bfont,
                                                std::string(1, b.letter).c_str(),
                                                midy)),
                                     b.fg, std::string(1, b.letter).c_str());
                        x += bs + (mini ? 5.0f : 8.0f) * S;
                    }

                    // 状态文字（右侧，先算宽给名称留位）
                    const char* stxt = StatusText(sess);
                    const float stw = TextW(f_stxt, font_stxt, stxt);
                    // 状态文字颜色：idle 绿 #57d97a；working/thinking/tool-use 强调色；
                    // confirm 红 #ff6666 600；其余白 0.45
                    ImU32 stcol = IM_COL32(255, 255, 255, 115);
                    if (sess.status == "idle") stcol = IM_COL32(0x57, 0xD9, 0x7A, 255);
                    else if (sess.status == "confirmation-needed")
                        stcol = IM_COL32(0xFF, 0x66, 0x66, 255);
                    else if (st.name_accent) stcol = st.accent;
                    AddTextS(cdl,f_stxt, font_stxt,
                                 ImVec2(rmax.x - pad_h - stw,
                                        TextCenteredY(f_stxt, font_stxt, stxt, midy)),
                                 stcol, stxt);

                    // 项目名（.project-name 12px 白 0.85；超宽截断省略号）
                    const float name_max = (rmax.x - pad_h - stw - 10.0f * S) - x;
                    char name[96];
                    TruncateUtf8(sess.project_name.c_str(), name_max, name,
                                 sizeof(name), f_name, font_name);
                    ImU32 namecol = IM_COL32(255, 255, 255, 217);
                    if (st.name_red) namecol = IM_COL32(0xFF, 0x66, 0x66, 255);
                    else if (st.name_accent) namecol = st.accent;
                    AddTextS(cdl,f_name, font_name,
                                 ImVec2(x, TextCenteredY(f_name, font_name, name, midy)),
                                 namecol, name);

                    if (clicked && on_project_click) on_project_click(sess);
                }
                ImGui::EndChild();
            }
        }
        p->status_h = ImGui::GetWindowSize().y;
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// ---------------------------------------------------------------------------
// 监控行绘制见 UIRenderer::Impl::MonitorRow（成员版本）
// ---------------------------------------------------------------------------
// 系统监控面板（对齐 1.x #monitor-panel；状态栏下方、窗口底部）
// ---------------------------------------------------------------------------
void UIRenderer::renderMetrics(const SysMetrics& m) {
    if (!impl_->imgui_ready || !showMetrics) {
        impl_->monitor_h = 0.0f;
        return;
    }
    auto* p = impl_;
    const float S = p->scale;
    const bool mini = p->mini;
    const double now = ImGui::GetTime();
    if (mini) {  // 1.x 迷你模式隐藏监控抽屉
        p->monitor_h = 0.0f;
        return;
    }

    // 底边贴窗口底部（1.x 面板列的最后一项）
    ImGui::SetNextWindowPos(ImVec2(p->panel_x, (float)p->win_h),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(p->panel_w, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Rgb(30, 30, 40));
    // 状态边框（1.x #monitor-panel.state-*：sleeping 蓝/working 黄/alert 红）
    ImGui::PushStyleColor(ImGuiCol_Border,
                          StateBorderColor(p->cur_state.c_str()));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * S);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoInputs;

    if (ImGui::Begin("##monitor", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 wmin = ImGui::GetWindowPos();
        const float w = ImGui::GetWindowSize().x;
        const float wh = ImGui::GetWindowSize().y;
        // 状态光晕（画在背景层，位于面板之下；1.x box-shadow 等效）
        DrawStateGlow(ImGui::GetBackgroundDrawList(), wmin,
                      ImVec2(wmin.x + w, wmin.y + wh), 12.0f * S,
                      p->cur_state.c_str(), now, S);

        // ---- 标题行（#monitor-header：标题 + ↺ 恢复 + ▾ 收起）----
        // 标题字号大于行标签（12px vs 10px），行高相应加大
        const float title_font = 12.0f * S * kTextScale;
        const float header_h = 2.0f * S * 2.0f + 16.0f * S;
        // 顶部圆角与窗口一致（方角底色会戳出窗口圆弧）
        dl->AddRectFilled(wmin, ImVec2(wmin.x + w, wmin.y + header_h),
                          IM_COL32(255, 255, 255, 8),
                          12.0f * S, ImDrawFlags_RoundCornersTop);
        dl->AddLine(ImVec2(wmin.x, wmin.y + header_h),
                    ImVec2(wmin.x + w, wmin.y + header_h),
                    IM_COL32(255, 255, 255, 15), 1.0f);
        const float midy = wmin.y + header_h * 0.5f;
        AddTextS(dl,p->font_12, title_font,
                    ImVec2(wmin.x + 8.0f * S,
                           TextCenteredY(p->font_12, title_font,
                                         I18n::t("monitor.title"), midy)),
                    IM_COL32(255, 255, 255, 230), I18n::t("monitor.title"));

        // 行区从标题行下方开始（标题是直接画在 drawlist 上的，不占布局流；
        // 按钮删除后没有 InvisibleButton 推进光标，需显式定位）。
        // 首行上留 4px：标题与 CPU 行之间的呼吸空间（用户反馈过近），
        // 行内已收紧，标题区相对更透气
        ImGui::SetCursorScreenPos(ImVec2(wmin.x, wmin.y + header_h + 4.0f * S));

        if (!monitorCollapsed) {
            // 先收集要显示的行，再统一绘制（行间无分隔线，见 MonitorRow）
            struct MonRow {
                const char* label;
                ImU32 lcol;
                char value[48];
                RingHistory* hist;
                float fmax;
                ImU32 scol;
            };
            MonRow rows[6];
            int n_rows = 0;
            if (showCpu) {
                MonRow& r = rows[n_rows++];
                r.label = I18n::t("monitor.cpu");
                r.lcol = IM_COL32(0x4F, 0xC3, 0xF7, 255);
                snprintf(r.value, sizeof(r.value), "%d%%", (int)(m.cpu_usage + 0.5f));
                r.hist = &p->cpu;
                r.fmax = 100.0f;
                r.scol = Rgb32(0x4F, 0xC3, 0xF7);
            }
            if (showRam) {
                MonRow& r = rows[n_rows++];
                r.label = I18n::t("monitor.ram");
                r.lcol = IM_COL32(0x81, 0xC7, 0x84, 255);
                char used[32], total[32];
                FormatBytesCompact(m.mem_used, used, sizeof(used));
                FormatBytesCompact(m.mem_total, total, sizeof(total));
                snprintf(r.value, sizeof(r.value), "%s/%s", used, total);
                r.hist = &p->ram;
                r.fmax = 0.0f;
                r.scol = Rgb32(0x81, 0xC7, 0x84);
            }
            if (showGpu) {
                MonRow& r = rows[n_rows++];
                r.label = I18n::t("monitor.gpu");
                r.lcol = IM_COL32(0xBA, 0x68, 0xC8, 255);
                if (m.has_gpu) {
                    snprintf(r.value, sizeof(r.value), "%d%%",
                             (int)(m.gpu_usage + 0.5f));
                    r.hist = &p->gpu;
                    r.fmax = 100.0f;
                    r.scol = Rgb32(0xBA, 0x68, 0xC8);
                } else {
                    snprintf(r.value, sizeof(r.value), "\xE2\x80\x94");  // —
                    r.hist = nullptr;
                    r.fmax = 0.0f;
                    r.scol = 0;
                }
                // 显存行（GPU 占用与显存分两项显示；无 N 卡时同样占位 —）
                MonRow& v = rows[n_rows++];
                v.label = I18n::t("monitor.vram");
                v.lcol = IM_COL32(0xCE, 0x93, 0xD8, 255);
                if (m.has_gpu && m.vram_total) {
                    char used[32], total[32];
                    FormatBytesCompact(m.vram_used, used, sizeof(used));
                    FormatBytesCompact(m.vram_total, total, sizeof(total));
                    snprintf(v.value, sizeof(v.value), "%s/%s", used, total);
                    v.hist = &p->vram;
                    v.fmax = 0.0f;  // 自适应：满载时曲线顶到行高
                    v.scol = Rgb32(0xCE, 0x93, 0xD8);
                } else {
                    snprintf(v.value, sizeof(v.value), "\xE2\x80\x94");  // —
                    v.hist = nullptr;
                    v.fmax = 0.0f;
                    v.scol = 0;
                }
            }
            if (showNet) {
                MonRow& r = rows[n_rows++];
                r.label = I18n::t("monitor.net");
                r.lcol = IM_COL32(0x4D, 0xD0, 0xE1, 255);
                char rx[24], tx[24];
                FormatRateCompact(m.net_rx_rate, rx, sizeof(rx));
                FormatRateCompact(m.net_tx_rate, tx, sizeof(tx));
                snprintf(r.value, sizeof(r.value), "\xE2\x86\x93%s \xE2\x86\x91%s",
                         rx, tx);  // ↓ ↑
                r.hist = &p->net;
                r.fmax = 0.0f;
                r.scol = Rgb32(0x4D, 0xD0, 0xE1);
            }
            if (showSelf) {
                MonRow& r = rows[n_rows++];
                r.label = I18n::t("monitor.self");
                r.lcol = IM_COL32(0xFF, 0xB7, 0x4D, 255);
                char mem[24];
                FormatBytesCompact(m.self_mem, mem, sizeof(mem));
                snprintf(r.value, sizeof(r.value), "%.1f%% \xC2\xB7 %s", m.self_cpu,
                         mem);  // ·
                r.hist = nullptr;
                r.fmax = 0.0f;
                r.scol = 0;
            }
            for (int i = 0; i < n_rows; i++)
                p->MonitorRow(rows[i].label, rows[i].lcol, rows[i].value,
                              rows[i].hist, rows[i].fmax, rows[i].scol);
            // 末行（自身）下留 2px 呼吸空间（Dummy 在手动光标处原位放置，
            // 无额外间距）：文字不顶贴面板底边圆角，同时撑起内容高度
            ImGui::Dummy(ImVec2(w, 2.0f * S));
        }
        p->monitor_h = ImGui::GetWindowSize().y;
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// ---------------------------------------------------------------------------
// 角色头顶状态特效（对齐 1.x #head-effect，锚定模型包围盒顶中，动画）
// ---------------------------------------------------------------------------
void UIRenderer::renderHeadEffect(const PetStatus& s) {
    if (!impl_->imgui_ready) return;
    auto* p = impl_;
    const float S = p->scale;
    const double now = ImGui::GetTime();

    // 预览提醒优先（动作设定 → 预览提醒效果）
    const bool previewing = now < p->alert_preview_until;
    std::string state = s.overall_state;
    if (previewing) state = "alert";
    if (state != "sleeping" && state != "working" && state != "alert") return;

    // 锚点：模型 bounds 顶中下移（1.x updateHeadEffectAnchor：
    // Q 版 aspect>=0.8 下移 20%，长身形 aspect<0.8 下移 4%）
    float top_x, top_y, bounds_h;
    if (p->has_model_rect) {
        const Rect& b = p->model_rect;
        top_x = b.x + b.w * 0.5f;
        const float aspect = b.w / (b.h > 0 ? b.h : 1.0f);
        float inset;
        if (!p->model_bounds_tight) {
            // GIF：包围盒是整张图（含透明留白），头顶≈盒顶。锚点只轻微
            // 下探 2%，点点点/ZZZ 悬在头顶上方一点（用户反馈 20% 下探
            // 会贴到头/脸上）
            inset = b.h * 0.02f;
            if (inset < 2.0f * S) inset = 2.0f * S;
            if (inset > 10.0f * S) inset = 10.0f * S;
        } else if (aspect < 0.8f) {
            inset = b.h * 0.04f;
            if (inset < 4.0f * S) inset = 4.0f * S;
            if (inset > 20.0f * S) inset = 20.0f * S;
        } else {
            inset = b.h * 0.2f;
            if (inset < 16.0f * S) inset = 16.0f * S;
            if (inset > 48.0f * S) inset = 48.0f * S;
        }
        top_y = b.y + inset;
        bounds_h = b.h;
    } else {
        top_x = p->win_w * 0.5f;
        top_y = 40.0f * S;
        bounds_h = 160.0f * S;
    }

    // 缩放 k：参考高度 190 CSS px（对齐 1.x），平滑过渡防抖动
    const float target_k =
        (std::min)((std::max)(bounds_h / (190.0f * S), 0.55f), 1.8f);
    p->effect_k += (target_k - p->effect_k) * 0.15f;
    const float k = p->effect_k;

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (!p->fx_built) p->BuildFxSprites();

    // 整体放大系数：1.x 是 96 DPI 下的 CSS px；等比复刻在 2.0 高分屏
    // 上观感偏小（用户反馈），统一放大 1.35x（字号/光晕/上浮距离同比）
    const float B = 1.35f;

    if (state == "sleeping") {
        // ZZZ：三个 Z 上浮放大淡出（zzz-rise 2.4s，延迟 0/0.8/1.6s）。
        // 精灵 = 真实 Segoe UI Bold 字形 + 真高斯光晕 + 白偏移影（烘焙）
        static const float kDelays[3] = {0.0f, 0.8f, 1.6f};
        for (int i = 0; i < 3; i++) {
            float t = (float)fmod(now + kDelays[i], 2.4) / 2.4f;
            // ease-in-out（1.x zzz-rise cubic-bezier(0.42,0,0.58,1) ≈
            // smoothstep）：起步缓-中段快-收尾缓的漂浮感
            const float te = t * t * (3.0f - 2.0f * t);
            float alpha = te < 0.25f ? te / 0.25f : 1.0f - (te - 0.25f) / 0.75f;
            if (alpha <= 0.0f) continue;
            const float y = top_y - B * 40.0f * k * S * te;
            const float sc = B * k * S * (0.7f + 0.45f * te);
            p->DrawFx(dl, p->fx_z[i], top_x, y, sc, alpha);
        }
    } else if (state == "working") {
        // 工作点：三个黄点自锚点上浮（working-dot 1.5s，延迟 0/0.5/1s）
        static const float kDelays[3] = {0.0f, 0.5f, 1.0f};
        for (int i = 0; i < 3; i++) {
            float t = (float)fmod(now + kDelays[i], 1.5) / 1.5f;
            // ease-out（1.x working-dot cubic-bezier(0,0,0.58,1) ≈
            // 1-(1-t)³）：快出慢收，点子弹出后缓缓飘散
            const float te = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
            float alpha = te < 0.3f ? te / 0.3f : 1.0f - (te - 0.3f) / 0.7f;
            if (alpha <= 0.0f) continue;
            const float y = top_y - B * 36.0f * k * S * te;
            const float sc = B * k * S * (0.4f + 0.6f * te);
            p->DrawFx(dl, p->fx_dot, top_x, y, sc, alpha);
        }
    } else {
        // 叹号弹跳（head-alert-bounce 0.5s alternate ease-in-out）：
        // alternate ease-in-out 往返 = 正弦摆动（两端速度为 0，中段最快）
        const float phase = (float)fmod(now, 1.0);
        const float tri = 0.5f - 0.5f * cosf(6.28318530718f * phase);
        const float y = top_y - B * 8.0f * k * S * tri;
        const float sc = B * k * S * (1.0f + 0.15f * tri);
        p->DrawFx(dl, p->fx_bang, top_x, y, sc, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// ImGui 自绘右键菜单（对齐 1.x #context-menu：视图切换 + 返回）
// 条目绘制见 UIRenderer::Impl::MenuRow / MenuLabel / MenuDivider / MotionCell
// ---------------------------------------------------------------------------

void UIRenderer::renderMenu() {
    if (!impl_->imgui_ready || !impl_->menu_open) return;
    auto* p = impl_;
    const float S = p->scale;
    const bool mini = p->mini;

    // ---- 菜单几何（1.x #context-menu：min-width 210 + padding 4x2）----
    // 向右展开：贴角色区右侧（base_w + 8）；向左展开（menu-left）：贴窗口左缘
    // 切换形象视图加宽到 280（1.x #menu-model-view min-width: 280，
    // 两列缩略图卡片需要空间）
    const bool models_view = p->menu_view == kMenuModels;
    const float menu_w = (models_view ? 280.0f : 218.0f) * S;
    const float base_w = mini ? 130.0f * S : 260.0f * S;  // 常规窗口宽
    const float menu_x = p->menu_left ? 8.0f * S : base_w + 8.0f * S;

    // ---- 菜单关闭条件：窗口内菜单矩形外点击（1.x 点击空白处关菜单）----
    if (ImGui::IsMouseClicked(0)) {
        const ImVec2 m = ImGui::GetIO().MouseClickedPos[0];
        if (m.x < menu_x || m.x > menu_x + menu_w || m.y < 4.0f * S ||
            m.y > (float)p->win_h - 4.0f * S) {
            closeMenu();
            return;
        }
    }

    ImGui::SetNextWindowPos(ImVec2(menu_x, 4.0f * S), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(menu_w, (float)p->win_h - 8.0f * S),
                             ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.95f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Rgb(30, 30, 40));
    ImGui::PushStyleColor(ImGuiCol_Border, Rgb(255, 255, 255, 26));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * S, 4.0f * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * S);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove;

    auto checked = [this](const std::string& id) {
        return menu_is_checked && menu_is_checked(id);
    };
    auto hint_of = [this](const std::string& id) -> std::string {
        return menu_hint ? menu_hint(id) : std::string();
    };
    auto activate = [this](const std::string& id) {
        if (menu_activate) menu_activate(id);
    };
    auto go = [&](int view) {
        p->menu_view = view;
        p->hover_motion = -1;
    };

    if (ImGui::Begin("##ctxmenu", nullptr, flags)) {
        switch (p->menu_view) {
        case kMenuMain: {
            // ==== 主菜单（对齐 1.x #menu-main-view）====
            if (p->MenuRow("models", I18n::t("menu.switchModel"), false, false,
                        nullptr, true).clicked)
                go(kMenuModels);
            if (p->MenuRow("upload", I18n::t("menu.uploadLive2D"), false, false,
                        nullptr, false).clicked) {
                activate("open-models-dir");
                closeMenu();
            }
            if (p->MenuRow("play", I18n::t("menu.playMotion"), false, false,
                        nullptr, true).clicked)
                go(kMenuMotionPlay);
            if (p->MenuRow("settings", I18n::t("menu.actionSettings"), false, false,
                        nullptr, true).clicked)
                go(kMenuSettings);
            if (p->MenuRow("flip", I18n::t("menu.flipHorizontal"), checked("flip"),
                        false, nullptr, false).clicked)
                activate("flip");
            if (p->MenuRow("mini", I18n::t("menu.miniMode"), checked("mini"),
                        false, nullptr, false).clicked)
                activate("mini");
            if (p->MenuRow("vis", I18n::t("menu.visibility"), false, false,
                        nullptr, true).clicked)
                go(kMenuVisibility);
            if (p->MenuRow("autostart", I18n::t("menu.autoLaunch"),
                        checked("autostart"), false, nullptr, false).clicked)
                activate("autostart");
            if (p->MenuRow("lang", I18n::t("menu.language"), false, false,
                        nullptr, true).clicked)
                go(kMenuLanguage);
            p->MenuDivider();
            if (p->MenuRow("install", I18n::t("menu.installHooks"), false, false,
                        nullptr, false).clicked) {
                activate("install-hooks");
                closeMenu();
            }
            if (p->MenuRow("hook", I18n::t("menu.hookStatus"), false, false,
                        hint_of("hook-status").c_str(), false).clicked) {
                activate("hook-status");
                closeMenu();
            }
            p->MenuDivider();
            if (p->MenuRow("quit", I18n::t("menu.quit"), false, true, nullptr,
                        false).clicked) {
                activate("quit");
                closeMenu();
            }
            break;
        }
        case kMenuModels: {
            // ==== 切换形象（1.x #menu-model-view：两列缩略图卡片网格）====
            if (p->MenuRow("back", I18n::t("menu.back"), false, false, nullptr, false).clicked)
                go(kMenuMain);
            p->MenuLabel(I18n::t("menu.switchModel"));
            p->MenuDivider();
            std::vector<MenuEntry> models =
                menu_collect ? menu_collect("models") : std::vector<MenuEntry>();
            if (models.empty()) {
                p->MenuLabel("(no models)");
                break;
            }
            // #character-grid：2 列，gap 6，padding 6px 4px；超出滚动
            {
                const float content_w = ImGui::GetWindowSize().x - 8.0f * S;
                const float grid_pad_h = 4.0f * S;
                const float gap = 6.0f * S;
                const float card_w = (content_w - grid_pad_h * 2.0f - gap) * 0.5f;
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                    ImVec2(gap, gap));
                ImGui::BeginChild("##chargrid", ImVec2(content_w, 0.0f),
                                  ImGuiChildFlags_None);
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + grid_pad_h);
                ImGui::Dummy(ImVec2(content_w - grid_pad_h * 2.0f, 6.0f * S));
                for (size_t i = 0; i < models.size(); i++) {
                    // 左列：换行后行首 x 是子区内容起点，补上网格左边距；
                    // 右列：与左卡同行，间隔 gap
                    if (i % 2 == 1)
                        ImGui::SameLine(0.0f, gap);
                    else
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + grid_pad_h);
                    if (p->CharCard(("cc" + std::to_string(i)).c_str(),
                                    models[i], card_w)) {
                        activate(models[i].id);
                        closeMenu();
                    }
                }
                ImGui::EndChild();
                ImGui::PopStyleVar(1);
            }
            break;
        }
        case kMenuMotionPlay:
        case kMenuMotionAssign: {
            // ==== 播放动作 / 动作设定选动作（1.x #menu-motion-view：3 列网格）====
            const bool assign = p->menu_view == kMenuMotionAssign;
            if (p->MenuRow("back", I18n::t("menu.back"), false, false, nullptr, false).clicked)
                go(assign ? kMenuSettings : kMenuMain);
            // 标题：播放 =「播放动作」；设定 =「选择动作」（1.x openMotionView）
            p->MenuLabel(assign ? I18n::t("menu.selectMotion")
                                : I18n::t("menu.playMotion"));

            std::string key = assign ? "motions:" + p->assign_state : "motions";
            std::vector<MenuEntry> motions =
                menu_collect ? menu_collect(key) : std::vector<MenuEntry>();
            if (motions.empty()) {
                p->MenuLabel("(no motions)");
                break;
            }
            // #motion-list：grid 3 列 / gap 2px / padding 2px 0；项居中 12px。
            // 超出滚动（返回行钉在顶部，同 1.x submenu-back sticky）
            ImGui::BeginChild("##mlist", ImVec2(0, 0), ImGuiChildFlags_None);
            const float gap = 2.0f * S;
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
            ImGui::Dummy(ImVec2(0, 2.0f * S));
            for (size_t i = 0; i < motions.size(); i++) {
                if (i % 3 != 0) ImGui::SameLine(0.0f, gap);
                auto r = p->MotionCell(("mo" + std::to_string(i)).c_str(),
                                        motions[i].label.c_str(),
                                        motions[i].checked);
                // 悬停预览动作（1.x mouseenter → previewMotion）
                if (r.hovered && p->hover_motion != (int)i) {
                    p->hover_motion = (int)i;
                    activate("preview:" + motions[i].id);
                }
                if (r.clicked) {
                    if (assign) {
                        activate("assign:" + p->assign_state + ":" + motions[i].id);
                        go(kMenuSettings);
                    } else {
                        activate("motion:" + motions[i].id);
                        closeMenu();
                    }
                }
            }
            ImGui::PopStyleVar(1);
            ImGui::EndChild();
            break;
        }
        case kMenuSettings: {
            // ==== 动作设定（1.x #menu-settings-view：状态行=状态名+当前动作名+▶）====
            if (p->MenuRow("back", I18n::t("menu.back"), false, false, nullptr, false).clicked)
                go(kMenuMain);
            p->MenuLabel(I18n::t("menu.actionSettings"));
            static const char* kStates[3] = {"sleeping", "working", "alert"};
            for (int i = 0; i < 3; i++) {
                if (p->SettingsRow(kStates[i],
                            I18n::t((std::string("settings.") + kStates[i]).c_str()),
                            hint_of(std::string("assign:") + kStates[i]).c_str()).clicked) {
                    p->assign_state = kStates[i];
                    go(kMenuMotionAssign);
                }
            }
            p->MenuDivider();
            if (p->MenuRow("test", I18n::t("menu.previewAlert"), false, false,
                        nullptr, false).clicked) {
                activate("preview-alert");
            }
            break;
        }
        case kMenuLanguage: {
            // ==== 语言（1.x #menu-language-view，8 语言本地名显示）====
            if (p->MenuRow("back", I18n::t("menu.back"), false, false, nullptr, false).clicked)
                go(kMenuMain);
            p->MenuLabel(I18n::t("menu.language"));
            for (const auto& [code, name] : I18n::languages()) {
                if (p->MenuRow(code.c_str(), name.c_str(),
                            checked("lang:" + code), false, nullptr, false).clicked)
                    activate("lang:" + code);
            }
            break;
        }
        case kMenuVisibility: {
            // ==== 显示隐藏（1.x #menu-visibility-view：分组分隔线）====
            if (p->MenuRow("back", I18n::t("menu.back"), false, false, nullptr, false).clicked)
                go(kMenuMain);
            p->MenuLabel(I18n::t("menu.visibility"));
            if (p->MenuRow("v0", I18n::t("menu.systemMonitor"), checked("vis-monitor"),
                        false, nullptr, false).clicked)
                activate("vis-monitor");
            p->MenuDivider();
            static const char* kVisKeys[5] = {"vis-cpu", "vis-ram", "vis-gpu",
                                              "vis-net", "vis-self"};
            static const char* kVisLabels[5] = {"monitor.cpu", "monitor.ram",
                                                "monitor.gpu", "monitor.net",
                                                "monitor.self"};
            for (int i = 0; i < 5; i++) {
                if (p->MenuRow(kVisKeys[i], I18n::t(kVisLabels[i]),
                            checked(kVisKeys[i]), false, nullptr, false).clicked)
                    activate(kVisKeys[i]);
            }
            p->MenuDivider();
            if (p->MenuRow("v6", I18n::t("monitor.projectList"),
                        checked("vis-projects"), false, nullptr, false).clicked)
                activate("vis-projects");
            break;
        }
        default:
            go(kMenuMain);
            break;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// ---------------------------------------------------------------------------
// 边缘吸附条（对齐 1.x #edge-dock-bar：整窗细条；顶部状态灯 + 项目徽标）
// ---------------------------------------------------------------------------
void UIRenderer::renderDockBar(const PetStatus& s) {
    if (!impl_->imgui_ready) return;
    auto* p = impl_;
    const float S = p->scale;
    const double now = ImGui::GetTime();

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2((float)p->win_w, (float)p->win_h),
                             ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Rgb(30, 30, 40));
    ImGui::PushStyleColor(ImGuiCol_Border, Rgb(255, 255, 255, 20));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f * S);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar;

    if (ImGui::Begin("##dockbar", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float w = (float)p->win_w;

        // ---- 顶部状态灯（最紧急状态：红 confirmation > 黄 working > 蓝 idle；
        //      发光 + working/alert 闪烁，同 1.x updateEdgeDockIndicator）----
        int level = 0;  // 0=idle 1=working 2=alert
        for (const auto& sess : s.sessions) {
            if (sess.status == "confirmation-needed") {
                level = 2;
                break;
            }
            if (sess.status == "working" || sess.status == "thinking" ||
                sess.status == "tool-use")
                level = level < 1 ? 1 : level;
        }
        ImU32 lamp = kColSleepDot;
        float lamp_period = 0.0f;
        if (level == 1) {
            lamp = kColWorkDot;
            lamp_period = 1.0f;
        } else if (level == 2) {
            lamp = kColAlertDot;
            lamp_period = 0.5f;
        }
        const float lamp_d = 12.0f * S;
        const ImVec2 lamp_c(w * 0.5f, 8.0f * S + lamp_d * 0.5f);
        // box-shadow 发光近似：低 alpha 大圆垫底
        dl->AddCircleFilled(lamp_c, lamp_d * 0.9f,
                            (lamp & ~IM_COL32_A_MASK) | (72u << IM_COL32_A_SHIFT),
                            16);
        ImU32 lamp_col = lamp;
        if (lamp_period > 0.0f)
            lamp_col = (lamp & ~IM_COL32_A_MASK) |
                       (ImU32)(255.0f * PulseAlpha((float)now, lamp_period))
                           << IM_COL32_A_SHIFT;
        dl->AddCircleFilled(lamp_c, lamp_d * 0.5f, lamp_col, 12);

        // ---- 项目两字徽标（.edge-dock-badge：11px 粗体圆角 4，状态色；
        //      点击 bringToFront；空态 "··" 灰）----
        bool badge_hovered = false;
        float y = 8.0f * S + 22.0f * S + 8.0f * S;  // PAD + 灯 + GAP
        if (s.sessions.empty()) {
            const char* txt = "\xC2\xB7\xC2\xB7";  // ··
            const float fs = p->font_11->FontSize;
            const float tw = TextW(p->font_11, fs, txt);
            const float bh = 22.0f * S;
            dl->AddRectFilled(ImVec2((w - tw - 6.0f * S) * 0.5f, y),
                              ImVec2((w + tw + 6.0f * S) * 0.5f, y + bh),
                              IM_COL32(255, 255, 255, 15), 4.0f * S);
            AddTextS(dl,p->font_11, fs,
                        ImVec2((w - tw) * 0.5f,
                               TextCenteredY(p->font_11, fs, txt, y + bh * 0.5f)),
                        IM_COL32(255, 255, 255, 89), txt);
        } else {
            for (const auto& sess : s.sessions) {
                const std::string initials = ProjectInitials(sess.project_name);
                const float fs = p->font_11->FontSize;
                const float tw = TextW(p->font_11, fs, initials.c_str());
                const float bh = 22.0f * S;
                const float bw = tw + 6.0f * S;
                const ImVec2 bmin((w - bw) * 0.5f, y);
                const ImVec2 bmax(bmin.x + bw, y + bh);

                // 徽标色（.status-*：idle 蓝 / working 黄 / thinking 紫 /
                // tool-use 橙 / confirm 红，闪烁 + 红底）
                ImU32 txt_col = IM_COL32(0x64, 0x96, 0xFF, 255);
                float period = 0.0f;
                ImU32 bg = IM_COL32(255, 255, 255, 15);
                if (sess.status == "confirmation-needed") {
                    txt_col = IM_COL32(0xFF, 0x44, 0x44, 255);
                    period = 0.5f;
                    bg = IM_COL32(255, 68, 68, 31);
                } else if (sess.status == "working") {
                    txt_col = IM_COL32(0xFF, 0xC8, 0x32, 255);
                    period = 1.0f;
                } else if (sess.status == "thinking") {
                    txt_col = IM_COL32(0x9B, 0x8C, 0xFF, 255);
                    period = 1.0f;
                } else if (sess.status == "tool-use") {
                    txt_col = IM_COL32(0xFF, 0x9B, 0x32, 255);
                    period = 0.7f;
                }
                if (period > 0.0f)
                    txt_col = (txt_col & ~IM_COL32_A_MASK) |
                              (ImU32)(255.0f * PulseAlpha((float)now, period))
                                  << IM_COL32_A_SHIFT;

                // 点击 → bringToFront（拖拽条时 ImGui 自身会取消点击）
                ImGui::SetCursorScreenPos(bmin);
                ImGui::PushID((int)(&sess - &s.sessions[0]));
                const bool clicked = ImGui::InvisibleButton(
                    "badge", ImVec2(bmax.x - bmin.x, bmax.y - bmin.y));
                const bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                if (hov) badge_hovered = true;

                dl->AddRectFilled(bmin, bmax,
                                  hov ? IM_COL32(255, 255, 255, 36) : bg, 4.0f * S);
                AddTextS(dl,p->font_11, fs,
                            ImVec2(bmin.x + 3.0f * S,
                                   TextCenteredY(p->font_11, fs,
                                                 initials.c_str(),
                                                 y + bh * 0.5f)),
                            txt_col, initials.c_str());
                if (clicked && on_project_click) on_project_click(sess);
                y += bh + 6.0f * S;
            }
        }

        // ---- 双击空白处退出吸附（1.x dblclick → leaveEdgeDock）----
        if (ImGui::IsMouseDoubleClicked(0) && !badge_hovered && on_undock)
            on_undock();
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// 吸附条内容高度（物理像素；对齐 1.x computeDockBarHeight）
float UIRenderer::dockBarHeight(const PetStatus& s) const {
    const float S = impl_->scale;
    const int n = (int)s.sessions.size();
    const int cnt = n < 1 ? 1 : n;
    return (8.0f * 2.0f + 22.0f + 8.0f + cnt * 22.0f + (cnt - 1) * 6.0f) * S;
}

void UIRenderer::endFrame() {
    if (impl_->imgui_ready) {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        // 语言切换后的字体重建：本帧 DrawData 已提交（引用旧图集），此时
        // 重建安全；下一帧起新字形生效
        if (impl_->fonts_dirty) {
            impl_->fonts_dirty = false;
            ImGui_ImplOpenGL3_DestroyFontsTexture();
            ImGui::GetIO().Fonts->Clear();
            impl_->BuildFonts();
            ImGui_ImplOpenGL3_CreateFontsTexture();
        }
        // 图集光栅化已完成（首帧 NewFrame 的 CreateFontsTexture 或上方重建
        // 路径），共享字体文件缓冲使命结束 —— 释放（构建峰值 ~32MB → 常驻 0）
        impl_->font_file_bufs.clear();
        // ImGui AddFont 对外部 FontData 强制 memcpy 一份接管（imgui_draw.cpp
        // ImFontAtlas::AddFont：FontDataOwnedByAtlas=false 时 IM_ALLOC+memcpy），
        // 4 档 × 3 字体 ≈ 165MB 常驻。图集构建后 FontData 不再被使用（仅
        // Clear 时 free），此处主动释放防堆积；置 false 防 double-free。
        // 幂等：释放后 FontData=NULL 直接跳过；fonts_dirty 重建走 Clear()
        // （FontData 已空，安全）→ BuildFonts 重新 AddFont。
        for (ImFontConfig& cfg : ImGui::GetIO().Fonts->ConfigData) {
            if (cfg.FontDataOwnedByAtlas && cfg.FontData) {
                ImGui::MemFree(cfg.FontData);
                cfg.FontData = NULL;
                cfg.FontDataOwnedByAtlas = false;
            }
        }
        // 图集 CPU 像素副本（TexPixelsRGBA32 ~16MB）：纹理已上传 GL，此后
        // 仅 fonts_dirty 重建用到 —— 重建走 Clear()（重置指针）→ Build
        // （重新生成）路径，不会读到已释放数据；GetTexDataAsRGBA32 只在
        // 后端 CreateFontsTexture（纹理重建）时调用，同样安全
        if (ImGui::GetIO().Fonts->TexPixelsRGBA32) {
            ImGui::MemFree(ImGui::GetIO().Fonts->TexPixelsRGBA32);
            ImGui::GetIO().Fonts->TexPixelsRGBA32 = NULL;
        }
    }
}

void UIRenderer::shutdown() {
    if (impl_ && impl_->imgui_ready) {
        // 缩略图纹理（GL 对象不归 ImGui 后端管，需手动释放）
        for (auto& [path, t] : impl_->thumb_cache) {
            if (t.tex) glDeleteTextures(1, &t.tex);
        }
        impl_->thumb_cache.clear();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        impl_->imgui_ready = false;
    }
}

#else  // 非 Windows 设备端占位

struct UIRenderer::Impl {};

UIRenderer::UIRenderer() : impl_(new Impl()) {}
UIRenderer::~UIRenderer() { delete impl_; }
bool UIRenderer::init(GLFWwindow*) { return true; }
bool UIRenderer::init(int, int) { return true; }
void UIRenderer::beginFrame() {}
void UIRenderer::forwardMouseButton(int, int, int) {}
void UIRenderer::forwardCursorPos(double, double) {}
void UIRenderer::forwardScroll(double, double) {}
void UIRenderer::forwardKey(int, int, int, int) {}
void UIRenderer::pushMetrics(const SysMetrics&) {}
void UIRenderer::renderMetrics(const SysMetrics&) {}
void UIRenderer::renderStatus(const PetStatus&) {}
void UIRenderer::renderHeadEffect(const PetStatus&) {}
void UIRenderer::renderMenu() {}
void UIRenderer::renderDockBar(const PetStatus&) {}
float UIRenderer::dockBarHeight(const PetStatus&) const { return 0.0f; }
void UIRenderer::endFrame() {}
void UIRenderer::shutdown() {}
float UIRenderer::statusBarHeight() const { return 0.0f; }
float UIRenderer::monitorHeight() const { return 0.0f; }
float UIRenderer::menuExtraWidth() const { return 0.0f; }
float UIRenderer::dpiScale() const { return 1.0f; }
void UIRenderer::setModelRect(const Rect&, bool) {}
void UIRenderer::setLayout(float, float, float, float, bool) {}
void UIRenderer::openMenu() {}
void UIRenderer::openMenuView(const std::string&) {}
void UIRenderer::closeMenu() {}
bool UIRenderer::isMenuOpen() const { return false; }
bool UIRenderer::isMotionViewActive() const { return false; }
void UIRenderer::setMenuLeft(bool) {}
bool UIRenderer::isPointInMenu(float, float) const { return false; }
bool UIRenderer::isPointClickable(float, float) const { return false; }
std::vector<Rect> UIRenderer::clickRegions() const { return {}; }
void UIRenderer::reloadFonts() {}
void UIRenderer::setScale(float) {}
void UIRenderer::previewAlert() {}

#endif

} // namespace dutyon
