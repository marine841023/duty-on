// 设备端界面离屏预览：在 Windows 上用桌面 OpenGL 编译同一份 task_panel.cpp，
// 把各个画面渲染成 PNG，改界面时不用每次都烧到设备上看。
//
// 用法：ui_preview <正文字体> <时钟字体> <输出目录> [角色图片]
// 布局部分与 main.cpp 的设备端分支保持一致（改那边时同步这里）。

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include <cstdio>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "ui/i18n.h"
#include "ui/task_panel.h"

using namespace dutyon;

namespace {

GLuint g_char_tex = 0;
int g_char_w = 0, g_char_h = 0;

// 角色占位：把图片按"贴底、留边"画进角色视口（固定管线即可）
void drawCharacter(int W, int H, int vx, int vy, int vw, int vh) {
    if (!g_char_tex) return;
    const float scale = std::min(vw / (float)g_char_w, vh / (float)g_char_h) * 0.92f;
    const float w = g_char_w * scale, h = g_char_h * scale;
    const float x0 = vx + (vw - w) * 0.5f, y0 = vy + (vh - h) * 0.5f;
    glViewport(0, 0, W, H);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, W, 0, H, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, g_char_tex);
    glColor4f(1, 1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 1);
    glVertex2f(x0, y0);
    glTexCoord2f(1, 1);
    glVertex2f(x0 + w, y0);
    glTexCoord2f(1, 0);
    glVertex2f(x0 + w, y0 + h);
    glTexCoord2f(0, 0);
    glVertex2f(x0, y0 + h);
    glEnd();
    glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
}

struct Scene {
    std::string name;
    int W = 480, H = 800;
    std::string screen = "normal";  // normal / provision / joining / pairing
    std::string mode = "multi";     // multi / single / frame / voice
    PetStatus status;
    bool pc_ready = true, wifi = true;
    std::string theme = "amber";
    // 引导画面
    std::string failure;
    bool waiting = false, wifi_just_ok = false;
    // 语音
    std::vector<std::string> voice_cmds;
    int voice_highlight = -1;
    std::string voice_hint;
    int brightness = 100;
};

SessionInfo session(const char* name, const char* status, const char* ide) {
    SessionInfo s;
    s.project_name = name;
    s.status = status;
    s.ide = ide;
    return s;
}

void finish(PetStatus& st) {
    st.overall_state = "sleeping";
    for (const auto& s : st.sessions) {
        if (s.status == "confirmation-needed") {
            st.overall_state = "alert";
            break;
        }
        if (s.status != "idle") st.overall_state = "working";
    }
    st.session_count = (int)st.sessions.size();
}

