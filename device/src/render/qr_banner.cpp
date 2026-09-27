#ifndef _WIN32  // 仅设备端（ARM Linux GLES）

#include "render/qr_banner.h"

#include <GLES3/gl3.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "render/qrcodegen.h"

namespace dutyon {

namespace {

// 每模块像素数（纹理内），静区 4 模块。version 3(29) -> (29+8)*6 = 222px
const int kModulePx = 6;
const int kQuietModules = 4;

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
        fprintf(stderr, "[QrBanner] shader compile: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

struct QrBanner::Impl {
    GLuint tex = 0;
    GLuint program = 0;
    std::string payload;  // 已烘焙的文本（相同则不重建）

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
            fprintf(stderr, "[QrBanner] link: %s\n", log);
            glDeleteProgram(program);
            program = 0;
        }
    }
};

QrBanner::QrBanner() : impl_(new Impl) {}

QrBanner::~QrBanner() {
    if (impl_->tex) glDeleteTextures(1, &impl_->tex);
    if (impl_->program) glDeleteProgram(impl_->program);
    delete impl_;
}

bool QrBanner::setPayload(const std::string& text_utf8) {
    if (text_utf8.empty()) return false;
    if (impl_->tex && impl_->payload == text_utf8) return true;  // 已烘焙

    QrCode qr = QrCode::encodeText(text_utf8, QrCode::Ecc::Medium);
    const int n = qr.size();
    if (n <= 0) {
        fprintf(stderr, "[QrBanner] encode failed (too long?)\n");
        return false;
    }

    // 生成 RGBA：白底 + 黑模块（含 4 模块静区）
    const int dim = (n + kQuietModules * 2) * kModulePx;
    std::vector<uint8_t> img((size_t)dim * dim * 4, 0xFF);  // 全白不透明
    for (int my = 0; my < n; ++my)
        for (int mx = 0; mx < n; ++mx) {
            if (!qr.at(mx, my)) continue;  // 白模块留白
            const int px0 = (mx + kQuietModules) * kModulePx;
            const int py0 = (my + kQuietModules) * kModulePx;
            for (int dy = 0; dy < kModulePx; ++dy)
                for (int dx = 0; dx < kModulePx; ++dx) {
                    size_t off = ((size_t)(py0 + dy) * dim + (px0 + dx)) * 4;
                    img[off + 0] = 0;
                    img[off + 1] = 0;
                    img[off + 2] = 0;
                    img[off + 3] = 255;
                }
        }

    if (impl_->tex) glDeleteTextures(1, &impl_->tex);
    glGenTextures(1, &impl_->tex);
    glBindTexture(GL_TEXTURE_2D, impl_->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, dim, dim, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, img.data());
    // NEAREST：模块方块保持锐利（线性过滤会糊化边缘，影响扫码识别）
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    impl_->payload = text_utf8;
    printf("[QrBanner] built %dx%d (modules %d)\n", dim, dim, n);
    return true;
}

bool QrBanner::isReady() const { return impl_->tex != 0; }

void QrBanner::render(int screen_w, int screen_h, float fill,
                      float center_x, float center_y) {
    if (!impl_->tex || screen_w <= 0 || screen_h <= 0) return;
    impl_->ensureProgram();
    if (!impl_->program) return;
    if (fill <= 0.f || fill > 1.f) fill = 0.7f;

    // 正方形，边长 = 短边 * fill；默认屏幕中央，center_*>=0 时以其为中心
    //（u_screen 仍传整屏尺寸，保证 NDC 归一化正确，仅平移四边形）
    const float side = (float)(screen_w < screen_h ? screen_w : screen_h) * fill;
    const float cx = (center_x >= 0.f) ? center_x : (float)screen_w * 0.5f;
    const float cy = (center_y >= 0.f) ? center_y : (float)screen_h * 0.5f;
    const float x0 = cx - side * 0.5f;
    const float y0 = cy - side * 0.5f;
    const float x1 = x0 + side;
    const float y1 = y0 + side;

    // xy + uv（GL 原点左下，v 翻转使图像正立）
    const float verts[] = {
        x0, y0, 0.0f, 1.0f,
        x1, y0, 1.0f, 1.0f,
        x1, y1, 1.0f, 0.0f,
        x0, y1, 0.0f, 0.0f,
    };

    glUseProgram(impl_->program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, impl_->tex);
    glUniform1i(glGetUniformLocation(impl_->program, "u_tex"), 0);
    glUniform2f(glGetUniformLocation(impl_->program, "u_screen"),
                (float)screen_w, (float)screen_h);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), verts + 2);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
}

} // namespace dutyon

#endif // !_WIN32
