#pragma once

#include <map>
#include <string>
#include <utility>
#include "api/client.h"

namespace dutyon {

// 将桌面端 API 状态映射到 Live2D 动作
// 默认值与桌面版 renderer.js DEFAULT_STATE_MOTIONS 保持一致（nito 系列模型）：
//   sleeping -> Flick3[1]   （哈欠）
//   working  -> FlickLeft[1]（走路）
//   alert    -> FlickLeft[0]（yeah）
// 空闲待机 -> Idle 组循环
//
// 桌面端 PetState（state_manager.rs）:
//   Sleeping -> 所有 IDE 空闲
//   Working  -> 有 AI 任务在跑（thinking / tool-use）
//   Alert    -> 有会话等待用户确认（confirmation-needed）
class StateMachine {
public:
    StateMachine();

    // (motion_group, motion_index)；状态未变化时 group 为空
    std::pair<std::string, int> onStatus(const PetStatus& status);

    // 当前状态对应的动作
    std::pair<std::string, int> currentMotion() const;

    // 指定状态的当前动作（动作设定菜单 hint 用）
    std::pair<std::string, int> motionForState(const std::string& state) const;

    // 动作设定：覆盖某状态的默认动作（对应 1.x 菜单「动作设定」）
    void setMotionFor(const std::string& state, const std::string& group, int index);

    // 清除全部用户覆盖（切换模型后按新模型的 stateMotions 重新应用）
    void clearOverrides();

private:
    std::string current_state_;
    // 状态 -> 用户设定的动作（覆盖默认映射）
    std::map<std::string, std::pair<std::string, int>> overrides_;
};

} // namespace dutyon
