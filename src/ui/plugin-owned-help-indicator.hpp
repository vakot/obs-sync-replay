#pragma once

#include <algorithm>

namespace obs_sync_replay {

struct PluginOwnedHelpIndicatorGeometry final {
    int indicator_x = 0;
    int indicator_y = 0;
    int indicator_width = 0;
    int indicator_height = 0;
    int hit_x = 0;
    int hit_y = 0;
    int hit_width = 0;
    int hit_height = 0;

    constexpr bool Contains(const int x, const int y) const noexcept {
        return x >= hit_x && x < hit_x + hit_width && y >= hit_y && y < hit_y + hit_height;
    }
};

inline PluginOwnedHelpIndicatorGeometry MakePluginOwnedHelpIndicatorGeometry(
    const int button_width, const int button_height, const int indicator_width, const int indicator_height,
    const int right_margin, const int hit_padding) noexcept {
    const int width = std::max(0, button_width);
    const int height = std::max(0, button_height);
    const int icon_width = std::clamp(indicator_width, 0, width);
    const int icon_height = std::clamp(indicator_height, 0, height);
    const int margin = std::max(0, right_margin);
    const int padding = std::max(0, hit_padding);
    const int indicator_x = std::max(0, width - margin - icon_width);
    const int indicator_y = std::max(0, (height - icon_height) / 2);
    const int hit_x = std::max(0, indicator_x - padding);
    const int hit_y = std::max(0, indicator_y - padding);
    const int hit_right = std::min(width, indicator_x + icon_width + padding);
    const int hit_bottom = std::min(height, indicator_y + icon_height + padding);

    return {indicator_x,
            indicator_y,
            icon_width,
            icon_height,
            hit_x,
            hit_y,
            std::max(0, hit_right - hit_x),
            std::max(0, hit_bottom - hit_y)};
}

} // namespace obs_sync_replay
