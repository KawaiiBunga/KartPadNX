// Shared by the main screen's source files (main_screen*.cpp): tab table, layout and small
// drawing/job helpers.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "app/app.h"
#include "mkw/services.h"
#include "screens/main_screen.h"
#include "ui/canvas.h"

namespace screens::main_ui {

using mkw::Variant;
using ui::Align;
using ui::Rect;
using ui::VAlign;

struct TabInfo {
    const char* name;
    char32_t icon;
    const char* heading;      // content panel header
    const char* description;  // right side of the header strip
};

inline constexpr TabInfo kTabs[] = {
    {"Play", ui::icon::kHome, "", ""},
    {"Saves", ui::icon::kSave, "Save Backups", "Back up and restore your save files"},
    {"Licenses", ui::icon::kBadge, "Licenses", "Name, Mii, VR and BR for each license"},
    {"Miis", ui::icon::kFace, "Miis", "Your Switch Miis, for both games"},
    {"Game Files", ui::icon::kFolder, "Game Files", "Your disc and Retro Rewind files"},
    {"Settings", ui::icon::kSettings, "Settings", "Saved when you leave this tab"},
    {"Updates", ui::icon::kUpdate, "Updates", "New versions from GitHub"},
};
static_assert(sizeof(kTabs) / sizeof(kTabs[0]) == static_cast<size_t>(MainScreen::Tab::Count));

// Layout (1280x720 logical).
inline constexpr Rect kFrame{20, 14, 1240, 692};
inline constexpr float kTabsY = 100.0f, kTabsH = 54.0f, kTabsX = 44.0f, kTabsW = 1192.0f, kTabGap = 10.0f;
inline constexpr Rect kContent{44, 170, 1192, 430};
inline constexpr float kHeadH = 50.0f;
inline constexpr float kBodyTop = 170.0f + 50.0f + 18.0f;  // first row inside a content panel
inline constexpr Rect kPlayArea{44, 170, 830, 430};
inline constexpr Rect kQuick{890, 170, 346, 430};
inline constexpr float kBarY = 618.0f;

Rect TabRect(int i);
const mkw::PayloadInfo* FindPayload(const std::vector<mkw::PayloadInfo>& list, Variant v);
// "KARTPAD-BUILD 2026-10-05T05:14:17Z" -> "Build 2026-10-05 05:14"
std::string ShortStamp(const std::string& stamp);
// SD path for display, trimmed from the left so the file name always shows.
std::string ShortPath(const std::string& path);
void DrawPanel(ui::Canvas& c, const ui::Theme& th, const Rect& r, const char* heading, const char* description);
// Shows a JobScreen polling `poll` until the job ends, then pops it and calls `after`.
void PushJob(app::App& app, std::string title, std::string subtitle, mkw::JobStatus (*poll)(), void (*cancel)(),
             std::function<void(bool)> after);

}  // namespace screens::main_ui
