#ifndef _WIN32  // 仅设备端（ARM Linux GLES）

#include "render/prompt_banner.h"

#include <GLES3/gl3.h>
#include <stb_image.h>

#include <cstdio>
#include <cstdlib>

namespace dutyon {

namespace {

// 正交 2D 贴图（GLES2 语法，ES2/ES3 上下文均可）
const char* kVertSrc =
    "attribute vec2 a_pos;\n"      // 像素坐标（GL 原点左下）
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
        fprintf(stderr, "[PromptBanner] shader compile: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

} // namespace

struct PromptBanner::Impl {
    GLuint tex = 0;
    GLuint program = 0;
    int img_w = 0, img_h = 0;

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
            fprintf(stderr, "[PromptBanner] link: %s\n", log);
            glDeleteProgram(program);
            program = 0;
        }
    }
};

PromptBanner::PromptBanner() : impl_(new Impl) {}

PromptBanner::~PromptBanner() {
    if (impl_->tex) glDeleteTextures(1, &impl_->tex);
    if (impl_->program) glDeleteProgram(impl_->program);
    delete impl_;
}

bool PromptBanner::load(const std::string& path_utf8) {
    int w = 0, h = 0, ch = 0;
    unsigned char* data = stbi_load(path_utf8.c_str(), &w, &h, &ch, STBI_rgb_alpha);
    if (!data) {
        fprintf(stderr, "[PromptBanner] load failed: %s\n", path_utf8.c_str());
        return false;
    }
    if (impl_->tex) glDeleteTextures(1, &impl_->tex);
    glGenTextures(1, &impl_->tex);
    glBindTexture(GL_TEXTURE_2D, impl_->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    // 大图缩绘（1792x1024 -> ~440px）：双线性过滤即可，免 mipmap 生成开销
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(data);
    impl_->img_w = w;
    impl_->img_h = h;
    printf("[PromptBanner] loaded %dx%d\n", w, h);
    return true;
}

bool PromptBanner::isLoaded() const { return impl_->tex != 0; }

void PromptBanner::render(int screen_w, int screen_h) {
    if (!impl_->tex || screen_w <= 0 || screen_h <= 0) return;
    impl_->ensureProgram();
    if (!impl_->program) return;

    // 宽度占屏 92%（保持长宽比），底边距底部 5%
    const float draw_w = screen_w * 0.92f;
    const float draw_h = draw_w * (float)impl_->img_h / (float)impl_->img_w;
    const float x0 = (screen_w - draw_w) * 0.5f;
    const float y0 = screen_h * 0.05f;
    const float x1 = x0 + draw_w;
    const float y1 = y0 + draw_h;

    // xy + uv 交错（GL 原点左下，v 翻转使图像正立）
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
