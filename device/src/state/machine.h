#pragma once

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

private:
    std::string current_state_;

    static std::pair<std::string, int> motionFor(const std::string& state);
};

} // namespace dutyon
