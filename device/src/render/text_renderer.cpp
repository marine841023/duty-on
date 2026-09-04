#ifndef _WIN32  // 仅设备端（ARM Linux GLES）

#include "render/text_renderer.h"

#define STB_TRUETYPE_IMPLEMENTATION  // ui_renderer.cpp 的同名定义在 _WIN32 段，
#include <stb_truetype.h>            // 与本文件互斥编译，无重复定义

#include <GLES3/gl3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace dutyon {

namespace {

// 正交 2D 贴图（GLES2 语法；图集仅用 alpha 通道，颜色走 uniform）
const char* kVertSrc =
    "attribute vec2 a_pos;\n"
    "attribute vec2 a_uv;\n"
    "uniform vec2 u_screen;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "  vec2 ndc = a_pos / u_screen * 2.0 - 1.0;\n"
    "  gl_Position = vec4(ndc, 0.0, 1.0);\n"
    "  v_uv = a_uv;\n"
    "}\n";

const char* kFragSrc =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform vec4 u_color;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "  gl_FragColor = vec4(u_color.rgb, u_color.a * texture2D(u_tex, v_uv).a);\n"
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
        fprintf(stderr, "[TextRenderer] shader compile: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

// UTF-8 解码：返回码位并前移指针；非法字节按 U+FFFD 处理（单字节步进防死循环）
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

} // namespace

struct TextRenderer::Impl {
    std::vector<unsigned char> font_buf;  // stbtt_fontinfo 只存指针，须同生命周期
    stbtt_fontinfo font;
    bool loaded = false;

    GLuint tex = 0;
    GLuint program = 0;

    static constexpr int kAtlas = 1024;
    std::vector<unsigned char> atlas_cpu;  // RGBA 副本（增量更新用）
    int shelf_x = 1, shelf_y = 1, shelf_h = 0;  // 货架式排布（1px 边距防渗色）

    struct Glyph {
        float dx0, dx1;   // 相对 pen 的 x 偏移（位图盒）
        float dy_top, dy_bottom;  // 相对基线的 GL 纵向偏移（上正下负）
        float advance;
        float u0, v0, u1, v1;  // 图集 UV（v0 = 顶边）
        bool has_bitmap;
    };
    // key = (码位 << 8) | 整数字号；实际字号只有两三档，按整数桶缓存
    std::map<unsigned long long, Glyph> cache;