void renderScene(TaskPanel& ui, const Scene& sc) {
    const int W = sc.W, H = sc.H;
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ui.setClockColor(sc.theme);

    if (sc.screen == "provision") {
        ui.renderProvision("DutyOn-7F3A", "k7m2x9qd", sc.failure, W, H);
    } else if (sc.screen == "joining") {
        ui.renderJoining("第 1 次尝试，还剩 48 秒", W, H);
    } else if (sc.screen == "pairing") {
        ui.renderPairing("482915", sc.waiting, sc.wifi_just_ok, W, H);
    } else {
        // ---- 与 main.cpp 设备端布局一致 ----
        const bool landscape = W >= H;
        const int left_w = landscape ? H : W;
        const int region_x = landscape ? left_w : 0;
        const int region_w = landscape ? W - left_w : W;
        const bool multi = sc.mode == "multi", voice = sc.mode == "voice";
        const bool large = !multi && !voice;
        const float header_h = ui.headerHeight(large, landscape);
        float panel_h = 0.f;
        if (voice)
            panel_h = ui.voicePanelHeight((int)sc.voice_cmds.size());
        else if (multi)
            panel_h =
                TaskPanel::heightForSessions(sc.pc_ready ? (int)sc.status.sessions.size() : 0);
        int vx = 0, vy = 0, vw = W, vh = H;
        if (landscape) {
            vw = left_w;
        } else if (multi || voice) {
            vy = (int)panel_h;
            vh = H - (int)header_h - (int)panel_h;
        }
        ui.renderBackdrop(sc.pc_ready ? sc.status.overall_state : "sleeping", W, H, vx, vy, vw, vh);
        drawCharacter(W, H, vx, vy, vw, vh);
        if (sc.pc_ready && sc.status.overall_state == "alert") ui.renderAlertEdge(W, H);
        glViewport(0, 0, W, H);

        const float below_header = (float)H - header_h;
        if (multi && sc.pc_ready)
            ui.render(sc.status, W, H, landscape ? below_header : panel_h, region_x, region_w);
        if (large && sc.mode == "single" && sc.pc_ready)
            ui.renderFocus(sc.status, W, H, landscape ? below_header - 8.f : 150.f, region_x,
                           region_w);
        if (voice)
            ui.renderVoicePanel(sc.voice_cmds, sc.voice_highlight, sc.voice_hint, W, H, panel_h,
                                region_x, region_w);
        ui.renderHeader("14:52:07", "11月12日", "星期四", large, landscape, W, H, region_x,
                        region_w);
    }
    ui.renderNetStatus(sc.wifi, sc.pc_ready, W, H);
    ui.renderDim(sc.brightness, W, H);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: ui_preview <font> <clock-font> <out-dir> [character-image]\n");
        return 2;
    }
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "ui-preview", nullptr, nullptr);
    if (!win) return 1;
    glfwMakeContextCurrent(win);
    if (glewInit() != GLEW_OK) return 1;

    if (argc > 4) {
        int n = 0;
        stbi_uc* px = stbi_load(argv[4], &g_char_w, &g_char_h, &n, 4);
        if (px) {
            glGenTextures(1, &g_char_tex);
            glBindTexture(GL_TEXTURE_2D, g_char_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_char_w, g_char_h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, px);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            stbi_image_free(px);
        }
    }

    I18n::setLang("zh-CN");
    TaskPanel ui;
    if (!ui.init(argv[1], argv[2])) fprintf(stderr, "font load failed: %s\n", argv[1]);

    std::vector<Scene> scenes;
    auto add = [&](Scene s) {
        finish(s.status);
        scenes.push_back(s);
        // 每个竖屏场景同时出一张横屏
        s.name += "-landscape";
        s.W = 800;
        s.H = 480;
        scenes.push_back(s);
    };
    {
        Scene s;
        s.name = "multi-working";
        s.status.sessions = {
            session("traeSprite", "tool-use", "claude"), session("ndmp", "thinking", "trae"),
            session("megrez-cloud", "working", "cursor"), session("docs-site", "idle", "codex")};
        add(s);
        s.name = "multi-alert";
        s.theme = "ice";
        s.status.sessions[1].status = "confirmation-needed";
        add(s);
    }
    {
        Scene s;
        s.name = "multi-idle";
        s.theme = "green";
        s.status.sessions = {session("traeSprite", "idle", "claude"),
                             session("一个名字特别长的中文项目名称用来测试截断", "idle", "qoder")};
        add(s);
        s.name = "multi-empty";
        s.theme = "pink";
        s.status.sessions.clear();
        add(s);
        s.name = "multi-overflow";
        s.theme = "white";
        for (int i = 0; i < 9; ++i)
            s.status.sessions.push_back(session(("project-" + std::to_string(i + 1)).c_str(),
                                                i == 7       ? "confirmation-needed"
                                                : i % 3 == 0 ? "thinking"
                                                : i % 3 == 1 ? "tool-use"
                                                             : "idle",
                                                i % 2 ? "opencode" : "trae"));
        add(s);
    }
    {
        Scene s;
        s.name = "single-working";
        s.mode = "single";
        s.status.sessions = {session("traeSprite", "tool-use", "claude")};
        add(s);
        s.name = "single-alert";
        s.status.sessions[0].status = "confirmation-needed";
        add(s);
        s.name = "frame-offline";
        s.mode = "frame";
        s.pc_ready = false;
        s.brightness = 70;
        add(s);
    }
    {
        Scene s;
        s.name = "voice-standby";
        s.mode = "voice";
        s.voice_hint = I18n::t("voice.wake");
        add(s);
        s.name = "voice-session";
        s.voice_cmds = {"开心", "生气", "害怕", "惊讶", "搞怪", "喜欢", "再见", "难过", "哈欠"};
        s.voice_highlight = 5;
        s.voice_hint = I18n::t("voice.hint");
        add(s);
    }
    {
        Scene s;
        s.screen = "provision";
        s.wifi = false;
        s.pc_ready = false;
        s.name = "provision";
        add(s);
        s.name = "provision-failed";
        s.failure = "Wi-Fi 密码不对，请重新输入";
        add(s);
        s.failure.clear();
        s.screen = "joining";
        s.name = "joining";
        add(s);
        s.screen = "pairing";
        s.wifi = true;
        s.name = "pairing";
        s.wifi_just_ok = true;
        add(s);
        s.name = "pairing-waiting";
        s.wifi_just_ok = false;
        s.waiting = true;
        s.theme = "ice";
        add(s);
    }

    for (const Scene& sc : scenes) {
        GLuint fbo = 0, tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sc.W, sc.H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        renderScene(ui, sc);
        std::vector<unsigned char> px((size_t)sc.W * sc.H * 4);
        glReadPixels(0, 0, sc.W, sc.H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
        stbi_flip_vertically_on_write(1);
        const std::string path = std::string(argv[3]) + "/" + sc.name + ".png";
        stbi_write_png(path.c_str(), sc.W, sc.H, 4, px.data(), sc.W * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        printf("wrote %s\n", path.c_str());
    }
    glfwTerminate();
    return 0;
}
