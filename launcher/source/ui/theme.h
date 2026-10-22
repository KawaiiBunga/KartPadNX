// Theme tokens: a light, soft-blue look with one user-selectable accent.
#pragma once

#include <cstdint>

#include "ui/geometry.h"

namespace ui {

enum class Accent : std::uint8_t { Blue = 0, Green = 1, Red = 2, Purple = 3 };
inline constexpr int kAccentCount = 4;

struct Theme {
    Color bg;         // page background (top of the gradient)
    Color bgLow;      // page background (bottom of the gradient)
    Color panel;      // card body
    Color panelHi;    // hovered / focused card body
    Color edge;       // hairline borders
    Color text;       // primary text
    Color textDim;    // secondary text
    Color textFaint;  // hints, disabled labels
    Color accent;     // the one accent colour
    Color accentHi;   // lighter end of the accent gradient
    Color panelHead;  // header strip of settings panels
    Color good;
    Color warn;
    Color bad;
};

Theme MakeTheme(Accent accent);
const char* AccentName(Accent accent);
Accent AccentFromIndex(int index);

}  // namespace ui