    std::vector<float> verts;  // 绘制暂存（pos2 + uv2）

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
            fprintf(stderr, "[TextRenderer] link: %s\n", log);
            glDeleteProgram(program);
            program = 0;
        }
    }

    void ensureAtlas() {
        if (tex) return;
        atlas_cpu.assign((size_t)kAtlas * kAtlas * 4, 0);
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kAtlas, kAtlas, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, atlas_cpu.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // 图集写满：整板重建（字形按需再烘焙；实际极少触发）
    void resetAtlas() {
        cache.clear();
        shelf_x = shelf_y = 1;
        shelf_h = 0;
        std::fill(atlas_cpu.begin(), atlas_cpu.end(), 0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kAtlas, kAtlas, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, atlas_cpu.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        printf("[TextRenderer] atlas full — rebuilt\n");
    }

    const Glyph* bake(unsigned int cp, int size) {
        const unsigned long long key =
            ((unsigned long long)cp << 8) | (unsigned)(size & 0xFF);
        auto it = cache.find(key);
        if (it != cache.end()) return &it->second;

        const float scale = stbtt_ScaleForPixelHeight(&font, (float)size);
        const int gi = stbtt_FindGlyphIndex(&font, (int)cp);

        Glyph g{};
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&font, gi, &adv, &lsb);
        g.advance = adv * scale;

        int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
        stbtt_GetGlyphBitmapBox(&font, gi, scale, scale, &bx0, &by0, &bx1, &by1);
        const int bw = bx1 - bx0, bh = by1 - by0;
        if (gi == 0 || bw <= 0 || bh <= 0) {  // 缺字/空白：只走 advance
            g.has_bitmap = false;
            if (g.advance <= 0.0f) g.advance = size * 0.5f;
            return &cache.emplace(key, g).first->second;
        }

        ensureAtlas();
        if (shelf_x + bw + 1 >= kAtlas) {  // 换行
            shelf_x = 1;
            shelf_y += shelf_h + 1;
            shelf_h = 0;
        }
        if (shelf_y + bh + 1 >= kAtlas) {
            resetAtlas();
            return bake(cp, size);
        }

        // 位图烘焙到 CPU 图集（alpha 通道），再增量上传
        const int gx = shelf_x, gy = shelf_y;
        {
            std::vector<unsigned char> bmp((size_t)bw * bh, 0);
            stbtt_MakeGlyphBitmap(&font, bmp.data(), bw, bh, bw, scale, scale, gi);
            for (int row = 0; row < bh; ++row) {
                unsigned char* dst =
                    &atlas_cpu[(((size_t)(gy + row)) * kAtlas + gx) * 4];
                const unsigned char* src = &bmp[(size_t)row * bw];
                for (int col = 0; col < bw; ++col) {
                    dst[col * 4 + 0] = 255;
                    dst[col * 4 + 1] = 255;
                    dst[col * 4 + 2] = 255;
                    dst[col * 4 + 3] = src[col];
                }
            }
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        // 逐行上传：CPU 图集行宽为 kAtlas*4，而子图宽只有 bw ——
        // 直接一次传整块会按紧凑行距（bw*4）跨行读入相邻单元格的内存，
        // 字形变成垃圾点阵（GLES2 无 GL_UNPACK_ROW_LENGTH，逐行最稳）
        for (int row = 0; row < bh; ++row) {
            glTexSubImage2D(GL_TEXTURE_2D, 0, gx, gy + row, bw, 1, GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            &atlas_cpu[((size_t)(gy + row) * kAtlas + gx) * 4]);
        }
        glBindTexture(GL_TEXTURE_2D, 0);

        shelf_x += bw + 1;
        if (bh > shelf_h) shelf_h = bh;

        g.has_bitmap = true;
        g.dx0 = (float)bx0;
        g.dx1 = (float)bx1;
        g.dy_top = -(float)by0;     // by0 <= 0：位图顶在基线上方
        g.dy_bottom = -(float)by1;
        g.u0 = (float)gx / kAtlas;
        g.u1 = (float)(gx + bw) / kAtlas;
        g.v0 = (float)gy / kAtlas;          // 顶边
        g.v1 = (float)(gy + bh) / kAtlas;   // 底边
        return &cache.emplace(key, g).first->second;
    }
};

TextRenderer::TextRenderer() : impl_(new Impl) {}

TextRenderer::~TextRenderer() {
    if (impl_->tex) glDeleteTextures(1, &impl_->tex);
    if (impl_->program) glDeleteProgram(impl_->program);
    delete impl_;
}

bool TextRenderer::load(const std::string& font_path) {
    FILE* f = fopen(font_path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "[TextRenderer] font open failed: %s\n", font_path.c_str());
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return false;
    }
    impl_->font_buf.resize((size_t)sz);
    const size_t rd = fread(impl_->font_buf.data(), 1, (size_t)sz, f);
    fclose(f);
    if (rd != (size_t)sz) return false;
    if (!stbtt_InitFont(&impl_->font, impl_->font_buf.data(),
                        stbtt_GetFontOffsetForIndex(impl_->font_buf.data(), 0))) {
        fprintf(stderr, "[TextRenderer] font parse failed: %s\n", font_path.c_str());
        return false;
    }
    impl_->loaded = true;
    printf("[TextRenderer] font loaded: %s (%ld bytes)\n", font_path.c_str(), sz);
    return true;
}

bool TextRenderer::isLoaded() const { return impl_->loaded; }

float TextRenderer::measureWidth(const std::string& text_utf8,
                                 float pixel_size) const {
    if (!impl_->loaded) return 0.0f;
    const float scale = stbtt_ScaleForPixelHeight(&impl_->font, pixel_size);
    const char* p = text_utf8.data();
    const char* end = p + text_utf8.size();
    float pen = 0.0f;
    unsigned int prev = 0;
    while (p < end) {
        const unsigned int cp = decodeUtf8(p, end);
        const int gi = stbtt_FindGlyphIndex(&impl_->font, (int)cp);
        if (prev) pen += stbtt_GetGlyphKernAdvance(&impl_->font,
                                                   (int)prev, gi) * scale;
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(&impl_->font, gi, &adv, &lsb);
        pen += adv * scale;
        prev = gi;
    }
    return pen;
}

