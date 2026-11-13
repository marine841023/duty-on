#if !defined(_WIN32) || defined(DUTYON_UI_PREVIEW)

// 设备端界面绘制。设计说明见 task_panel.h。

#include "ui/task_panel.h"

#ifdef DUTYON_UI_PREVIEW
#include <GL/glew.h>
#else
#include <GLES3/gl3.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "render/text_renderer.h"
#include "ui/i18n.h"

namespace dutyon {

namespace {

// ---------------------------------------------------------------------------
// 着色器（GLES2 语法）。形状在片元里用圆角矩形 SDF 求覆盖率：圆角、描边、
// 柔光全部抗锯齿，一个四边形一个形状。v_p 是相对形状中心的局部坐标（数值小，
// mediump 也够精度）。
// ---------------------------------------------------------------------------
const char* kShapeVert =
    "attribute vec2 a_pos;\n"
    "uniform vec2 u_screen;\n"
    "uniform vec2 u_center;\n"
    "uniform vec2 u_ext;\n"
    "varying vec2 v_p;\n"
    "void main() {\n"
    "  v_p = a_pos * u_ext;\n"
    "  vec2 ndc = (u_center + v_p) / u_screen * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "}\n";

const char* kShapeFrag =
    "#ifdef GL_ES\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "#endif\n"
    "varying vec2 v_p;\n"
    "uniform vec2 u_half;\n"
    "uniform float u_radius;\n"
    "uniform vec4 u_top;\n"
    "uniform vec4 u_bot;\n"
    "uniform float u_border;\n"
    "uniform vec4 u_bcol;\n"
    "uniform float u_soft;\n"
    "uniform vec2 u_rot;\n"
    "void main() {\n"
    "  vec2 q = vec2(u_rot.x * v_p.x + u_rot.y * v_p.y,\n"
    "  -u_rot.y * v_p.x + u_rot.x * v_p.y);\n"
    "  vec2 e = abs(q) - u_half + u_radius;\n"
    "  float d = length(max(e, 0.0)) + min(max(e.x, e.y), 0.0) - u_radius;\n"
    "  float t = clamp(q.y / (2.0 * u_half.y) + 0.5, 0.0, 1.0);\n"
    "  vec4 col = mix(u_bot, u_top, t);\n"
    "  if (u_border > 0.0)\n"
    "    col = mix(col, u_bcol, smoothstep(-u_border - 0.75, -u_border + 0.75, d));\n"
    "  float a = 1.0 - smoothstep(-u_soft, u_soft, d);\n"
    "  gl_FragColor = vec4(col.rgb, col.a * a);\n"
    "}\n";

// 舞台光：椭圆径向衰减 + 抖动（8bit 屏上大面积暗渐变不加抖动会出色带）
const char* kGlowFrag =
    "#ifdef GL_ES\n"
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n"
    "precision highp float;\n"
    "#else\n"
    "precision mediump float;\n"
    "#endif\n"
    "#endif\n"
    "varying vec2 v_p;\n"
    "uniform vec2 u_half;\n"
    "uniform vec4 u_top;\n"
    "void main() {\n"
    "  float r = length(v_p / u_half);\n"
    "  float f = max(1.0 - r, 0.0);\n"
    "  float n = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);\n"
    "  float a = u_top.a * f * f + (n - 0.5) * (1.5 / 255.0);\n"
    "  gl_FragColor = vec4(u_top.rgb, max(a, 0.0));\n"
    "}\n";

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

GLuint linkProgram(const char* vert, const char* frag) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vert);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, frag);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_pos");
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        fprintf(stderr, "[TaskPanel] link: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ---------------------------------------------------------------------------
// 色板。整屏只有三类颜色：纸白系文字、主题色（时钟色）、状态色。
// ---------------------------------------------------------------------------
struct Color {
    float r, g, b, a;
};
constexpr Color rgb(int hex, float a = 1.0f) {
    return {((hex >> 16) & 0xFF) / 255.f, ((hex >> 8) & 0xFF) / 255.f, (hex & 0xFF) / 255.f, a};
}
constexpr Color withAlpha(Color c, float a) {
    return {c.r, c.g, c.b, a};
}

constexpr Color kPaper = rgb(0xF3EFE7);           // 主要文字（暖白）
constexpr Color kDim = rgb(0xA29D93);             // 次要文字
constexpr Color kFaint = rgb(0x6B6860);           // 说明、占位
constexpr Color kTileTop = rgb(0x24242A, 0.90f);  // 牌面（上沿略亮，像受光）
constexpr Color kTileBot = rgb(0x17171B, 0.90f);
constexpr Color kHairline = rgb(0xFFFFFF, 0.09f);
constexpr Color kCoral = rgb(0xFF5F52);  // 需要你：确认 / 断开
constexpr Color kCoralDeep = rgb(0xF2463B);
constexpr Color kCoralInk = rgb(0x2A0906);  // 珊瑚底上的深色字

constexpr Color kIdle = rgb(0x7D8797);
constexpr Color kThinking = rgb(0xAE9FFF);
constexpr Color kTool = rgb(0xFF9F45);
constexpr Color kWorking = rgb(0xFFC94D);

struct StatusStyle {
    Color color;
    const char* i18n_key;
    int lamp;  // 0 空心环 / 1 脉冲点 / 2 三点 / 3 转圈 / 4 感叹号
};
StatusStyle styleFor(const std::string& status) {
    if (status == "working") return {kWorking, "status.busy", 1};
    if (status == "thinking") return {kThinking, "status.thinking", 2};
    if (status == "tool-use") return {kTool, "status.toolUse", 3};
    if (status == "confirmation-needed") return {kCoral, "status.confirmationNeeded", 4};
    return {kIdle, "status.idle", 0};
}

struct IdeTag {
    char letter;
    Color color;
};
IdeTag ideTag(const std::string& ide) {
    if (ide == "qoder") return {'Q', rgb(0xCFAAFF)};
    if (ide == "cursor") return {'C', rgb(0x8FE8D8)};
    if (ide == "codex") return {'X', rgb(0xFFC880)};
    if (ide == "claude") return {'A', rgb(0xF09A7C)};
    if (ide == "opencode") return {'O', rgb(0xFF9FB5)};
    return {'T', rgb(0x9DB9FF)};
}

// UTF-8 解码（前移指针返回码位；非法字节按单字节步进）
unsigned int decodeUtf8(const char*& p, const char* end) {
    const unsigned char* s = reinterpret_cast<const unsigned char*>(p);
    int n = s[0] < 0x80             ? 1
            : (s[0] & 0xE0) == 0xC0 ? 2
            : (s[0] & 0xF0) == 0xE0 ? 3
            : (s[0] & 0xF8) == 0xF0 ? 4
                                    : 1;
    if (p + n > end) n = 1;
    p += n;
    return s[0];
}

// 布局（480x800 竖屏 / 800x480 横屏右列 320 宽）
constexpr float kMargin = 12.f;    // 牌距区域左右边
constexpr float kRowPitch = 60.f;  // 牌间距（含 8px 空隙）
constexpr float kTileH = 52.f;
constexpr float kTileRadius = 16.f;
constexpr float kListTopGap = 8.f;
constexpr float kListBottom = 12.f;
constexpr int kMaxRows = 6;
constexpr float kNameSize = 30.f;
constexpr float kStatusSize = 24.f;
constexpr float kPi = 3.1415927f;

}  // namespace

