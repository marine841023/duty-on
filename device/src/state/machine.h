#pragma once

#include <string>
#include "api/client.h"

namespace dutyon {

// 将桌面端 API 状态映射到 Live2D 动作组/表情
// 对应桌面版 renderer.js 中的 state -> motion 逻辑
class StateMachine {
public:
    StateMachine();

    // 根据新状态返回应播放的 Live2D 动作组名
    // 状态未变化时返回空字符串（避免重复触发）
    std::string onStatus(const PetStatus& status);

    // 当前状态对应的 idle 动作组
    std::string currentIdleGroup() const;

private:
    std::string current_state_;

    static std::string motionGroupFor(const std::string& state);
};

} // namespace dutyon
