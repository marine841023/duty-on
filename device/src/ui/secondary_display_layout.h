#pragma once

#include <algorithm>

namespace dutyon {

enum class SecondaryPlacement { Auto, Left, Right };
enum class SecondaryMetrics { Hidden, Compact, Full };

struct SecondaryDisplaySettings {
    SecondaryPlacement placement = SecondaryPlacement::Auto;
    bool show_metrics = false;
};

// Rectangles use the top-left origin of the display, after rotation.
struct DisplayRegion {
    int x = 0, y = 0, w = 0, h = 0;
};

struct SecondaryDisplayLayout {
    DisplayRegion pet;
    DisplayRegion monitor;
    SecondaryMetrics metrics = SecondaryMetrics::Hidden;
};

inline SecondaryDisplayLayout secondaryDisplayLayout(int w, int h, SecondaryDisplaySettings settings) {
    if (w <= 0 || h <= 0) return {};
    SecondaryDisplayLayout layout{{0, 0, w, h}, {}, SecondaryMetrics::Hidden};
    const int long_side = ((std::min)(w, h) * 16 + 4) / 9;
    if (w > long_side) {
        const int extra = w - long_side;
        if (settings.show_metrics && h >= 320 && extra >= 180 && extra * 720 >= h * 300)
            layout.metrics = h >= 540 && extra * 720 >= h * 520
                ? SecondaryMetrics::Full : SecondaryMetrics::Compact;
        const bool monitor = layout.metrics != SecondaryMetrics::Hidden;
        layout.pet.w = long_side;
        if (settings.placement == SecondaryPlacement::Right) layout.pet.x = extra;
        else if (settings.placement == SecondaryPlacement::Auto && !monitor) layout.pet.x = extra / 2;
        if (monitor) layout.monitor = {layout.pet.x == 0 ? long_side : 0, 0, extra, h};
    } else if (h > long_side) {
        const int extra = h - long_side;
        layout.pet.h = long_side;
        if (settings.show_metrics && w >= 360 && extra >= (std::max)(140, w * 220 / 720)) {
            layout.metrics = SecondaryMetrics::Compact;
            layout.monitor = {0, long_side, w, extra};
        } else {
            layout.pet.y = extra / 2;
        }
    }
    return layout;
}

} // namespace dutyon