float TextRenderer::lineHeight(float pixel_size) const {
    if (!impl_->loaded) return pixel_size;
    int ascent = 0, descent = 0, lgap = 0;
    stbtt_GetFontVMetrics(&impl_->font, &ascent, &descent, &lgap);
    const float scale = stbtt_ScaleForPixelHeight(&impl_->font, pixel_size);
    return (float)(ascent - descent) * scale;
}

void TextRenderer::draw(const std::string& text_utf8, float x, float y,
                        float pixel_size, float r, float g, float b, float a,
                        int screen_w, int screen_h) {
    if (!impl_->loaded || text_utf8.empty() || screen_w <= 0 || screen_h <= 0)
        return;
    impl_->ensureProgram();
    if (!impl_->program) return;

    const int size = pixel_size < 1.0f ? 1 : (int)(pixel_size + 0.5f);
    int ascent = 0, descent = 0, lgap = 0;
    stbtt_GetFontVMetrics(&impl_->font, &ascent, &descent, &lgap);
    const float scale = stbtt_ScaleForPixelHeight(&impl_->font, (float)size);
    const float baseline = y - (float)ascent * scale;  // 左上锚点 -> 基线

    auto& v = impl_->verts;
    v.clear();
    v.reserve(text_utf8.size() * 6 * 4 / 2);
    float pen = x;
    unsigned int prev = 0;
    const char* p = text_utf8.data();
    const char* end = p + text_utf8.size();
    while (p < end) {
        const unsigned int cp = decodeUtf8(p, end);
        const Impl::Glyph* gl = impl_->bake(cp, size);
        if (prev) {
            const int prev_gi = stbtt_FindGlyphIndex(&impl_->font, (int)prev);
            const int cur_gi = stbtt_FindGlyphIndex(&impl_->font, (int)cp);
            pen += stbtt_GetGlyphKernAdvance(&impl_->font, prev_gi, cur_gi) * scale;
        }
        if (gl->has_bitmap) {
            const float x0 = pen + gl->dx0, x1 = pen + gl->dx1;
            const float yt = baseline + gl->dy_top, yb = baseline + gl->dy_bottom;
            // 两三角形（顶边取 v0、底边取 v1，文字正立）
            v.insert(v.end(), {x0, yb, gl->u0, gl->v1,
                               x1, yb, gl->u1, gl->v1,
                               x1, yt, gl->u1, gl->v0,
                               x0, yb, gl->u0, gl->v1,
                               x1, yt, gl->u1, gl->v0,
                               x0, yt, gl->u0, gl->v0});
        }
        pen += gl->advance;
        prev = cp;
    }
    if (v.empty()) return;

    glUseProgram(impl_->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, impl_->tex);
    glUniform1i(glGetUniformLocation(impl_->program, "u_tex"), 0);
    glUniform2f(glGetUniformLocation(impl_->program, "u_screen"),
                (float)screen_w, (float)screen_h);
    glUniform4f(glGetUniformLocation(impl_->program, "u_color"), r, g, b, a);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), v.data());
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          v.data() + 2);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(v.size() / 4));
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

void TextRenderer::drawBold(const std::string& text_utf8, float x, float y,
                            float pixel_size, float r, float g, float b,
                            float a, int screen_w, int screen_h, float stroke) {
    // 四向微偏移叠加：近似描边加粗（alpha 叠加自然平滑，无锯齿）
    draw(text_utf8, x - stroke, y, pixel_size, r, g, b, a, screen_w, screen_h);
    draw(text_utf8, x + stroke, y, pixel_size, r, g, b, a, screen_w, screen_h);
    draw(text_utf8, x, y - stroke, pixel_size, r, g, b, a, screen_w, screen_h);
    draw(text_utf8, x, y + stroke, pixel_size, r, g, b, a, screen_w, screen_h);
    draw(text_utf8, x, y, pixel_size, r, g, b, a, screen_w, screen_h);
}

} // namespace dutyon

#endif // !_WIN32
