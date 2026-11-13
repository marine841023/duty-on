#pragma once

// 设备端界面（ARM Linux GLES 直绘，无 ImGui）。视觉语言：
//   - 屏幕是一个小舞台：角色身后一束随整体状态变色、呼吸的舞台光；
//   - 任务是一排"值班牌"：圆角牌 + 状态灯 + 项目名 + 状态词。颜色只给状态灯
//     和状态词，需要确认的牌整块反色成珊瑚红，隔着桌子一眼能看到；
//   - 时钟主题色（PC 菜单下发）是全屏唯一的强调色。
// 所有形状走同一个 SDF 着色器（圆角/描边/光晕都有抗锯齿）。
//
// 坐标约定：像素坐标，GL 原点左下、y 向上。region_x/region_w 是信息区的
// 水平范围（横屏为右列，竖屏为整屏；region_w<0 = 整屏宽）。
//
// DUTYON_UI_PREVIEW：Windows 离屏预览工具（device/tools/ui-preview）用桌面 GL
// 编译同一份代码出图，设备端与 PC 正式程序都不定义它。

#if !defined(_WIN32) || defined(DUTYON_UI_PREVIEW)

#include <string>
#include <vector>

#include "api/client.h"

namespace dutyon {

class TaskPanel {
public:
    TaskPanel();
    ~TaskPanel();

    // 加载字体：font_path = 正文（含 CJK）；clock_font_path = 时钟数字字体，
    // 缺失时回退正文字体。正文字体失败返回 false（之后只画形状无文字）
    bool init(const std::string& font_path,
              const std::string& clock_font_path = "/opt/dutyon/assets/font-clock.ttf");

    // 主题色（PC 菜单下发）：amber(默认)/ice/white/green/pink，未知回退 amber
    void setClockColor(const std::string& name);

    // ---- 正常画面 ----

    // 舞台光：画在角色之前。vx/vy/vw/vh = 角色视口；overall_state =
    // sleeping/working/alert，决定光的颜色和呼吸节奏
    void renderBackdrop(const std::string& overall_state, int screen_w, int screen_h, int vx,
                        int vy, int vw, int vh);

    // 顶部时间栏高度（主循环给角色视口让位用）。large = 单任务/相框的大时钟
    float headerHeight(bool large, bool landscape) const;

    // 时间栏：时:分 大字 + 小号秒；date（如 11月12日）与 weekday（如 星期四）
    // 竖屏多任务排在右侧两行，其余排成一行。
    // hms = "HH:MM:SS"；date/weekday 可为空
    void renderHeader(const std::string& hms, const std::string& date, const std::string& weekday,
                      bool large, bool landscape, int screen_w, int screen_h, int region_x = 0,
                      int region_w = -1);

    // 任务牌占用高度（竖屏多任务模式动态分屏）；0 个任务按 1 行算
    static float heightForSessions(int session_count);

    // 任务牌列表：自 area_top 向下排，放不下的折叠成"还有 N 个"
    void render(const PetStatus& status, int screen_w, int screen_h, float area_top,
                int region_x = 0, int region_w = -1);

    // 单任务模式：只画最要紧的一张牌（待确认 > 进行中），都空闲则不画。
    // 牌顶边 = area_top
    void renderFocus(const PetStatus& status, int screen_w, int screen_h, float area_top,
                     int region_x = 0, int region_w = -1);

    // 语音指令区：提示行 + 指令药丸网格（highlight 为命中的指令）。
    // area_top = 区域顶边；返回实际高度
    float renderVoicePanel(const std::vector<std::string>& commands, int highlight,
                           const std::string& hint, int screen_w, int screen_h, float area_top,
                           int region_x = 0, int region_w = -1);
    float voicePanelHeight(int command_count);

    // ---- 引导画面（整屏，不画角色）----

    // 配网：热点名/密码 + 步骤。failure 非空时作为标题（上次入网失败原因）
    void renderProvision(const std::string& ssid, const std::string& pass,
                         const std::string& failure, int screen_w, int screen_h);
    // 正在入网：progress = 尝试次数/倒计时文案
    void renderJoining(const std::string& progress, int screen_w, int screen_h);
    // 配对码：waiting = 已配对过、等原电脑上线；wifi_just_ok = 刚入网的提示
    void renderPairing(const std::string& code, bool waiting, bool wifi_just_ok, int screen_w,
                       int screen_h);

    // ---- 常驻叠加 ----

    // 右上角两段链路：Wi-Fi（设备入网）与电脑（已连上 PC）。
    // 断开 = 变灰 + 珊瑚色斜杠（不只靠颜色区分）
    void renderNetStatus(bool wifi_online, bool pc_online, int screen_w, int screen_h);

    // 有任务等确认时，屏幕四边泛起呼吸的珊瑚色光（画在角色之上，
    // 任何角色、任何模式下余光都能扫到）
    void renderAlertEdge(int screen_w, int screen_h);

    // 软件亮度：整屏压暗（brightness 10-100，>=100 不画）
    void renderDim(int brightness, int screen_w, int screen_h);

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace dutyon

#endif  // !_WIN32 || DUTYON_UI_PREVIEW
