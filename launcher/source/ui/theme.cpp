#include "ui/theme.h"

namespace ui {

Theme MakeTheme(Accent accent) {
    Color acc{0x2F, 0x8B, 0xF5, 255};
    Color hi{0x6B, 0xB8, 0xFF, 255};
    switch (accent) {
        case Accent::Blue:   acc = {0x2F, 0x8B, 0xF5, 255}; hi = {0x6B, 0xB8, 0xFF, 255}; break;
        case Accent::Green:  acc = {0x2E, 0xA8, 0x57, 255}; hi = {0x6C, 0xD4, 0x8A, 255}; break;
        case Accent::Red:    acc = {0xE0, 0x45, 0x45, 255}; hi = {0xFF, 0x86, 0x7A, 255}; break;
        case Accent::Purple: acc = {0x7B, 0x5C, 0xE6, 255}; hi = {0xA9, 0x93, 0xFF, 255}; break;
    }
    Theme t{};
    t.bg = {0xE9, 0xF1, 0xFB, 255};
    t.bgLow = {0xD5, 0xE4, 0xF6, 255};
    t.panel = {0xFF, 0xFF, 0xFF, 255};
    t.panelHi = {0xF1, 0xF6, 0xFE, 255};
    t.panelHead = Mix(acc, Color{0xFF, 0xFF, 0xFF, 255}, 0.55f);
    t.edge = {0xD6, 0xE1, 0xEE, 255};
    t.text = {0x1E, 0x2F, 0x4D, 255};
    t.textDim = {0x5D, 0x6F, 0x8A, 255};
    t.textFaint = {0x9A, 0xA8, 0xBC, 255};
    t.accent = acc;
    t.accentHi = hi;
    t.good = {0x2F, 0xB8, 0x62, 255};
    t.warn = {0xE5, 0x8E, 0x1F, 255};
    t.bad = {0xE0, 0x45, 0x45, 255};
    return t;
}

const char* AccentName(Accent accent) {
    switch (accent) {
        case Accent::Blue:   return "Blue";
        case Accent::Green:  return "Green";
        case Accent::Red:    return "Red";
        case Accent::Purple: return "Purple";
    }
    return "Blue";
}

Accent AccentFromIndex(int index) {
    if (index < 0 || index >= kAccentCount) return Accent::Blue;
    return static_cast<Accent>(index);
}

}  // namespace ui
