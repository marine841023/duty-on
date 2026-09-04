#pragma once

#include <functional>
#include <string>
#include <vector>

// 前向声明避免暴露 GLFW/ImGui 头文件到公共接口
struct GLFWwindow;

namespace dutyon {

struct PetStatus;
struct SessionInfo;
struct SysMetrics;
struct Rect;

// 2.0 原生 UI 层：PC 端用 ImGui，设备端为纯文本占位。
// 覆盖在 Live2D 角色上方，面板半透明背景，其余区域透明。
//
// 布局对齐 1.x（styles.css #pet-container flex 列，自上而下）：
//   角色画布 240x260 → 状态栏(状态头+项目列表) → 系统监控面板
// 窗口高度随面板内容变化（底边固定向上生长，由主循环驱动）；
// 右键菜单为 ImGui 自绘（1.x #context-menu 样式），打开时窗口向右
// 临时扩展出菜单区域（主循环按 menuExtraWidth() 放宽窗口）。
class UIRenderer {
public:
    UIRenderer();
    ~UIRenderer();

    // PC 端初始化（GLFW 窗口句柄）
    bool init(GLFWwindow* window);
    // 设备端初始化（无窗口句柄）
    bool init(int display_w, int display_h);

    void beginFrame();

    // 新监控样本到达时压入历史缓冲（折线图数据源）；
    // 主循环仅在 takeMetrics() 拿到新数据时调用，约 1.5s 一次
    void pushMetrics(const SysMetrics& m);

    // 渲染系统监控面板（showMetrics 为 true 时；位于状态栏下方/窗口底部）
    void renderMetrics(const SysMetrics& m);

    // 渲染底部状态栏（状态头 + 项目列表；位于角色画布下方）
    void renderStatus(const PetStatus& s);

    // 渲染角色头顶状态特效（Zzz / ... / !，动画；锚定 setModelRect）
    void renderHeadEffect(const PetStatus& s);

    // 渲染右键菜单（isMenuOpen() 时；窗口右侧临时扩展区）
    void renderMenu();

    void endFrame();

    void shutdown();

    // 上一帧测得的面板高度（主循环据此动态调整窗口高度）
    float statusBarHeight() const;
    float monitorHeight() const;

    // 菜单打开时窗口需要在常规宽度外向右扩展的像素数（0 = 菜单关闭）
    float menuExtraWidth() const;

    // 常规内容区逻辑宽度（240，菜单不算）；窗口总宽 = 240 + 边距
    // 由主循环换算物理像素
    static constexpr float kPanelW = 240.0f;

    // DPI 缩放（主循环的边距/尺寸换算用，与字体一致）
    float dpiScale() const;

    // ---- 头顶特效锚定：主循环每帧把模型内容包围盒（窗口客户区坐标，
    // Y 向下）传入；迷你模式下传迷你画布内的包围盒 ----
    void setModelRect(const Rect& r, bool tight_bounds = true);

    // ---- 布局参数注入（主循环在 init / mini 切换后调用；物理像素）----
    // canvas_h 角色区高；panel_x/panel_w 面板左边与宽；panel_gap 面板间距
    void setLayout(float canvas_h, float panel_x, float panel_w, float panel_gap,
                   bool mini);

    // ---- 显隐开关（显示隐藏菜单切换；持久化由 main 处理）----
    bool showMetrics = true;    // 系统监控面板整体
    bool showProjects = true;   // 状态栏里的项目列表
    bool showCpu = true;
    bool showRam = true;
    bool showGpu = true;
    bool showNet = true;
    bool showSelf = true;
    bool monitorCollapsed = false;  // 监控面板收起（只留标题行）

    // ---- 右键菜单（ImGui 自绘，结构对齐 1.x index.html）----
    void openMenu();     // 右键角色 / 状态栏 ☰ 按钮
    void closeMenu();
    // 调试：按名称直接打开指定视图（main 的 DUTYON_MENU_VIEW 用；
    // 合法值 main/models/motion-play/motion-assign/settings/language/visibility）
    void openMenuView(const std::string& view);
    bool isMenuOpen() const;
    // 菜单处于「播放动作 / 选择动作」视图（主循环据此维持/结束动作预览循环）
    bool isMotionViewActive() const;
    // 语言切换后重建字体图集（ja/ko/zh-TW 需要不同字形范围）；
    // 在帧中调用安全，实际重建推迟到本帧渲染完成后
    void reloadFonts();
    // 动态更新缩放基准（跨显示器拖动后分辨率/DPI 变化）：
    // 更新布局 scale 并在帧末按新 scale 重建字体图集
    void setScale(float s);
    // 菜单展开方向：false 向右扩（默认）/ true 向左扩（右侧屏幕空间不足，
    // 对齐 1.x menu-left 模式：菜单贴窗口左缘、角色区右锚）
    void setMenuLeft(bool left);
    // 硬件显示端状态（主循环每帧注入）：在线时菜单显示"设备模式"分组
    //（单任务/多任务/电子相框三选一，menu_activate 收 "device-mode:<m>"）
    void setDeviceStatus(bool online, const std::string& mode) {
        device_online_ = online;
        device_mode_ = mode;
    }
    // 客户区坐标是否落在菜单矩形内（右键菜单区域时不触发开/关切换）
    bool isPointInMenu(float x, float y) const;
    // 客户区坐标是否落在「可点击内容」上（1.x setupClickThrough 的
    // clickable regions：模型 bounds（GIF=整个画布）+ 状态栏 + 监控面板。
    // 不含菜单——菜单区由调用方按 isPointInMenu 另判。
    // 主循环用它驱动点击穿透与拖拽区域测试；坐标为物理像素）
    bool isPointClickable(float x, float y) const;
    // 可点击区域矩形列表（对齐 1.x collectRegions：模型 bounds + 状态栏 +
    // 监控面板 + 菜单矩形）；主循环每帧上报窗口层做穿透判定（独立线程轮询）
    std::vector<Rect> clickRegions() const;

