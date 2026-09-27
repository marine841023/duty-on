// 电子相框照片播放器实现（仅设备端）。见 photo_player.h 顶部设计说明。

#ifndef _WIN32

#include "render/photo_player.h"

#include <GLES3/gl3.h>
#include <stb_image.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>

namespace dutyon {

namespace {

// 正交 2D 贴图（GLES2 语法；Y 向下：uv 的 v=0 是图像首行=屏幕顶部，与
// stb 自顶向下的行序天然一致，无需翻转）。同 gif_sprite 设备版。
const char* kPhotoVertSrc =
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

const char* kPhotoFragSrc =
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
        fprintf(stderr, "[PhotoPlayer] shader compile: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

constexpr float kPhotoSeconds = 5.0f;  // 每张停留时长

}  // namespace

PhotoPlayer::PhotoPlayer() = default;

PhotoPlayer::~PhotoPlayer() { stop(); }

void PhotoPlayer::setFetcher(std::function<bool(std::vector<unsigned char>&)> f) {
    fetcher_ = std::move(f);
}

void PhotoPlayer::start() {
    if (running_.load()) return;
    quit_.store(false);
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        pending_.clear();
        pending_ready_ = false;
        want_fetch_ = true;  // 立即请求第一张
    }
    q_cv_.notify_all();
    running_.store(true);
    worker_ = std::thread(&PhotoPlayer::workerMain, this);
}

void PhotoPlayer::stop() {
    if (running_.load()) {
        quit_.store(true);
        q_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        running_.store(false);
    }
    // 释放当前纹理（调用方在主循环内 = GL 上下文当前，安全）
    if (tex_) {
        glDeleteTextures(1, &tex_);
        tex_ = 0;
    }
    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        pending_.clear();
        pending_ready_ = false;
        want_fetch_ = false;
    }
    tex_w_ = tex_h_ = 0;
    have_cur_ = false;
    timer_ = 0.f;
}

void PhotoPlayer::workerMain() {
    while (!quit_.load()) {
        std::vector<unsigned char> bytes;
        bool have_job = false;
        {
            std::unique_lock<std::mutex> lk(q_mtx_);
            q_cv_.wait(lk, [&] {
                return quit_.load() || want_fetch_;
            });
            if (quit_.load()) return;
            if (want_fetch_) {
                want_fetch_ = false;
                have_job = true;
            }
        }
        if (!have_job) continue;
        // 取字节在锁外执行（网络可能耗时，不能占着 mailbox 锁）
        bool ok = fetcher_ && fetcher_(bytes);
        if (ok && !quit_.load()) {
            std::lock_guard<std::mutex> lk(q_mtx_);
            if (!pending_ready_) {  // 单槽：渲染线程还没取走上一个则不覆盖
                pending_ = std::move(bytes);
                pending_ready_ = true;
            }
        } else if (!ok) {
            // 失败（PC 离线 / 无照片 / 传输错误）：退避后重试，避免空转刷网络
            for (int i = 0; i < 30 && !quit_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            std::lock_guard<std::mutex> lk(q_mtx_);
            if (!pending_ready_) want_fetch_ = true;  // 仍无就绪图 -> 再要一次
        }
    }
}

bool PhotoPlayer::takePendingToTexture() {
    std::vector<unsigned char> bytes;
    {
        std::lock_guard<std::mutex> lk(q_mtx_);
        if (!pending_ready_) return false;
        bytes.swap(pending_);
        pending_ready_ = false;
    }
    int w = 0, h = 0, ch = 0;
    unsigned char* rgba = stbi_load_from_memory(
        bytes.data(), (int)bytes.size(), &w, &h, &ch, 4);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) stbi_image_free(rgba);
        fprintf(stderr, "[PhotoPlayer] decode failed: %s\n",
                stbi_failure_reason());
        return false;  // 解不开：丢弃，等下一张
    }
    if (!tex_) {
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, tex_);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                 rgba);
    glBindTexture(GL_TEXTURE_2D, 0);
    stbi_image_free(rgba);
    tex_w_ = w;
    tex_h_ = h;
    have_cur_ = true;
    return true;
}

void PhotoPlayer::update(float delta_seconds) {
    if (!have_cur_) {
        // 首张：mailbox 一有货立刻上屏（不等满 5s），随后请求下一张
        if (takePendingToTexture()) {
            timer_ = 0.f;
            std::lock_guard<std::mutex> lk(q_mtx_);
            want_fetch_ = true;
            q_cv_.notify_all();
        }
        return;
    }
    timer_ += delta_seconds;
    if (timer_ >= kPhotoSeconds) {
        // 到点：下一张就绪则切换；未就绪则保持当前（不留白），下帧再看
        if (takePendingToTexture()) {
            timer_ = 0.f;
            std::lock_guard<std::mutex> lk(q_mtx_);
            want_fetch_ = true;
            q_cv_.notify_all();
        }
    }
}

void PhotoPlayer::ensureProgram() {
    if (program_) return;
    GLuint vs = compileShader(GL_VERTEX_SHADER, kPhotoVertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, kPhotoFragSrc);
    if (!vs || !fs) return;
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glBindAttribLocation(program_, 0, "a_pos");
    glBindAttribLocation(program_, 1, "a_uv");
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        fprintf(stderr, "[PhotoPlayer] link: %s\n", log);
        glDeleteProgram(program_);
        program_ = 0;
    }
}

void PhotoPlayer::render(int vp_x, int vp_y, int vp_w, int vp_h) {
    if (!have_cur_ || !tex_ || vp_w <= 0 || vp_h <= 0) return;
    ensureProgram();
    if (!program_) return;
    vp_x_ = vp_x;
    vp_y_ = vp_y;
    vp_w_ = vp_w;
    vp_h_ = vp_h;

    // cover 适配：等比放大到铺满视口，居中；溢出部分落在视口外，由 NDC
    // 裁剪自动切掉（无需 scissor）。Y 向下坐标系（原点视口左上）
    const float scale =
        (float)vp_w / (float)tex_w_ > (float)vp_h / (float)tex_h_
            ? (float)vp_w / (float)tex_w_
            : (float)vp_h / (float)tex_h_;
    const float dw = (float)tex_w_ * scale;
    const float dh = (float)tex_h_ * scale;
    const float dx = ((float)vp_w - dw) * 0.5f;  // 可为负 = 左溢出
    const float dy = ((float)vp_h - dh) * 0.5f;

    glViewport(vp_x, vp_y, vp_w, vp_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const float verts[] = {
        dx,      dy,      0.0f, 0.0f,
        dx + dw, dy,      1.0f, 0.0f,
        dx + dw, dy + dh, 1.0f, 1.0f,
        dx,      dy + dh, 0.0f, 1.0f,
    };

    glUseProgram(program_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_);
    glUniform1i(glGetUniformLocation(program_, "u_tex"), 0);
    glUniform2f(glGetUniformLocation(program_, "u_screen"), (float)vp_w,
                (float)vp_h);
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

}  // namespace dutyon

#endif  // !_WIN32
