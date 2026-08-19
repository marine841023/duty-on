// 2.0 原生 UI 层实现
//
// PC (Windows): ImGui + GLFW + OpenGL3 后端
// ARM Linux: 占位实现（后续可用更轻量方案，如 fbdev 文本）

#include "ui/ui_renderer.h"
#include "api/client.h"

#include <cstdio>

#ifdef _WIN32
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#endif

namespace dutyon {

// 格式化字节速率为易读字符串
static void FormatRate(unsigned long long bytes, char* out, size_t len) {
    if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(out, len, "%.1f GB/s", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024)
        snprintf(out, len, "%.1f MB/s", bytes / (1024.0 * 1024));
    else if (bytes >= 1024ULL)
        snprintf(out, len, "%.0f KB/s", bytes / 1024.0);
    else
        snprintf(out, len, "%llu B/s", bytes);
}

static void FormatBytes(unsigned long long bytes, char* out, size_t len) {
    if (bytes >= 1024ULL * 1024 * 1024)
        snprintf(out, len, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024ULL * 1024)
        snprintf(out, len, "%.0f MB", bytes / (1024.0 * 1024));
    else
        snprintf(out, len, "%llu KB", bytes / 1024);
}

struct UIRenderer::Impl {
#ifdef _WIN32
    GLFWwindow* window = nullptr;
    bool imgui_ready = false;
#endif
    int disp_w = 0, disp_h = 0;
};

UIRenderer::UIRenderer() : impl_(new Impl()) {}
UIRenderer::~UIRenderer() { shutdown(); delete impl_; }

bool UIRenderer::init(GLFWwindow* window) {
#ifdef _WIN32
    impl_->window = window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.WindowBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.14f, 0.85f);

    ImGui_ImplGlfw_InitForOpenGL(window, false);  // 不接管事件（我们自己的回调）
    ImGui_ImplOpenGL3_Init("#version 330 core");
    impl_->imgui_ready = true;
    printf("[UI] ImGui initialized (Windows)\n");
    return true;
#else
    (void)window;
    return false;
#endif
}

bool UIRenderer::init(int display_w, int display_h) {
    impl_->disp_w = display_w;
    impl_->disp_h = display_h;
    printf("[UI] headless overlay (%dx%d)\n", display_w, display_h);
    return true;
}

void UIRenderer::beginFrame() {
#ifdef _WIN32
    if (impl_->imgui_ready) {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }
#endif
}

void UIRenderer::renderMetrics(const SysMetrics& m) {
#ifdef _WIN32
    if (!impl_->imgui_ready || !showMetrics) return;

    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoNav;

    if (ImGui::Begin("##metrics", nullptr, flags)) {
        ImGui::Text("CPU  %5.1f%%", m.cpu_usage);

        char buf[64];
        FormatBytes(m.mem_used, buf, sizeof(buf));
        char total[64];
        FormatBytes(m.mem_total, total, sizeof(total));
        ImGui::Text("RAM  %s / %s", buf, total);

        if (m.has_gpu) {
            ImGui::Text("GPU  %5.1f%%", m.gpu_usage);
            FormatBytes(m.vram_used, buf, sizeof(buf));
            FormatBytes(m.vram_total, total, sizeof(total));
            ImGui::Text("VRAM %s / %s", buf, total);
        } else {
            ImGui::Text("GPU  --");
        }

        char rx[64], tx[64];
        FormatRate(m.net_rx_rate, rx, sizeof(rx));
        FormatRate(m.net_tx_rate, tx, sizeof(tx));
        ImGui::Text("NET  ↓ %s  ↑ %s", rx, tx);

        FormatBytes(m.self_mem, buf, sizeof(buf));
        ImGui::Text("DutyOn  %.1f%%  %s", m.self_cpu, buf);
    }
    ImGui::End();
#else
    (void)m;
#endif
}

void UIRenderer::renderStatus(const PetStatus& s) {
#ifdef _WIN32
    if (!impl_->imgui_ready || !showStatusBar) return;

    ImGui::SetNextWindowPos(ImVec2(10, impl_->disp_h - 60.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.7f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoFocusOnAppearing;

    if (ImGui::Begin("##status", nullptr, flags)) {
        const char* label = "空闲";
        ImVec4 color = ImVec4(0.48f, 0.62f, 1.0f, 1.0f);  // 蓝
        if (s.overall_state == "working") {
            label = "忙碌";
            color = ImVec4(1.0f, 0.78f, 0.2f, 1.0f);      // 黄
        } else if (s.overall_state == "alert") {
            label = "待确认";
            color = ImVec4(1.0f, 0.4f, 0.4f, 1.0f);       // 红
        }
        ImGui::TextColored(color, "● %s", label);
        if (s.session_count > 0) {
            ImGui::SameLine();
            ImGui::Text(" (%d 会话)", s.session_count);
        }
    }
    ImGui::End();
#else
    (void)s;
#endif
}

void UIRenderer::endFrame() {
#ifdef _WIN32
    if (impl_->imgui_ready) {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
#endif
}

void UIRenderer::shutdown() {
#ifdef _WIN32
    if (impl_ && impl_->imgui_ready) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        impl_->imgui_ready = false;
    }
#endif
}

} // namespace dutyon