    // 菜单动态条目（切换形象 / 动作列表，main 注入）
    struct MenuEntry {
        std::string id;      // 激活 id（model:<key> / motion:<g>:<i> / assign:<g>:<i>）
        std::string label;   // 显示名
        bool checked = false;
        std::string thumb;   // 缩略图图片文件路径（空 = 首字母占位；
                             // PNG / GIF 均可，stb 解码取首帧）
    };
    // key: "models"=切换形象列表；"motions"=播放动作；"motions:<状态>"=动作设定
    std::function<std::vector<MenuEntry>(const std::string&)> menu_collect;
    // 勾选/激活态：flip / mini / autostart / vis-* / lang:<代码>
    std::function<bool(const std::string&)> menu_is_checked;
    // 右侧提示文字：hook-status（已安装/未安装/未检查）、assign:<状态>（当前动作名）
    std::function<std::string(const std::string&)> menu_hint;
    // 执行菜单项（open-models-dir / install-hooks / hook-status / quit /
    // model:<key> / motion:<g>:<i> / assign:<状态>:<g>:<i> / preview:g:i /
    // flip / mini / autostart / lang:<代码> / vis-* / preview-alert）
    std::function<void(const std::string&)> menu_activate;
    // 菜单打开时触发（main 刷新 autostart / hook 状态缓存，避免每帧 HTTP）
    std::function<void()> on_menu_open;
    // 监控面板内部操作（↺ 恢复默认 / ▾ 收起）后的持久化回调（main 注入）
    std::function<void(const std::string&)> on_monitor_action;  // "reset"/"collapse"

    // 预览提醒效果（动作设定 → 预览提醒效果：头顶 ! 特效 + 状态栏闪红 3s）
    void previewAlert();

    // ---- 鼠标事件转发（窗口层 -> ImGui；点击穿透到面板前先喂给 ImGui）----
    // 由窗口层在 GLFW 鼠标回调里调用；窗口销毁前置空
    void forwardMouseButton(int button, int action, int mods);
    void forwardCursorPos(double x, double y);
    // 滚轮转发（菜单动作列表滚动）
    void forwardScroll(double x, double y);
    // 键盘转发（Esc 关菜单）
    void forwardKey(int key, int scancode, int action, int mods);

    // 项目行点击（对应 1.x bringToFront：把该 IDE 窗口前置）
    std::function<void(const SessionInfo&)> on_project_click;

    // ---- 边缘吸附条（1.x #edge-dock-bar：窗口贴屏幕左/右缘的 40px 细条）----
    // 整窗绘制：顶部状态灯（最紧急状态色）+ 项目两字徽标（点击前置 IDE）。
    // 主循环在 isEdgeDocked() 时只调它，不画角色/状态栏/菜单。
    void renderDockBar(const PetStatus& s);
    // 吸附条内容高度（物理像素；主循环喂给窗口层 updateDockBarHeight，
    // 公式对齐 1.x computeDockBarHeight：PAD*2+灯+GAP+n*徽标+(n-1)*徽标间距）
    float dockBarHeight(const PetStatus& s) const;
    // 双击吸附条空白处：请求退出吸附恢复整窗（main 接窗口层 exitEdgeDock）
    std::function<void()> on_undock;

    // 请求打开菜单（状态栏 ☰ 按钮点击后由本类置位，主循环/窗口层消费）
    // 简化：☰ 点击直接 openMenu()，无需跨帧标志

private:
    struct Impl;
    Impl* impl_;
    // 硬件显示端状态（setDeviceStatus 注入；渲染线程同循环读，无并发）
    bool device_online_ = false;
    std::string device_mode_ = "multi";
};

} // namespace dutyon
