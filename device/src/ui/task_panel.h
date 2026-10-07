#pragma once

// 设备端任务列表面板（屏幕下半屏）：会话行 = 状态色圆点 + 项目名 + 状态文字，
// 配色/文案对齐 PC 端状态栏（ui_renderer.cpp StyleFor / StatusText）。
// 行块贴角色区下方堆叠成半透明黑圆角卡片（与角色留一线距离），未占区域保持透明黑。

#ifndef _WIN32

#include <string>
#include <vector>

#include "api/client.h"

namespace dutyon {

class TaskPanel {
public:
    TaskPanel();
    ~TaskPanel();

    // 加载文字字体；失败则面板只画底色无文字
    bool init(const std::string& font_path);

    // 设定时钟/日期颜色主题（PC 菜单下发）：amber(默认)/ice/white/green/pink；
    // 未知名字回退 amber。renderClock/renderDate 立即使用新颜色
    void setClockColor(const std::string& name);

    // multi 模式动态分屏：按任务数算面板区高度（行数×行高 + 顶隙 +
    // 底边距）。任务少时面板矮、角色区大；0 任务按 1 行（"暂无任务"）
    static float heightForSessions(int session_count);

    // 在下半屏绘制：area_top = 面板区顶边（布局约定为 screen_h/2），
    // 行块自角色区下方（留隙）向下堆叠（卡片高度按行数自适应）。
    // region_x/region_w：水平放置区域（横屏左右布局时限制到右列）；
    // region_w<0 表示用整屏宽（竖屏默认，等价旧行为）
    void render(const PetStatus& status, int screen_w, int screen_h, float area_top,
                int region_x = 0, int region_w = -1);

    // 顶部时钟（HH:MM:SS 居中）：y_top = 文本区顶边（GL 坐标），
    // size = 字号（多任务模式 ~40，单任务模式 ~56）。region_x/region_w
    // 同上（在区域内水平居中；region_w<0 = 整屏）
    void renderClock(const std::string& text, float y_top, float size,
                     int screen_w, int screen_h,
                     int region_x = 0, int region_w = -1);

    // 底部日期行（YYYY年M月D日 星期X 居中，屏幕下缘）：同款字体/配色、
    // 字号更小；过宽自动缩字号保证一行放下。region_x/region_w 同上
    void renderDate(const std::string& text, float y_top, float size,
                    int screen_w, int screen_h,
                    int region_x = 0, int region_w = -1);

    // 语音会话动作区（设备端，布局：上时钟/中人物/下动作 ≤2 行）：
    // pill 网格展示当前角色可执行指令（仅展示，无触摸），highlight 指令
    // 主题色高亮闪烁；commands 为空 = 角色不支持（画 voice.unsupported 提示）。
    // area_top = 动作区顶边（GL y）；返回面板实际高度（px，主循环压缩人物视口用）
    float renderVoicePanel(const std::vector<std::string>& commands, int highlight,
                           const std::string& hint, int screen_w, int screen_h,
                           float area_top, int region_x = 0, int region_w = -1);

    // 语音面板高度测量（与 renderVoicePanel 同公式）：主循环设人物视口时
    // 需先知道面板高度（面板贴底、卡片占 [0, 高度]），渲染时 area_top 传
    // 该值即可精确贴底。字体未加载返回 0（面板不画、人物视口不压缩）。
    float voicePanelHeight(int command_count);

    // 屏幕右上角连接状态图标（两块，纯色几何、无贴图）：
    //   最右 Wi-Fi 信号条（4 根递增）——设备是否已入网：入网=绿，未入网/配网中=红；
    //   其左 显示器图标——是否已连上 PC：连上=绿，未连=灰。两段链路分开表示。
    void renderNetStatus(bool wifi_online, bool pc_online,
                         int screen_w, int screen_h);

    // 全屏压暗叠层（软件亮度）：brightness 10-100，>=100 不画。
    // 当前屏无 /sys/class/backlight 内核接口时的亮度实现；
    // 有背光 sysfs 的量产屏由 main 优先写背光，此叠层不生效
    void renderDim(int brightness, int screen_w, int screen_h);

    // 时钟字体行高（用于日期行垂直定位；卡通字体加载失败回退主字体行高）
    float clockLineHeight(float pixel_size) const;

    // 字距缩放转发到两套字体（<1 收紧；配网大字页用，渲染后还原 1.0）
    void setSpacing(float scale);

private:
    struct Impl;
    Impl* impl_;
};

} // namespace dutyon

#endif // !_WIN32
