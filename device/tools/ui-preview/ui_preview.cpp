// 设备端界面离屏预览：在 Windows 上用桌面 OpenGL 编译同一份 task_panel.cpp，
// 把各个画面渲染成 PNG，改界面时不用每次都烧到设备上看。
//
// 用法：ui_preview <正文字体> <时钟字体> <输出目录> [角色图片]
// 布局与正式程序直接共用 TaskPanel 的布局和绘制入口。

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
    bool secondary = false;
    SecondaryDisplaySettings display_settings;
    bool metrics_ready = true;
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

void renderSecondary(TaskPanel& ui, const Scene& sc) {
    const auto layout = secondaryDisplayLayout(sc.W, sc.H, sc.display_settings);
    const auto& pet = layout.pet;
    const auto screen = ui.screenLayout(sc.mode, pet.w, pet.h, (int)sc.status.sessions.size(), 0.f, true);
    auto viewport = [&](const DisplayRegion& region) {
        glViewport(region.x, sc.H - region.y - region.h, region.w, region.h);
        glScissor(region.x, sc.H - region.y - region.h, region.w, region.h);
    };
    glEnable(GL_SCISSOR_TEST);
    viewport(pet);
    ui.renderBackdrop(sc.status.overall_state, pet.w, pet.h, screen.character_x, screen.character_y,
                      screen.character_w, screen.character_h, true);
    drawCharacter(sc.W, sc.H, pet.x + screen.character_x, sc.H - pet.y - pet.h + screen.character_y,
                  screen.character_w, screen.character_h);
    viewport(pet);
    ui.renderScreen(sc.status, sc.mode, pet.w, pet.h, screen, 1794485527.0, 480);
    if (layout.metrics != SecondaryMetrics::Hidden) {
        SysMetrics metrics{};
        metrics.cpu_usage = 28.f;
        metrics.mem_total = 32;
        metrics.mem_used = 18;
        metrics.has_gpu = true;
        metrics.gpu_usage = 63.f;
        metrics.vram_total = 12;
        metrics.vram_used = 3;
        metrics.net_rx_rate = 13421772;
        metrics.net_tx_rate = 86016;
        viewport(layout.monitor);
        ui.renderSecondaryMetrics(sc.metrics_ready ? &metrics : nullptr, layout.metrics,
                                  layout.monitor.w, layout.monitor.h, layout.monitor.y > 0);
    }
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, sc.W, sc.H);
    ui.renderDim(sc.brightness, sc.W, sc.H);
}

void renderScene(TaskPanel& ui, const Scene& sc) {
    const int W = sc.W, H = sc.H;
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ui.setClockColor(sc.theme);

    if (sc.secondary) {
        renderSecondary(ui, sc);
        return;
    }
    if (sc.screen == "provision") {
        ui.renderProvision("DutyOn-7F3A", "k7m2x9qd", sc.failure, W, H);
    } else if (sc.screen == "joining") {
        ui.renderJoining("第 1 次尝试，还剩 48 秒", W, H);
    } else if (sc.screen == "pairing") {
        ui.renderPairing("482915", sc.waiting, sc.wifi_just_ok, W, H);
    } else {
        const bool voice = sc.mode == "voice";
        const float panel_h = voice ? ui.voicePanelHeight((int)sc.voice_cmds.size(), W, H) : 0.0f;
        const auto layout = ui.screenLayout(sc.mode, W, H,
            sc.pc_ready ? (int)sc.status.sessions.size() : 0, panel_h);
        ui.renderBackdrop(sc.pc_ready && sc.mode != "frame" ? sc.status.overall_state : "sleeping",
                          W, H, layout.character_x, layout.character_y,
                          layout.character_w, layout.character_h);
        drawCharacter(W, H, layout.character_x, layout.character_y,
                       layout.character_w, layout.character_h);
        glViewport(0, 0, W, H);
        if (voice)
            ui.renderVoicePanel(sc.voice_cmds, sc.voice_highlight, sc.voice_hint, W, H, panel_h,
                                layout.region_x, layout.region_w);
        ui.renderScreen(sc.status, sc.mode, W, H, layout, 1794485527.0, 480, true, sc.pc_ready);
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
        s.name += "-1280";
        s.W = 1280;
        s.H = 720;
        scenes.push_back(s);
        s.name += "-portrait";
        s.W = 720;
        s.H = 1280;
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

    {
        Scene s;
        s.secondary = true;
        s.status.sessions = {session("一个名字特别长的中文项目名称用来测试截断", "tool-use", "trae"),
                             session("wewins3", "confirmation-needed", "trae")};
        s.display_settings.show_metrics = true;
        finish(s.status);
        auto secondary = [&](const char* name, int w, int h) {
            s.name = name;
            s.W = w;
            s.H = h;
            scenes.push_back(s);
        };
        secondary("secondary-standard", 1280, 720);
        secondary("secondary-standard-hidpi", 2560, 1440);
        secondary("secondary-compact", 1600, 720);
        secondary("secondary-full", 1920, 720);
        secondary("secondary-half-left", 2560, 720);
        secondary("secondary-short", 1920, 480);
        secondary("secondary-square", 800, 800);
        secondary("secondary-portrait", 720, 1280);
        secondary("secondary-tall", 720, 1600);
        s.metrics_ready = false;
        secondary("secondary-awaiting-sample", 1920, 720);
        s.metrics_ready = true;
        s.display_settings.placement = SecondaryPlacement::Right;
        secondary("secondary-half-right", 2560, 720);
        s.display_settings.show_metrics = false;
        secondary("secondary-right-empty", 2560, 720);
        s.display_settings.placement = SecondaryPlacement::Auto;
        secondary("secondary-centered", 2560, 720);
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
