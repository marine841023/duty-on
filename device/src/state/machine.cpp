#include "state/machine.h"

namespace dutyon {

StateMachine::StateMachine() : current_state_("idle") {}

std::string StateMachine::onStatus(const PetStatus& status) {
    if (status.state == current_state_) return "";
    current_state_ = status.state;
    return motionGroupFor(status.state);
}

std::string StateMachine::currentIdleGroup() const {
    return motionGroupFor(current_state_);
}

std::string StateMachine::motionGroupFor(const std::string& state) {
    // 与桌面版 state -> motion 映射保持一致
    // 这些动作组名来自 Live2D 模型的 .model3.json Motions 字段
    if (state == "working")  return "working";   // 忙碌/工作中动作
    if (state == "alert")    return "alert";     // 提醒/注意动作
    if (state == "sleeping") return "sleeping";  // 睡眠动作
    return "idle";                               // 默认待机
}

} // namespace dutyon
