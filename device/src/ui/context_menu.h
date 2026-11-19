#pragma once

#include <string>
#include <vector>

namespace dutyon {

struct ContextMenuItem {
    std::string action;
    std::string label;
    std::string hint;
    bool checked = false;
    bool enabled = true;
    bool separator = false;
    std::vector<ContextMenuItem> children;
};

}