struct TaskPanel::Impl {
    TextRenderer text;          // 正文（Noto Sans SC）
    TextRenderer clock_font;    // 时钟数字
    Color ink = rgb(0xD49E52);  // 主题色（setClockColor）
    GLuint shape_prog = 0, glow_prog = 0;
    bool programs_tried = false;
    int sw = 1, sh = 1;  // 当前帧屏幕尺寸（每个公开入口先 begin()）

    TextRenderer& digits() { return clock_font.isLoaded() ? clock_font : text; }

    void begin(int screen_w, int screen_h) {
        sw = screen_w;
        sh = screen_h;
        if (!programs_tried) {
            programs_tried = true;
            shape_prog = linkProgram(kShapeVert, kShapeFrag);
            glow_prog = linkProgram(kShapeVert, kGlowFrag);
        }
    }

    float now() const {
        static const auto t0 = std::chrono::steady_clock::now();
        return std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count();
    }

    void quad(GLuint prog, float cx, float cy, float ex, float ey) {
        static const float kCorners[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
        glUniform2f(glGetUniformLocation(prog, "u_screen"), (float)sw, (float)sh);
        glUniform2f(glGetUniformLocation(prog, "u_center"), cx, cy);
        glUniform2f(glGetUniformLocation(prog, "u_ext"), ex, ey);
        glEnable(GL_BLEND);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, kCorners);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glDisableVertexAttribArray(0);
        glUseProgram(0);
    }

    // 通用形状：圆角矩形 [x0,x1]x[y0,y1]，上下渐变，可选描边、柔边、旋转
    void shape(float x0, float y0, float x1, float y1, float radius, Color top, Color bot,
               float border = 0.f, Color bcol = {0, 0, 0, 0}, float soft = 0.75f,
               float angle = 0.f) {
        if (!shape_prog || x1 <= x0 || y1 <= y0) return;
        const float hx = (x1 - x0) * 0.5f, hy = (y1 - y0) * 0.5f;
        radius = std::min(radius, std::min(hx, hy));
        glUseProgram(shape_prog);
        glUniform2f(glGetUniformLocation(shape_prog, "u_half"), hx, hy);
        glUniform1f(glGetUniformLocation(shape_prog, "u_radius"), radius);
        glUniform4f(glGetUniformLocation(shape_prog, "u_top"), top.r, top.g, top.b, top.a);
        glUniform4f(glGetUniformLocation(shape_prog, "u_bot"), bot.r, bot.g, bot.b, bot.a);
        glUniform1f(glGetUniformLocation(shape_prog, "u_border"), border);
        glUniform4f(glGetUniformLocation(shape_prog, "u_bcol"), bcol.r, bcol.g, bcol.b, bcol.a);
        glUniform1f(glGetUniformLocation(shape_prog, "u_soft"), soft);
        glUniform2f(glGetUniformLocation(shape_prog, "u_rot"), std::cos(angle), std::sin(angle));
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // 旋转后的形状用外接圆大小的四边形覆盖
        const float pad = soft + 1.f;
        const float ex = angle != 0.f ? std::sqrt(hx * hx + hy * hy) + pad : hx + pad;
        const float ey = angle != 0.f ? ex : hy + pad;
        quad(shape_prog, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, ex, ey);
    }
    void fill(float x0, float y0, float x1, float y1, float radius, Color c) {
        shape(x0, y0, x1, y1, radius, c, c);
    }
    void circle(float cx, float cy, float r, Color c) {
        shape(cx - r, cy - r, cx + r, cy + r, r, c, c);
    }
    void ring(float cx, float cy, float r, float width, Color c) {
        const Color none = withAlpha(c, 0.f);
        shape(cx - r, cy - r, cx + r, cy + r, r, none, none, width, c);
    }
    // 柔光团（叠加混合，画在黑底上像打了一盏灯）
    void glow(float cx, float cy, float rx, float ry, Color c) {
        if (!glow_prog || c.a <= 0.f) return;
        glUseProgram(glow_prog);
        glUniform2f(glGetUniformLocation(glow_prog, "u_half"), rx, ry);
        glUniform4f(glGetUniformLocation(glow_prog, "u_top"), c.r, c.g, c.b, c.a);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        quad(glow_prog, cx, cy, rx, ry);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }

    // ---- 文字 ----
    void put(TextRenderer& f, const std::string& s, float x, float y_top, float size, Color c,
             bool bold = false) {
        if (!f.isLoaded() || s.empty()) return;
        if (bold)
            f.drawBold(s, x, y_top, size, c.r, c.g, c.b, c.a, sw, sh, size * 0.022f);
        else
            f.draw(s, x, y_top, size, c.r, c.g, c.b, c.a, sw, sh);
    }
    // 垂直居中于 cy
    void putMid(TextRenderer& f, const std::string& s, float x, float cy, float size, Color c,
                bool bold = false) {
        put(f, s, x, cy + f.lineHeight(size) * 0.5f, size, c, bold);
    }
    // 水平居中于 [x0,x1]，过宽缩字号
    float putCentered(TextRenderer& f, const std::string& s, float x0, float x1, float y_top,
                      float size, Color c, bool bold = false) {
        size = fit(f, s, x1 - x0, size);
        put(f, s, (x0 + x1) * 0.5f - f.measureWidth(s, size) * 0.5f, y_top, size, c, bold);
        return size;
    }
    float fit(TextRenderer& f, const std::string& s, float max_w, float size) {
        while (size > 12.f && f.measureWidth(s, size) > max_w) size -= 1.f;
        return size;
    }
    // 超宽截断（码位边界 + 省略号）
    std::string truncate(const std::string& s, float max_w, float size) {
        if (text.measureWidth(s, size) <= max_w) return s;
        static const char* kEll = "\xE2\x80\xA6";
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

    // ---- 状态灯：每种状态一个不同的动作，不看颜色也分得清 ----
    void lamp(float cx, float cy, const StatusStyle& st, float t) {
        const Color c = st.color;
        switch (st.lamp) {
            case 0:  // 空闲：静止的空心环
                ring(cx, cy, 7.f, 2.f, withAlpha(c, 0.9f));
                break;
            case 1: {  // 忙碌：实心点 + 向外扩散的波纹
                const float k = std::fmod(t / 1.6f, 1.f);
                ring(cx, cy, 6.f + 9.f * k, 1.6f, withAlpha(c, 0.55f * (1.f - k)));
                circle(cx, cy, 6.f, c);
                break;
            }
            case 2:  // 思考：三点依次起伏
                for (int i = 0; i < 3; ++i) {
                    const float w = 0.5f + 0.5f * std::sin(t * 4.2f - i * 0.9f);
                    circle(cx + (i - 1) * 9.f, cy + (w - 0.5f) * 5.f, 3.2f,
                           withAlpha(c, 0.45f + 0.55f * w));
                }
                break;
            case 3:  // 执行：转圈
                for (int i = 0; i < 8; ++i) {
                    const float ang = 2.f * kPi * i / 8.f;
                    float k = std::fmod(i / 8.f + t * 1.1f, 1.f);
                    circle(cx + 9.f * std::cos(-ang), cy + 9.f * std::sin(-ang), 2.3f,
                           withAlpha(c, 0.18f + 0.82f * k * k));
                }
                break;
            default: {  // 需要确认：深色圆里一个感叹号（画在珊瑚底上）
                circle(cx, cy, 13.f, kCoralInk);
                fill(cx - 1.8f, cy - 2.f, cx + 1.8f, cy + 7.5f, 1.8f, kCoral);
                circle(cx, cy - 6.5f, 2.1f, kCoral);
                break;
            }
        }
    }

    // ---- 一张值班牌 ----
    void tile(const SessionInfo& s, float x0, float x1, float y_top, float t) {
        const StatusStyle st = styleFor(s.status);
        const bool alert = st.lamp == 4;
        const bool idle = st.lamp == 0;
        const float y0 = y_top - kTileH, cy = y_top - kTileH * 0.5f;

        if (alert) {
            // 整块反色 + 呼吸的外发光：隔着桌子也看得见
            const float k = 0.5f + 0.5f * std::sin(t * 2.f * kPi / 1.4f);
            shape(x0 - 2.f, y0 - 2.f, x1 + 2.f, y_top + 2.f, kTileRadius + 2.f,
                  withAlpha(kCoral, 0.20f + 0.30f * k), withAlpha(kCoral, 0.20f + 0.30f * k), 0.f,
                  {0, 0, 0, 0}, 9.f);
            shape(x0, y0, x1, y_top, kTileRadius, kCoral, kCoralDeep);
        } else {
            shape(x0, y0, x1, y_top, kTileRadius, kTileTop, kTileBot, 1.f, kHairline);
        }

        lamp(x0 + 30.f, cy, st, t);
        if (!text.isLoaded()) return;

        // 状态词（右对齐）
        const char* label = I18n::t(st.i18n_key);
        const float lw = text.measureWidth(label, kStatusSize);
        const float lx = x1 - 18.f - lw;
        putMid(text, label, lx, cy, kStatusSize,
               alert  ? kCoralInk
               : idle ? kFaint
                      : st.color,
               alert);

        // 项目名 + 紧跟其后的 IDE 小标
        const float nx = x0 + 56.f;
        const float tag_w = 22.f, tag_gap = 8.f;
        const float name_max = lx - 14.f - nx - tag_w - tag_gap;
        if (name_max < 24.f) return;
        const std::string name = truncate(s.project_name, name_max, kNameSize);
        putMid(text, name, nx, cy, kNameSize, alert ? kCoralInk : idle ? kDim : kPaper, alert);

        const IdeTag tag = ideTag(s.ide);
        const float tx = nx + text.measureWidth(name, kNameSize) + tag_gap;
        const Color tc = alert ? kCoralInk : withAlpha(tag.color, idle ? 0.55f : 0.9f);
        const Color none = withAlpha(tc, 0.f);
        shape(tx, cy - 11.f, tx + tag_w, cy + 11.f, 6.f, none, none, 1.2f, withAlpha(tc, 0.5f));
        const char letter[2] = {tag.letter, 0};
        putMid(text, letter, tx + (tag_w - text.measureWidth(letter, 16.f)) * 0.5f, cy, 16.f, tc);
    }

    // 最要紧的一个会话：待确认 > 进行中；全空闲返回 -1
    static int focusIndex(const PetStatus& status) {
        int best = -1, best_rank = 0;
        for (int i = 0; i < (int)status.sessions.size(); ++i) {
            const std::string& s = status.sessions[i].status;
            const int rank = s == "confirmation-needed" ? 2 : s == "idle" ? 0 : 1;
            if (rank > best_rank) {
                best = i;
                best_rank = rank;
            }
        }
        return best;
    }

    // ---- 引导画面的小部件 ----
    // 步骤：圆圈序号 + 主文 + 可选说明；返回占用高度
    float step(int n, const std::string& main, const std::string& sub, float x0, float x1,
               float y_top) {
        const float cy = y_top - 17.f;
        ring(x0 + 17.f, cy, 16.f, 1.5f, withAlpha(ink, 0.8f));
        const std::string num = std::to_string(n);
        putMid(digits(), num, x0 + 17.f - digits().measureWidth(num, 20.f) * 0.5f, cy, 20.f, ink);
        const float tx = x0 + 48.f;
        float size = fit(text, main, x1 - tx, 26.f);
        putMid(text, main, tx, cy, size, kPaper);
        if (sub.empty()) return 50.f;
        size = fit(text, sub, x1 - tx, 21.f);
        put(text, sub, tx, y_top - 38.f, size, kDim);
        return 78.f;
    }
    void spinner(float cx, float cy, float radius, float t) {
        for (int i = 0; i < 12; ++i) {
            const float ang = 2.f * kPi * i / 12.f;
            const float k = std::fmod(i / 12.f + t * 0.9f, 1.f);
            circle(cx + radius * std::cos(-ang), cy + radius * std::sin(-ang), radius * 0.11f,
                   withAlpha(ink, 0.12f + 0.88f * k * k));
        }
    }
};

TaskPanel::TaskPanel() : impl_(new Impl) {}
TaskPanel::~TaskPanel() {
    if (impl_->shape_prog) glDeleteProgram(impl_->shape_prog);
    if (impl_->glow_prog) glDeleteProgram(impl_->glow_prog);
    delete impl_;
}

bool TaskPanel::init(const std::string& font_path, const std::string& clock_font_path) {
    impl_->clock_font.load(clock_font_path);
    if (!impl_->clock_font.isLoaded())
        printf("[TaskPanel] clock font missing, fallback to main font\n");
    return impl_->text.load(font_path);
}

void TaskPanel::setClockColor(const std::string& name) {
    struct Preset {
        const char* name;
        Color c;
    };
    static const Preset kPresets[] = {
        {"amber", rgb(0xE0A653)},  // 琥珀（默认）
        {"ice", rgb(0x8AC2E6)},    // 冰蓝
        {"white", rgb(0xE2DCCB)},  // 暖白
        {"green", rgb(0x7ECF86)},  // 竹绿
        {"pink", rgb(0xE897C6)},   // 樱粉
    };
    impl_->ink = kPresets[0].c;
    for (const auto& k : kPresets)
        if (name == k.name) impl_->ink = k.c;
}

// ---------------------------------------------------------------------------
// 舞台光
// ---------------------------------------------------------------------------
void TaskPanel::renderBackdrop(const std::string& overall_state, int screen_w, int screen_h, int vx,
                               int vy, int vw, int vh) {
    auto* p = impl_;
    if (screen_w <= 0 || screen_h <= 0 || vw <= 0 || vh <= 0) return;
    p->begin(screen_w, screen_h);
    glViewport(0, 0, screen_w, screen_h);
    const float t = p->now();
    Color c = p->ink;
    float level, period;
    if (overall_state == "alert") {
        c = kCoral;
        level = 0.34f;
        period = 1.4f;  // 与待确认牌同步呼吸
    } else if (overall_state == "working") {
        level = 0.26f;
        period = 3.2f;
    } else {
        level = 0.10f;  // 睡着：只留一点余光
        period = 6.0f;
    }
    const float k = 0.82f + 0.18f * std::sin(t * 2.f * kPi / period);
    const float cx = vx + vw * 0.5f;
    // 身后的大光团 + 脚下的一摊光
    p->glow(cx, vy + vh * 0.52f, vw * 0.62f, vh * 0.56f, withAlpha(c, level * k * 0.62f));
    p->glow(cx, vy + vh * 0.09f, vw * 0.46f, vh * 0.085f, withAlpha(c, level * k * 1.5f));
}

// ---------------------------------------------------------------------------
// 时间栏
// ---------------------------------------------------------------------------
float TaskPanel::headerHeight(bool large, bool landscape) const {
    if (!landscape) return large ? 150.f : 104.f;
    const float size = large ? 80.f : 60.f;
    return 40.f + impl_->digits().lineHeight(size) + 44.f;
}

void TaskPanel::renderHeader(const std::string& hms, const std::string& date,
                             const std::string& weekday, bool large, bool landscape, int screen_w,
                             int screen_h, int region_x, int region_w) {
    auto* p = impl_;
    if (screen_w <= 0 || screen_h <= 0 || hms.size() < 5) return;
    p->begin(screen_w, screen_h);
    if (region_w < 0) region_w = screen_w;
    TextRenderer& df = p->digits();
    if (!df.isLoaded()) return;

    const std::string hm = hms.substr(0, 5);
    const std::string ss = hms.size() >= 8 ? hms.substr(6, 2) : std::string();
    const float rx0 = (float)region_x, rx1 = (float)(region_x + region_w);

    // 时:分 大字 + 秒小字（同一基线）。秒是"它还活着"的信号，但不该抢眼
    auto drawTime = [&](float x, float y_top, float size) {
        const float ss_size = size * 0.42f;
        p->put(df, hm, x, y_top, size, p->ink, true);
        const float sx = x + df.measureWidth(hm, size) + size * 0.12f;
        p->put(df, ss, sx, y_top - df.ascent(size) + df.ascent(ss_size), ss_size,
               withAlpha(p->ink, 0.55f));
        return sx + df.measureWidth(ss, ss_size);
    };
    auto timeWidth = [&](float size) {
        return df.measureWidth(hm, size) + size * 0.12f + df.measureWidth(ss, size * 0.42f);
    };

    if (landscape) {
        // 右列：时间左对齐，日期一行在下
        float size = large ? 80.f : 60.f;
        while (size > 28.f && timeWidth(size) > region_w - 32.f) size -= 2.f;
        const float top = (float)screen_h - 40.f;
        drawTime(rx0 + 16.f, top, size);
        const std::string line = date.empty()      ? weekday
                                 : weekday.empty() ? date
                                                   : date + "  " + weekday;
        const float dsize = p->fit(p->text, line, region_w - 32.f, 24.f);
        p->put(p->text, line, rx0 + 18.f, top - df.lineHeight(size) - 4.f, dsize, kDim);
    } else if (large) {
        // 单任务/相框：时间居中置顶，日期贴屏幕底（角色占满中间）
        float size = 100.f;
        while (size > 40.f && timeWidth(size) > region_w - 48.f) size -= 2.f;
        drawTime((rx0 + rx1) * 0.5f - timeWidth(size) * 0.5f, (float)screen_h - 42.f, size);
        const std::string line = date.empty()      ? weekday
                                 : weekday.empty() ? date
                                                   : date + "  " + weekday;
        p->putCentered(p->text, line, rx0 + 20.f, rx1 - 20.f, 52.f, 26.f, kDim);
    } else {
        // 多任务：时间在左，星期/日期两行在右（连接图标正下方）
        const float size = 64.f;
        const float end_x = drawTime(rx0 + 20.f, (float)screen_h - 18.f, size);
        const float right = rx1 - 18.f;
        float wsize = 26.f, dsize = 22.f;
        const float room = right - end_x - 16.f;
        wsize = p->fit(p->text, weekday, room, wsize);
        dsize = p->fit(p->text, date, room, dsize);
        p->put(p->text, weekday, right - p->text.measureWidth(weekday, wsize),
               (float)screen_h - 40.f, wsize, kPaper);
        p->put(p->text, date, right - p->text.measureWidth(date, dsize), (float)screen_h - 72.f,
               dsize, kDim);
    }
}

// ---------------------------------------------------------------------------
// 任务牌
// ---------------------------------------------------------------------------
float TaskPanel::heightForSessions(int session_count) {
    const int rows = std::max(1, std::min(session_count, kMaxRows));
    return rows * kRowPitch - (kRowPitch - kTileH) + kListTopGap + kListBottom;
}

void TaskPanel::render(const PetStatus& status, int screen_w, int screen_h, float area_top,
                       int region_x, int region_w) {
    auto* p = impl_;
    if (screen_w <= 0 || screen_h <= 0 || area_top <= 0) return;
    p->begin(screen_w, screen_h);
    if (region_w < 0) region_w = screen_w;
    const float x0 = region_x + kMargin, x1 = region_x + region_w - kMargin;
    const float top = area_top - kListTopGap;
    const float t = p->now();

    const int total = (int)status.sessions.size();
    if (total == 0) {
        // 空状态：一张只有轮廓的空牌
        const Color none = {0, 0, 0, 0};
        p->shape(x0, top - kTileH, x1, top, kTileRadius, none, none, 1.f, kHairline);
        p->putCentered(p->text, "现在没有任务", x0, x1,
                       top - kTileH * 0.5f + p->text.lineHeight(24.f) * 0.5f, 24.f, kFaint);
        return;
    }

    // 能放下几张：受区域高度限制（横屏右列较矮）
    int fit_rows = (int)((top - kListBottom + (kRowPitch - kTileH)) / kRowPitch);
    fit_rows = std::max(1, std::min(fit_rows, kMaxRows));
    const bool overflow = total > fit_rows;
    const int shown = overflow ? fit_rows - 1 : total;

    // 放不下时待确认的任务优先占位（警报不能被折叠掉），其余保持原顺序
    std::vector<int> order;
    for (int i = 0; i < shown; ++i) order.push_back(i);
    for (int i = shown; i < total; ++i) {
        if (status.sessions[i].status != "confirmation-needed") continue;
        for (int k = (int)order.size() - 1; k >= 0; --k) {
            if (status.sessions[order[k]].status == "confirmation-needed") continue;
            order.erase(order.begin() + k);
            order.push_back(i);
            break;
        }
    }
    std::sort(order.begin(), order.end());
    for (int row = 0; row < (int)order.size(); ++row)
        p->tile(status.sessions[order[row]], x0, x1, top - row * kRowPitch, t);

    if (overflow) {
        int hidden_alerts = 0;
        for (int i = 0; i < total; ++i)
            if (status.sessions[i].status == "confirmation-needed" &&
                std::find(order.begin(), order.end(), i) == order.end())
                ++hidden_alerts;
        const float y_top = top - shown * kRowPitch;
        std::string more = "还有 " + std::to_string(total - shown) + " 个任务";
        if (hidden_alerts > 0) more += "，" + std::to_string(hidden_alerts) + " 个等你确认";
        p->putCentered(p->text, more, x0, x1,
                       y_top - kTileH * 0.5f + p->text.lineHeight(22.f) * 0.5f, 22.f,
                       hidden_alerts > 0 ? kCoral : kFaint);
    }
}

void TaskPanel::renderFocus(const PetStatus& status, int screen_w, int screen_h, float area_top,
                            int region_x, int region_w) {
    auto* p = impl_;
    if (screen_w <= 0 || screen_h <= 0) return;
    const int i = Impl::focusIndex(status);
    if (i < 0) return;
    p->begin(screen_w, screen_h);
    if (region_w < 0) region_w = screen_w;
    p->tile(status.sessions[i], region_x + kMargin, region_x + region_w - kMargin, area_top,
            p->now());
}

// ---------------------------------------------------------------------------
// 语音指令区
// ---------------------------------------------------------------------------
namespace {
constexpr float kPillGap = 8.f;
constexpr float kPillH = 46.f;
constexpr float kHintSize = 28.f;
constexpr float kHintGap = 10.f;
constexpr float kVoiceBottom = 12.f;
}  // namespace

float TaskPanel::voicePanelHeight(int command_count) {
    auto* p = impl_;
    if (!p->text.isLoaded()) return 0.f;
    const int rows = command_count > 0 ? (command_count + 4) / 5 : 0;
    return p->text.lineHeight(kHintSize) + kHintGap + rows * kPillH +
           (rows > 1 ? (rows - 1) * kPillGap : 0.f) + kVoiceBottom;
}

float TaskPanel::renderVoicePanel(const std::vector<std::string>& commands, int highlight,
                                  const std::string& hint, int screen_w, int screen_h,
                                  float area_top, int region_x, int region_w) {
    auto* p = impl_;
    if (screen_w <= 0 || screen_h <= 0 || area_top <= 0.f || !p->text.isLoaded()) return 0.f;
    p->begin(screen_w, screen_h);
    if (region_w < 0) region_w = screen_w;
    const int n = (int)commands.size();
    const int rows = n > 0 ? (n + 4) / 5 : 0;
    const float panel_h = std::min(voicePanelHeight(n), area_top);
    const float x0 = region_x + kMargin, x1 = region_x + region_w - kMargin;
    const float t = p->now();

    // 提示行：居中，前面一个跟着声音起伏的小麦克风点
    const float hint_h = p->text.lineHeight(kHintSize);
    const float hsize = p->fit(p->text, hint, x1 - x0 - 40.f, kHintSize);
    const float hw = p->text.measureWidth(hint, hsize);
    const float hx = (x0 + x1) * 0.5f - hw * 0.5f + 12.f;
    const float hcy = area_top - hint_h * 0.5f;
    const float beat = 0.5f + 0.5f * std::sin(t * 3.2f);
    p->circle(hx - 22.f, hcy, 4.f + 2.5f * beat, withAlpha(p->ink, 0.35f));
    p->circle(hx - 22.f, hcy, 4.f, p->ink);
    p->putMid(p->text, hint, hx, hcy, hsize, kPaper);
    if (rows == 0) return panel_h;

    const float usable = x1 - x0;
    const float top = area_top - hint_h - kHintGap;
    for (int r = 0; r < rows; ++r) {
        const int cols = std::min(5, n - r * 5);
        const float pill_w = (usable - kPillGap * (cols - 1)) / (float)cols;
        for (int c = 0; c < cols; ++c) {
            const int idx = r * 5 + c;
            const float px0 = x0 + c * (pill_w + kPillGap), px1 = px0 + pill_w;
            const float py1 = top - r * (kPillH + kPillGap), py0 = py1 - kPillH;
            const bool hi = idx == highlight;
            if (hi) {
                // 命中的指令：主题色实底 + 深色字，外圈一闪
                const float k = 0.5f + 0.5f * std::sin(t * 2.f * kPi / 1.2f);
                p->shape(px0 - 2.f, py0 - 2.f, px1 + 2.f, py1 + 2.f, kPillH * 0.5f + 2.f,
                         withAlpha(p->ink, 0.25f + 0.3f * k), withAlpha(p->ink, 0.25f + 0.3f * k),
                         0.f, {0, 0, 0, 0}, 8.f);
                p->fill(px0, py0, px1, py1, kPillH * 0.5f, p->ink);
            } else {
                p->shape(px0, py0, px1, py1, kPillH * 0.5f, kTileTop, kTileBot, 1.f, kHairline);
            }
            const float size = p->fit(p->text, commands[idx], pill_w - 14.f, 24.f);
            const float tw = p->text.measureWidth(commands[idx], size);
            p->putMid(p->text, commands[idx], (px0 + px1) * 0.5f - tw * 0.5f, (py0 + py1) * 0.5f,
                      size, hi ? rgb(0x15120C) : kPaper, hi);
        }
    }
    return panel_h;
}

// ---------------------------------------------------------------------------
// 引导画面
// ---------------------------------------------------------------------------
void TaskPanel::renderProvision(const std::string& ssid, const std::string& pass,
                                const std::string& failure, int screen_w, int screen_h) {
    auto* p = impl_;
    if (screen_w <= 0 || screen_h <= 0) return;
    p->begin(screen_w, screen_h);
    const bool landscape = screen_w >= screen_h;
    const float H = (float)screen_h;
    // 竖屏单列；横屏左列热点信息、右列步骤
    const float ax0 = 28.f, ax1 = landscape ? screen_w * 0.5f - 14.f : screen_w - 28.f;
    const float bx0 = landscape ? screen_w * 0.5f + 14.f : 28.f;
    const float bx1 = screen_w - 28.f;

    // 标题：上次失败就直说原因
    float y = H - 44.f;
    const bool failed = !failure.empty();
    p->put(p->text, failed ? failure : "给设备连上 Wi-Fi", ax0, y,
           p->fit(p->text, failed ? failure : "给设备连上 Wi-Fi", ax1 - ax0, 38.f),
           failed ? kCoral : kPaper, true);
    y -= 56.f;
    p->put(p->text, "先用手机连接这个设备热点", ax0, y,
           p->fit(p->text, "先用手机连接这个设备热点", ax1 - ax0, 24.f), kDim);
    y -= 46.f;

    // 热点牌：名称与密码是要照着输入的，给最大的字
    const float card_h = 212.f;
    p->shape(ax0 - 8.f, y - card_h, ax1 + 8.f, y, 22.f, kTileTop, kTileBot, 1.f, kHairline);
    const float cx0 = ax0 + 14.f, cx1 = ax1 - 14.f;
    p->put(p->text, "热点名称", cx0, y - 18.f, 20.f, kFaint);
    p->put(p->text, ssid, cx0, y - 44.f, p->fit(p->text, ssid, cx1 - cx0, 44.f), p->ink, true);
    p->fill(cx0, y - 108.f, cx1, y - 107.f, 0.f, kHairline);
    p->put(p->text, "密码", cx0, y - 122.f, 20.f, kFaint);
    p->put(p->text, pass, cx0, y - 148.f, p->fit(p->text, pass, cx1 - cx0, 44.f), kPaper, true);
    y -= card_h;

    // 步骤
    float sy = landscape ? H - 48.f : y - 34.f;
    sy -= p->step(1, "连上后点「登录到网络」通知", "没有弹出就用浏览器打开 192.168.4.1", bx0, bx1,
                  sy);
    sy -= p->step(2, "选择家里的 Wi-Fi 并输入密码", "", bx0, bx1, sy);
    sy -= p->step(3, "手机提示无互联网时", "选择继续使用此网络", bx0, bx1, sy);
    sy -= p->step(4, "联网后这里会显示配对码", "", bx0, bx1, sy);

    // 电脑端还没装的人需要知道去哪下载
    const std::string foot = "电脑端下载  gitee.com/megrezsoft/duty-on";
    p->put(p->text, foot, bx0, 46.f, p->fit(p->text, foot, bx1 - bx0, 20.f), kFaint);
}

void TaskPanel::renderJoining(const std::string& progress, int screen_w, int screen_h) {
    auto* p = impl_;
    if (screen_w <= 0 || screen_h <= 0) return;
    p->begin(screen_w, screen_h);
    const float H = (float)screen_h, x0 = 28.f, x1 = screen_w - 28.f;
    const float cy = H * 0.62f;
    p->glow(screen_w * 0.5f, cy, 150.f, 150.f, withAlpha(p->ink, 0.16f));
    p->spinner(screen_w * 0.5f, cy, 46.f, p->now());
    p->putCentered(p->text, "正在连接 Wi-Fi", x0, x1, cy - 96.f, 38.f, kPaper, true);
    p->putCentered(p->text, progress, x0, x1, cy - 156.f, 26.f, p->ink);
    p->putCentered(p->text, "连不上会自动回到设备热点", x0, x1, cy - 204.f, 22.f, kFaint);
}

void TaskPanel::renderPairing(const std::string& code, bool waiting, bool wifi_just_ok,
                              int screen_w, int screen_h) {
    auto* p = impl_;
    if (screen_w <= 0 || screen_h <= 0) return;
    p->begin(screen_w, screen_h);
    const bool landscape = screen_w >= screen_h;
    const float H = (float)screen_h, x0 = 28.f, x1 = screen_w - 28.f;
    const float mid = screen_w * 0.5f;
    const float t = p->now();

    if (wifi_just_ok) {
        const std::string ok = "Wi-Fi 已连接";
        const float w = p->text.measureWidth(ok, 22.f) + 52.f;
        const float py1 = H - 30.f, py0 = py1 - 40.f;
        const Color none = {0, 0, 0, 0};
        p->shape(mid - w * 0.5f, py0, mid + w * 0.5f, py1, 20.f, none, none, 1.2f,
                 withAlpha(p->ink, 0.7f));
        p->circle(mid - w * 0.5f + 22.f, (py0 + py1) * 0.5f, 5.f, p->ink);
        p->putMid(p->text, ok, mid - w * 0.5f + 38.f, (py0 + py1) * 0.5f, 22.f, p->ink);
    }

    const float label_top = landscape ? H - 96.f : H * 0.5f + 200.f;
    p->putCentered(p->text, "配对码", x0, x1, label_top, 26.f, kDim);

    // 六位码：每位一格，三位一组（念和抄都按 3+3）
    const float cell_w = 62.f, cell_h = 88.f, gap = 8.f, group_gap = 22.f;
    const float total = cell_w * 6.f + gap * 4.f + group_gap;
    const float top = label_top - 52.f;
    p->glow(mid, top - cell_h * 0.5f, total * 0.62f, cell_h * 1.5f, withAlpha(p->ink, 0.14f));
    TextRenderer& df = p->digits();
    float cx = mid - total * 0.5f;
    for (int i = 0; i < 6; ++i) {
        p->shape(cx, top - cell_h, cx + cell_w, top, 16.f, kTileTop, kTileBot, 1.f, kHairline);
        if (i < (int)code.size()) {
            const std::string d(1, code[i]);
            p->putMid(df, d, cx + (cell_w - df.measureWidth(d, 64.f)) * 0.5f, top - cell_h * 0.5f,
                      64.f, p->ink, true);
        }
        cx += cell_w + (i == 2 ? group_gap : gap);
    }

    // 当前状态：一句话 + 状态灯
    float y = top - cell_h - (landscape ? 44.f : 64.f);
    const std::string head = waiting ? "等电脑上线" : "还没有电脑连接";
    const float hw = p->text.measureWidth(head, 34.f);
    const float hx = mid - hw * 0.5f + 16.f;
    p->lamp(hx - 26.f, y - p->text.lineHeight(34.f) * 0.5f,
            waiting ? StatusStyle{p->ink, "", 2} : StatusStyle{kIdle, "", 0}, t);
    p->put(p->text, head, hx, y, 34.f, kPaper, true);
    y -= landscape ? 58.f : 70.f;
    if (waiting) {
        p->putCentered(p->text, "电脑上的 Duty On 启动后会自动连接", x0, x1, y, 24.f, kDim);
    } else {
        p->putCentered(p->text, "在电脑上右键桌宠", x0, x1, y, 24.f, kDim);
        p->putCentered(p->text, "选 设备 → 配对设备，输入上面的配对码", x0, x1, y - 38.f, 24.f,
                       kDim);
    }
}

// ---------------------------------------------------------------------------
// 常驻叠加
// ---------------------------------------------------------------------------
void TaskPanel::renderNetStatus(bool wifi_online, bool pc_online, int screen_w, int screen_h) {
    auto* p = impl_;
    if (screen_w <= 0 || screen_h <= 0) return;
    p->begin(screen_w, screen_h);
    const float top = (float)screen_h - 14.f, bottom = top - 18.f;
    const Color off = withAlpha(kFaint, 0.9f);

    // 断开标记：珊瑚色斜杠（下垫一条黑边，压在图标上也清楚）
    auto slash = [&](float x0, float x1) {
        const float cx = (x0 + x1) * 0.5f, cy = (top + bottom) * 0.5f;
        p->shape(cx - 14.f, cy - 3.f, cx + 14.f, cy + 3.f, 3.f, rgb(0x000000, 0.85f),
                 rgb(0x000000, 0.85f), 0.f, {0, 0, 0, 0}, 0.75f, -0.72f);
        p->shape(cx - 13.f, cy - 1.4f, cx + 13.f, cy + 1.4f, 1.4f, kCoral, kCoral, 0.f,
                 {0, 0, 0, 0}, 0.75f, -0.72f);
    };

    // Wi-Fi：四根递增的圆头信号条
    const float bar_w = 3.5f, bar_gap = 2.5f;
    const float wx1 = (float)screen_w - 18.f;
    const float wx0 = wx1 - (bar_w * 4.f + bar_gap * 3.f);
    static const float kBarH[4] = {6.f, 10.f, 14.f, 18.f};
    for (int i = 0; i < 4; ++i) {
        const float bx = wx0 + i * (bar_w + bar_gap);
        p->fill(bx, bottom, bx + bar_w, bottom + kBarH[i], bar_w * 0.5f,
                wifi_online ? p->ink : off);
    }
    if (!wifi_online) slash(wx0, wx1);

    // 电脑：圆角屏框 + 底座
    const float px1 = wx0 - 12.f, px0 = px1 - 22.f;
    const Color pc = pc_online ? p->ink : off;
    const Color none = withAlpha(pc, 0.f);
    p->shape(px0, bottom + 4.f, px1, top, 3.5f, none, none, 2.f, pc);
    p->fill(px0 + 5.f, bottom, px1 - 5.f, bottom + 2.f, 1.f, pc);
    if (!pc_online) slash(px0, px1);
}

void TaskPanel::renderAlertEdge(int screen_w, int screen_h) {
    auto* p = impl_;
    if (screen_w <= 0 || screen_h <= 0) return;
    p->begin(screen_w, screen_h);
    const float W = (float)screen_w, H = (float)screen_h;
    const float k = 0.5f + 0.5f * std::sin(p->now() * 2.f * kPi / 1.4f);
    const Color c = withAlpha(kCoral, 0.16f + 0.26f * k);
    p->glow(W * 0.5f, H, W * 0.75f, 64.f, c);
    p->glow(W * 0.5f, 0.f, W * 0.75f, 64.f, c);
    p->glow(0.f, H * 0.5f, 56.f, H * 0.75f, c);
    p->glow(W, H * 0.5f, 56.f, H * 0.75f, c);
}

void TaskPanel::renderDim(int brightness, int screen_w, int screen_h) {
    if (brightness >= 100 || screen_w <= 0 || screen_h <= 0) return;
    impl_->begin(screen_w, screen_h);
    const int b = brightness < 10 ? 10 : brightness;
    impl_->fill(-2.f, -2.f, screen_w + 2.f, screen_h + 2.f, 0.f,
                rgb(0x000000, 1.0f - (float)b / 100.0f));
}

}  // namespace dutyon

#endif  // !_WIN32 || DUTYON_UI_PREVIEW
