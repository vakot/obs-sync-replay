#include "ui/plugin-owned-help-indicator.hpp"

#include <cstdlib>
#include <iostream>

using namespace obs_sync_replay;

namespace {

void Require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void TestIndicatorIsRightAlignedAndCentered() {
    const auto geometry = MakePluginOwnedHelpIndicatorGeometry(200, 40, 16, 16, 8, 4);
    Require(geometry.indicator_x == 176 && geometry.indicator_y == 12, "indicator must use button-relative geometry");
    Require(geometry.indicator_width == 16 && geometry.indicator_height == 16, "indicator size must be preserved");
    Require(geometry.Contains(176, 12) && geometry.Contains(195, 19), "indicator hit area must include the glyph");
    Require(!geometry.Contains(100, 20), "main button text area must not be the indicator hit area");
}

void TestGeometryClampsForDpiAndSmallButtons() {
    const auto geometry = MakePluginOwnedHelpIndicatorGeometry(8, 8, 24, 24, 6, 4);
    Require(geometry.indicator_width == 8 && geometry.indicator_height == 8,
            "indicator must remain inside a small resized button");
    Require(geometry.hit_x >= 0 && geometry.hit_y >= 0 && geometry.hit_width <= 8 && geometry.hit_height <= 8,
            "hit area must remain inside the button at high DPI or small sizes");
}

} // namespace

int main() {
    TestIndicatorIsRightAlignedAndCentered();
    TestGeometryClampsForDpiAndSmallButtons();
    return EXIT_SUCCESS;
}
