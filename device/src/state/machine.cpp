#include "state/machine.h"

namespace dutyon {

StateMachine::StateMachine() : current_state_("sleeping") {}

std::pair<std::string, int> StateMachine::onStatus(const PetStatus& status) {
    // confirmation-needed 优先级最高，即使 overallState 是 working 也切到 alert
    std::string effective = status.has_confirmation ? "alert" : status.overall_state;

    if (effective == current_state_) return {"", 0};
    current_state_ = effective;
    return motionForState(effective);
}

std::pair<std::string, int> StateMachine::currentMotion() const {
    return motionForState(current_state_);
}

std::pair<std::string, int> StateMachine::motionForState(const std::string& state) const {
    // 用户通过「动作设定」覆盖的优先
    auto it = overrides_.find(state);
    if (it != overrides_.end()) return it->second;

    // 与桌面版 DEFAULT_STATE_MOTIONS（renderer.js:1785）一致
    if (state == "working") return {"FlickLeft", 1};  // 走路
    if (state == "alert")   return {"FlickLeft", 0};  // yeah
    if (state == "sleeping") return {"Flick3", 1};    // 哈欠
    return {"Idle", 0};
}

void StateMachine::setMotionFor(const std::string& state,
                                const std::string& group, int index) {
    overrides_[state] = {group, index};
}

void StateMachine::clearOverrides() {
    overrides_.clear();
}

} // namespace dutyon
