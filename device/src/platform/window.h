#pragma once

namespace dutyon {

// 平台窗口抽象：Windows (GLFW+Win32) 和 ARM Linux (EGL/fbdev) 各自实现
// 统一的接口，main.cpp 按平台选择实现。
class IPlatformWindow {
public:
    virtual ~IPlatformWindow() = default;

    // 创建窗口和 GL 上下文
    virtual bool init(int width, int height) = 0;

    // 处理事件（拖拽、右键菜单、托盘），返回 false 表示应退出
    virtual bool pollEvents() = 0;

    // 交换缓冲
    virtual void swapBuffers() = 0;

    // 点击穿透开关（true = 鼠标穿透到下层窗口）
    virtual void setClickThrough(bool through) = 0;

    virtual bool isClickThrough() const = 0;

    virtual void shutdown() = 0;

    // 窗口尺寸
    virtual int width() const = 0;
    virtual int height() const = 0;

    // 平台原生句柄（PC 端返回 GLFWwindow*，设备端返回 nullptr）
    virtual void* nativeHandle() { return nullptr; }
};

// 按平台创建窗口实例（工厂函数，各平台 .cpp 实现）
IPlatformWindow* createPlatformWindow();

} // namespace dutyon
