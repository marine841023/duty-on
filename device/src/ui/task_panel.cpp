#ifndef _WIN32  // 仅设备端（ARM Linux GLES）

#include "ui/task_panel.h"

#include <GLES3/gl3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "render/text_renderer.h"
#include "ui/i18n.h"

namespace dutyon {

namespace {

// 纯色填充（GLES2 语法）：面板底色 / 状态圆点共用
const char* kVertSrc =
    "attribute vec2 a_pos;\n"
    "uniform vec2 u_screen;\n"
    "void main() {\n"
    "  vec2 ndc = a_pos / u_screen * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "}\n";

const char* kFragSrc =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main() { gl_FragColor = u_color; }\n";

GLuint compileShader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "[TaskPanel] shader compile: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// 状态行样式（对齐 PC 端 ui_renderer StyleFor：圆点/状态文字/名称强调色/
// 行背景/左边框；working 等活动状态整行淡色底 + 文字强调色）
struct RowStyle {
    float dot[3];       // 圆点颜色
    float txt[3];       // 状态文字颜色
    const char* i18n_key;
    float name[3];      // 项目名颜色（活动状态 = 强调色）
    float bg[4];        // 行背景 RGBA（a=0 无背景）
    float border[3];    // 左边框颜色
    bool breathing;     // confirm：红底呼吸（0.05↔0.12，周期 1.5s）
};
const RowStyle& styleFor(const std::string& status) {
    static const RowStyle kIdle      = {{0x64 / 255.f, 0x96 / 255.f, 0xFF / 255.f},
                                        {0x57 / 255.f, 0xD9 / 255.f, 0x7A / 255.f},
                                        "status.idle",
                                        {0.93f, 0.94f, 0.96f},
                                        {0, 0, 0, 0},
                                        {0x64 / 255.f, 0x96 / 255.f, 0xFF / 255.f},
                                        false};
    static const RowStyle kWorking   = {{0xFF / 255.f, 0xC8 / 255.f, 0x32 / 255.f},
                                        {0xFF / 255.f, 0xC8 / 255.f, 0x32 / 255.f},
                                        "status.busy",
                                        {0xFF / 255.f, 0xC8 / 255.f, 0x32 / 255.f},
                                        {1.0f, 0.78f, 0.20f, 0.10f},
                                        {0xFF / 255.f, 0xC8 / 255.f, 0x32 / 255.f},
                                        false};
    static const RowStyle kThinking  = {{0x9B / 255.f, 0x8C / 255.f, 0xFF / 255.f},
                                        {0x9B / 255.f, 0x8C / 255.f, 0xFF / 255.f},
                                        "status.thinking",
                                        {0x9B / 255.f, 0x8C / 255.f, 0xFF / 255.f},
                                        {0.61f, 0.55f, 1.0f, 0.10f},
                                        {0x9B / 255.f, 0x8C / 255.f, 0xFF / 255.f},
                                        false};
    static const RowStyle kToolUse   = {{0xFF / 255.f, 0x9B / 255.f, 0x32 / 255.f},
                                        {0xFF / 255.f, 0x9B / 255.f, 0x32 / 255.f},
                                        "status.toolUse",
                                        {0xFF / 255.f, 0x9B / 255.f, 0x32 / 255.f},
                                        {1.0f, 0.61f, 0.20f, 0.10f},
                                        {0xFF / 255.f, 0x9B / 255.f, 0x32 / 255.f},
                                        false};
    static const RowStyle kConfirm   = {{0xFF / 255.f, 0x44 / 255.f, 0x44 / 255.f},
                                        {0xFF / 255.f, 0x66 / 255.f, 0x66 / 255.f},
                                        "status.confirmationNeeded",
                                        {0xFF / 255.f, 0x66 / 255.f, 0x66 / 255.f},
                                        {1.0f, 0.27f, 0.27f, 0.08f},
                                        {0xFF / 255.f, 0x44 / 255.f, 0x44 / 255.f},
                                        true};
    if (status == "working") return kWorking;
    if (status == "thinking") return kThinking;
    if (status == "tool-use") return kToolUse;
    if (status == "confirmation-needed") return kConfirm;
    return kIdle;
}

// IDE 徽标（对齐 PC 端 BadgeFor：字母 + 双色调圆角块）
struct IdeBadge {
    char letter;
    float bg[3];
    float fg[3];
};
IdeBadge badgeFor(const std::string& ide) {
    if (ide == "qoder")
        return {'Q', {0xBA / 255.f, 0x8C / 255.f, 0xFF / 255.f},
                      {0xCF / 255.f, 0xAA / 255.f, 0xFF / 255.f}};
    if (ide == "cursor")
        return {'C', {0x50 / 255.f, 0xD2 / 255.f, 0xBE / 255.f},
                      {0x8F / 255.f, 0xE8 / 255.f, 0xD8 / 255.f}};
    if (ide == "codex")
        return {'X', {0xFF / 255.f, 0xAA / 255.f, 0x50 / 255.f},
                      {0xFF / 255.f, 0xC8 / 255.f, 0x80 / 255.f}};
    if (ide == "opencode")
        return {'O', {0xFF / 255.f, 0x78 / 255.f, 0xA0 / 255.f},
                      {0xFF / 255.f, 0x9F / 255.f, 0xB5 / 255.f}};
    return {'T', {0x64 / 255.f, 0x96 / 255.f, 0xFF / 255.f},
                  {0x9D / 255.f, 0xB9 / 255.f, 0xFF / 255.f}};
}

// UTF-8 解码（与 text_renderer 同款；前移指针返回码位）
unsigned int decodeUtf8(const char*& p, const char* end) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(p);
    if (s[0] < 0x80) { p += 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0 && p + 2 <= end && (s[1] & 0xC0) == 0x80) {
        p += 2;
        return ((unsigned int)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((s[0] & 0xF0) == 0xE0 && p + 3 <= end &&
        (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        p += 3;
        return ((unsigned int)(s[0] & 0x0F) << 12) |
               ((unsigned int)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((s[0] & 0xF8) == 0xF0 && p + 4 <= end &&
        (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        p += 4;
        return ((unsigned int)(s[0] & 0x07) << 18) |
               ((unsigned int)(s[1] & 0x3F) << 12) |
               ((unsigned int)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    p += 1;
    return 0xFFFD;
}

// 布局常量（480x800 竖屏，下半屏 480x400）
constexpr float kPanelMargin = 12.f;   // 面板左右距屏幕边
constexpr float kPad = 16.f;           // 行内容左右边距
constexpr float kRowH = 60.f;          // 行高
constexpr float kNameSize = 32.f;      // 项目名字号
constexpr float kStatusSize = 28.f;    // 状态文字字号
constexpr float kDotR = 8.f;           // 状态圆点半径
constexpr float kBadgeSize = 26.f;     // IDE 徽标边长
constexpr float kBadgeFont = 20.f;     // IDE 徽标字母字号
constexpr float kBorderW = 4.f;        // 行左边框宽
constexpr int kMaxRows = 6;            // 下半屏最多可容纳行数（6*60=360≤400）

} // namespace

struct TaskPanel::Impl {
    TextRenderer text;        // 任务列表（Noto SC，含 CJK）
    TextRenderer clock_font;  // 时钟专用（圆润卡通字体，仅数字/冒号）
    GLuint program = 0;
    std::vector<float> verts;

    void ensureProgram() {
        if (program) return;
        GLuint vs = compileShader(GL_VERTEX_SHADER, kVertSrc);
        GLuint fs = compileShader(GL_FRAGMENT_SHADER, kFragSrc);
        if (!vs || !fs) return;
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glBindAttribLocation(program, 0, "a_pos");
        glLinkProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        GLint ok = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512] = {};
            glGetProgramInfoLog(program, sizeof(log), nullptr, log);
            fprintf(stderr, "[TaskPanel] link: %s\n", log);
            glDeleteProgram(program);
            program = 0;
        }
    }

    // 顶点列表（xy 对）以 TRIANGLE_FAN 填充
    void fillFan(const std::vector<float>& fan, float r, float g, float b,
                 float a, int screen_w, int screen_h) {
        ensureProgram();
        if (!program || fan.size() < 6) return;
        glUseProgram(program);
        glUniform2f(glGetUniformLocation(program, "u_screen"),
                    (float)screen_w, (float)screen_h);
        glUniform4f(glGetUniformLocation(program, "u_color"), r, g, b, a);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
                              fan.data());
        glDrawArrays(GL_TRIANGLE_FAN, 0, (GLsizei)(fan.size() / 2));
        glDisableVertexAttribArray(0);
        glUseProgram(0);
    }

    void fillRect(float x0, float y0, float x1, float y1, float r, float g,
                  float b, float a, int sw, int sh) {
        fillFan({x0, y0, x1, y0, x1, y1, x0, y1}, r, g, b, a, sw, sh);
    }

    void fillCircle(float cx, float cy, float radius, float r, float g,
                    float b, float a, int sw, int sh) {
        std::vector<float> fan = {cx, cy};
        const int seg = 24;
        for (int i = 0; i <= seg; ++i) {
            const float t = 6.2831853f * i / seg;
            fan.push_back(cx + radius * std::cos(t));
            fan.push_back(cy + radius * std::sin(t));
        }
        fillFan(fan, r, g, b, a, sw, sh);
    }

    // 圆角矩形（TRIANGLE_FAN：中心 + 四角圆弧；凸多边形，fan 即可）
    void fillRoundedRect(float x0, float y0, float x1, float y1, float rad,
                         float r, float g, float b, float a, int sw, int sh) {
        if (x1 - x0 <= rad * 2 || y1 - y0 <= rad * 2)
            return fillRect(x0, y0, x1, y1, r, g, b, a, sw, sh);
        std::vector<float> fan = {(x0 + x1) * 0.5f, (y0 + y1) * 0.5f};
        // 四角圆心：左上→右上→右下→左下；角度从 π 起连续递减走一圈
        const float cx[4] = {x0 + rad, x1 - rad, x1 - rad, x0 + rad};
        const float cy[4] = {y1 - rad, y1 - rad, y0 + rad, y0 + rad};
        const float start[4] = {3.1415927f, 1.5707963f, 0.f, -1.5707963f};
        const int seg = 6;  // 每角 6 段
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i <= seg; ++i) {
                const float t = start[c] - (1.5707963f * i / seg);
                fan.push_back(cx[c] + rad * std::cos(t));
                fan.push_back(cy[c] + rad * std::sin(t));
            }
        // 闭合扇形：首边界点是左上角（角度 π），末点是左下角（-π），
        // 左边缘两点间不闭合会在左侧留一个缺口三角（中心-左上-左下）
        fan.push_back(cx[0] + rad * std::cos(3.1415927f));
        fan.push_back(cy[0] + rad * std::sin(3.1415927f));
        fillFan(fan, r, g, b, a, sw, sh);
    }

    // 超宽截断（码位边界 + 省略号），防项目名压掉状态文字
    std::string truncate(const std::string& s, float max_w, float size) {
        if (text.measureWidth(s, size) <= max_w) return s;
        static const char* kEll = "\xE2\x80\xA6";  // …
        const float ew = text.measureWidth(kEll, size);
        std::string out;
        const char* p = s.data();
        const char* end = p + s.size();
        while (p < end) {
            decodeUtf8(p, end);
            const std::string cand = s.substr(0, (size_t)(p - s.data()));
            if (text.measureWidth(cand, size) + ew > max_w) break;
            out = cand;
        }
        return out + kEll;
    }
};

TaskPanel::TaskPanel() : impl_(new Impl) {}
TaskPanel::~TaskPanel() {
    if (impl_->program) glDeleteProgram(impl_->program);
    delete impl_;
}

bool TaskPanel::init(const std::string& font_path) {
    // 时钟专用圆润卡通字体（与任务列表的 Noto SC 独立；缺失则回退主字体）
    impl_->clock_font.load("/opt/dutyon/assets/font-clock.ttf");
    if (!impl_->clock_font.isLoaded())
        printf("[TaskPanel] clock font missing, fallback to main font\n");
    return impl_->text.load(font_path);
}

void TaskPanel::render(const PetStatus& status, int screen_w, int screen_h,
                       float area_top) {
    auto* p = impl_;
    if (screen_w <= 0 || screen_h <= 0 || area_top <= 0) return;

    const int total = (int)status.sessions.size();
    const int shown = std::min(total, kMaxRows);

    // 面板充满下半屏（无上下留白）：顶边贴角色区、底边贴屏幕底
    const float x0 = kPanelMargin;
    const float x1 = screen_w - kPanelMargin;
    const float y_top = std::min(area_top, (float)screen_h);
    const float y_bottom = 0.f;

    // 面板底色（深灰半透明，对齐 PC 端状态栏底色观感）
    p->fillRect(x0, y_bottom, x1, y_top, 0.08f, 0.09f, 0.13f, 0.90f,
                screen_w, screen_h);

    if (shown == 0) {
        if (p->text.isLoaded()) {
            const std::string empty = "暂无任务";
            const float w = p->text.measureWidth(empty, kNameSize);
            const float lh = p->text.lineHeight(kNameSize);
            const float cy = (y_top + y_bottom) * 0.5f;
            p->text.draw(empty, (x0 + x1) * 0.5f - w * 0.5f, cy + lh * 0.5f,
                         kNameSize, 0.55f, 0.58f, 0.65f, 1.0f,
                         screen_w, screen_h);
        }
        return;
    }

    const float text_lh = p->text.lineHeight(kNameSize);
    const float badge_lh = p->text.lineHeight(kBadgeFont);
    // confirm 行红底呼吸动画（对齐 PC 端 item-glow：0.05↔0.12，周期 1.5s）
    static const auto t0 = std::chrono::steady_clock::now();
    const float now_s = std::chrono::duration<float>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    const float breathe =
        0.05f + 0.07f * (0.5f + 0.5f * std::sin(now_s * 6.2831853f / 1.5f));

    for (int i = 0; i < shown; ++i) {
        const SessionInfo& s = status.sessions[i];
        const RowStyle& st = styleFor(s.status);

        const float row_top = y_top - i * kRowH;
        const float row_bottom = row_top - kRowH;
        const float cy = row_top - kRowH * 0.5f;  // 行中心（GL y）

        // 行背景（活动状态整行淡色底；confirm 呼吸红）
        if (st.breathing)
            p->fillRect(x0, row_bottom, x1, row_top, st.bg[0], st.bg[1],
                        st.bg[2], breathe, screen_w, screen_h);
        else if (st.bg[3] > 0.f)
            p->fillRect(x0, row_bottom, x1, row_top, st.bg[0], st.bg[1],
                        st.bg[2], st.bg[3], screen_w, screen_h);

        // 左边框（状态色，PC 端 3px → 屏幕 2 倍宽取 4px）
        p->fillRect(x0, row_bottom, x0 + kBorderW, row_top, st.border[0],
                    st.border[1], st.border[2], 1.0f, screen_w, screen_h);

        // 状态圆点
        const float dot_x = x0 + kPad + kDotR;
        p->fillCircle(dot_x, cy, kDotR, st.dot[0], st.dot[1], st.dot[2], 1.0f,
                      screen_w, screen_h);

        // IDE 徽标（圆角块 + 首字母，颜色对齐 PC 端 BadgeFor）
        const float badge_x = dot_x + kDotR + 12.f;
        const IdeBadge b = badgeFor(s.ide);
        p->fillRoundedRect(badge_x, cy - kBadgeSize * 0.5f,
                           badge_x + kBadgeSize, cy + kBadgeSize * 0.5f, 5.f,
                           b.bg[0], b.bg[1], b.bg[2], 0.22f, screen_w,
                           screen_h);
        if (p->text.isLoaded()) {
            const char letter[2] = {b.letter, 0};
            const float lw = p->text.measureWidth(letter, kBadgeFont);
            p->text.draw(letter, badge_x + (kBadgeSize - lw) * 0.5f,
                         cy + badge_lh * 0.5f, kBadgeFont, b.fg[0], b.fg[1],
                         b.fg[2], 1.0f, screen_w, screen_h);
        }

        // 状态文字（右对齐）
        const char* st_txt = I18n::t(st.i18n_key);
        const float st_w = p->text.measureWidth(st_txt, kStatusSize);
        const float st_lh = p->text.lineHeight(kStatusSize);
        const float st_x = x1 - kPad - st_w;
        if (p->text.isLoaded())
            p->text.draw(st_txt, st_x, cy + st_lh * 0.5f, kStatusSize,
                         st.txt[0], st.txt[1], st.txt[2], 1.0f,
                         screen_w, screen_h);

        // 项目名（左侧，超宽截断；活动状态用强调色）
        if (p->text.isLoaded()) {
            const float name_x = badge_x + kBadgeSize + 12.f;
            const float name_max = st_x - name_x - 16.f;
            const std::string name =
                name_max > 20.f ? p->truncate(s.project_name, name_max, kNameSize)
                                : std::string();
            p->text.draw(name, name_x, cy + text_lh * 0.5f, kNameSize,
                         st.name[0], st.name[1], st.name[2], 1.0f,
                         screen_w, screen_h);
        }
    }
}

void TaskPanel::renderClock(const std::string& text, float y_top, float size,
                            int screen_w, int screen_h) {
    auto* p = impl_;
    // 时钟用圆润卡通字体（加载失败回退主字体）；字体缺失时静默回退
    TextRenderer& font =
        p->clock_font.isLoaded() ? p->clock_font : p->text;
    if (!font.isLoaded() || text.empty() || screen_w <= 0) return;

    // ===== 柔光橙黄时钟（无背板、无描边）=====
    // 主体：柔和橙黄（不刺眼的暖色）+ 同色多层柔和光晕，加粗卡通字形
    const float lh = font.lineHeight(size);
    const float w = font.measureWidth(text, size);
    const float x = ((float)screen_w - w) * 0.5f;
    const float ty = y_top;  // 文字顶边（GL y 向上）

    // 柔和光晕：暗橙黄、低透明度（柔光衬托，不刺眼）
    const float glow_r = 0.72f, glow_g = 0.48f, glow_b = 0.16f;
    struct Glow { float dx, dy, a; };
    const Glow glows[] = {
        // 外圈（半径大、最淡）
        {-6.f,0,0.07f},{6.f,0,0.07f},{0,-6.f,0.07f},{0,6.f,0.07f},
        {-4.f,-4.f,0.06f},{4.f,-4.f,0.06f},{-4.f,4.f,0.06f},{4.f,4.f,0.06f},
        // 中圈
        {-4.f,0,0.12f},{4.f,0,0.12f},{0,-4.f,0.12f},{0,4.f,0.12f},
        {-3.f,-3.f,0.11f},{3.f,-3.f,0.11f},{-3.f,3.f,0.11f},{3.f,3.f,0.11f},
        // 内圈（贴字形）
        {-2.f,0,0.18f},{2.f,0,0.18f},{0,-2.f,0.18f},{0,2.f,0.18f},
        {-1.5f,-1.5f,0.15f},{1.5f,-1.5f,0.15f},{-1.5f,1.5f,0.15f},{1.5f,1.5f,0.15f},
    };
    for (const auto& g : glows)
        font.draw(text, x + g.dx, ty + g.dy, size, glow_r, glow_g, glow_b,
                  g.a, screen_w, screen_h);

    // 主体数字：暗橙黄（柔和暖色、低亮度不刺眼），加粗卡通字形
    font.drawBold(text, x, ty, size, 0.80f, 0.55f, 0.20f, 1.0f,
                  screen_w, screen_h, size * 0.022f);
}

void TaskPanel::renderDate(const std::string& text, float y_top, float size,
                           int screen_w, int screen_h) {
    auto* p = impl_;
    // 日期含中文（年月日/星期），卡通字体无中文字形，必须用主字体渲染
    TextRenderer& font = p->text;
    if (!font.isLoaded() || text.empty() || screen_w <= 0) return;

    // 一行内放不下则自动缩小字号（宽度留 20px 边距）
    const float max_w = (float)screen_w - 20.f;
    while (size > 12.f && font.measureWidth(text, size) > max_w) size -= 2.f;

    const float w = font.measureWidth(text, size);
    const float x = ((float)screen_w - w) * 0.5f;
    const float ty = y_top;

    // 同款暗橙黄柔光（缩小版光晕：层数/半径随字号减小）
    const float glow_r = 0.72f, glow_g = 0.48f, glow_b = 0.16f;
    const float g1 = size * 0.05f, g2 = size * 0.09f;
    struct Glow { float dx, dy, a; };
    const Glow glows[] = {
        {-g2,0,0.07f},{g2,0,0.07f},{0,-g2,0.07f},{0,g2,0.07f},
        {-g1,0,0.13f},{g1,0,0.13f},{0,-g1,0.13f},{0,g1,0.13f},
        {-g1,-g1,0.10f},{g1,-g1,0.10f},{-g1,g1,0.10f},{g1,g1,0.10f},
    };
    for (const auto& g : glows)
        font.draw(text, x + g.dx, ty + g.dy, size, glow_r, glow_g, glow_b,
                  g.a, screen_w, screen_h);
    // 主体（同色暗橙黄）
    font.drawBold(text, x, ty, size, 0.80f, 0.55f, 0.20f, 1.0f,
                  screen_w, screen_h, size * 0.020f);
}

float TaskPanel::clockLineHeight(float pixel_size) const {
    TextRenderer& f =
        impl_->clock_font.isLoaded() ? impl_->clock_font : impl_->text;
    return f.isLoaded() ? f.lineHeight(pixel_size) : pixel_size;
}

} // namespace dutyon

#endif // !_WIN32
