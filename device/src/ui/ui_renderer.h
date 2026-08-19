#pragma once

#include <string>

// 前向声明避免暴露 GLFW/ImGui 头文件到公共接口
struct GLFWwindow;

namespace dutyon {

struct PetStatus;
struct SysMetrics;

// 2.0 原生 UI 层：PC 端用 ImGui，设备端为纯文本占位。
// 覆盖在 Live2D 角色上方，面板半透明背景，其余区域透明。
class UIRenderer {
public:
    UIRenderer();
    ~UIRenderer();

    // PC 端初始化（GLFW 窗口句柄）
    bool init(GLFWwindow* window);
    // 设备端初始化（无窗口句柄）
    bool init(int display_w, int display_h);

    void beginFrame();

    // 显示系统监控面板
    void renderMetrics(const SysMetrics& m);

    // 显示当前状态（角色名称、状态文字）
    void renderStatus(const PetStatus& s);

    void endFrame();

    void shutdown();

    // 是否显示监控面板（右键菜单切换）
    bool showMetrics = true;
    bool showStatusBar = true;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace dutyon
