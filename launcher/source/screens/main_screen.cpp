// The main screen: frame, tab strip, bottom bar and input. Tabs: main_screen_play.cpp
// (Play), main_screen_profile_tabs.cpp (Saves, Licenses, Miis), main_screen_system_tabs.cpp
// (Game Files, Settings, Updates).
#include "screens/main_screen.h"
#include "screens/main_screen_ui.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "app/app.h"
#include "mkw/data.h"
#include "screens/flow.h"
#include "screens/profiles.h"
#include "ui/keyboard.h"

namespace screens {
namespace main_ui {

Rect TabRect(int i) {
    const int n = static_cast<int>(MainScreen::Tab::Count);
    const float w = (kTabsW - kTabGap * (n - 1)) / n;
    return {kTabsX + i * (w + kTabGap), kTabsY, w, kTabsH};
}

const mkw::PayloadInfo* FindPayload(const std::vector<mkw::PayloadInfo>& list, Variant v) {
    for (const auto& p : list) {
        if (p.variant == v) return &p;
    }
    return nullptr;
}

// "KARTPAD-BUILD 2026-10-05T05:14:17Z" -> "Build 2026-10-05 05:14"
std::string ShortStamp(const std::string& stamp) {
    const auto sp = stamp.find(' ');
    std::string s = sp == std::string::npos ? stamp : stamp.substr(sp + 1);
    if (s.size() >= 16 && s[10] == 'T') s = s.substr(0, 10) + " " + s.substr(11, 5);
    return s.empty() ? std::string("Unknown build") : "Build " + s;
}

// SD path for display, trimmed from the left so the file name always shows.
std::string ShortPath(const std::string& path) {
    std::string p = path.rfind("sdmc:", 0) == 0 ? "SD:" + path.substr(5) : path;
    constexpr size_t kMax = 56;
    if (p.size() > kMax) p = "..." + p.substr(p.size() - (kMax - 3));
    return p;
}

void DrawPanel(ui::Canvas& c, const ui::Theme& th, const Rect& r, const char* heading, const char* description) {
    c.Shadow(r, 22, 18, 40);
    c.FillRoundRect(r, 22, th.panel);
    c.StrokeRoundRect(r, 22, 1.0f, th.edge);
    if (heading && *heading) {
        const Rect head{r.x + 10, r.y + 10, r.w - 20, kHeadH - 6};
        c.FillGradientRoundRect(head, 14, th.accentHi, th.accent);
        c.Text(ui::kTitle, heading, head.x + 18, head.y + head.h * 0.5f, ui::Color{255, 255, 255, 255}, Align::Left, VAlign::Middle);
        if (description && *description) {
            c.Text(ui::kSmall, description, head.x + head.w - 18, head.y + head.h * 0.5f, ui::Color{255, 255, 255, 230},
                   Align::Right, VAlign::Middle);
        }
    }
}

void PushJob(app::App& app, std::string title, std::string subtitle, mkw::JobStatus (*poll)(), void (*cancel)(),
             std::function<void(bool)> after) {
    app.Push(std::make_unique<JobScreen>(
        std::move(title), std::move(subtitle), [poll]() { return poll(); },
        [cancel]() {
            if (cancel) cancel();
        },
        [&app, after = std::move(after)](bool ok) {
            app.Pop();
            if (after) after(ok);
        }));
}

}  // namespace main_ui

using namespace main_ui;

void MainScreen::OnEnter(app::App& app) {
    selected_ = app.settings().lastVariant == 1 ? Variant::RetroRewind : Variant::Vanilla;
    RefreshProfile();
    Build(app);
}

void MainScreen::OnLeave(app::App& app) {
    if (settingsDirty_) {
        mkw::Settings().Save(app.settings());
        settingsDirty_ = false;
    }
}

void MainScreen::RefreshProfile() {
    profileName_.clear();
    std::string err;
    if (mkw::Licenses().Load(selected_, &err)) {
        for (const auto& l : mkw::Licenses().List()) {
            if (l.used) {
                profileName_ = l.name;
                break;
            }
        }
    }
}

void MainScreen::SetTab(app::App& app, Tab tab) {
    if (tab_ == Tab::Settings && settingsDirty_) {
        if (!mkw::Settings().Save(app.settings())) app.Toast("Could not save settings to the SD card.", 3.5f);
        settingsDirty_ = false;
    }
    tab_ = tab;
    Build(app);
}

void MainScreen::Update(app::App& app, const app::Input& in, float dt) {
    const int count = static_cast<int>(Tab::Count);
    if (in.tabPrev || in.tabNext) {
        SetTab(app, static_cast<Tab>((static_cast<int>(tab_) + (in.tabNext ? 1 : -1) + count) % count));
        return;
    }
    if (in.touchUp) {
        for (int i = 0; i < count; ++i) {
            if (TabRect(i).Contains(in.touchX, in.touchY)) {
                SetTab(app, static_cast<Tab>(i));
                return;
            }
        }
    }
    // Hold + anywhere: start the selected game.
    if (in.plusHeld && !plusWasHeld_) {
        plusWasHeld_ = true;
        Launch(app, selected_);
        return;
    }
    plusWasHeld_ = in.plusHeld;
    Screen::Update(app, in, dt);
}

void MainScreen::Draw(app::App& app, ui::Canvas& c) {
    const ui::Theme th = app.theme();
    const ui::Color white{255, 255, 255, 255};

    // Frame and header.
    c.Shadow(kFrame, 30, 24, 50);
    c.FillRoundRect(kFrame, 30, ui::WithAlpha(white, 150));
    c.StrokeRoundRect(kFrame, 30, 1.0f, ui::WithAlpha(white, 230));
    c.Icon(ui::icon::kFlag, 46, 78, 56, th.text);
    c.Text(ui::kDisplay, "MKW Launcher", 112, 26, th.text);
    c.Text(ui::kSmall, "Play  \xC2\xB7  Customize  \xC2\xB7  Race Together", 114, 72, th.textDim);

    // Header-right controller hint.
    const Rect hint{1060, 40, 176, 36};
    c.FillRoundRect(hint, 18, th.panel);
    c.StrokeRoundRect(hint, 18, 1.0f, th.edge);
    c.Text(ui::kSmall, "L / R  change tab", hint.x + hint.w * 0.5f, hint.y + hint.h * 0.5f, th.textDim, Align::Center,
           VAlign::Middle);

    // Tabs.
    for (int i = 0; i < static_cast<int>(Tab::Count); ++i) {
        const Rect r = TabRect(i);
        const bool active = i == static_cast<int>(tab_);
        if (active) {
            c.Shadow(r, 16, 14, 70);
            c.FillGradientRoundRect(r, 16, th.accentHi, th.accent);
        } else {
            c.Shadow(r, 16, 8, 22);
            c.FillRoundRect(r, 16, th.panel);
            c.StrokeRoundRect(r, 16, 1.0f, th.edge);
        }
        const ui::Color fg = active ? white : th.text;
        const float tw = c.TextWidth(ui::kLabel, kTabs[i].name);
        const float x0 = r.x + (r.w - tw - 34) * 0.5f;
        c.Icon(kTabs[i].icon, 26, x0 + 13, r.y + r.h * 0.5f, active ? white : th.textDim);
        c.Text(ui::kLabel, kTabs[i].name, x0 + 34, r.y + r.h * 0.5f, fg, Align::Left, VAlign::Middle);
    }

    // Content panels.
    if (tab_ == Tab::Play) {
        DrawPanel(c, th, kPlayArea, nullptr, nullptr);
        DrawPanel(c, th, kQuick, "Quick Settings", nullptr);
    } else {
        const TabInfo& info = kTabs[static_cast<int>(tab_)];
        DrawPanel(c, th, kContent, info.heading, info.description);
    }

    // Bottom bar: a soft wave line, then the centre status.
    c.FillRoundRect(Rect{44, kBarY - 6, 1192, 2}, 1, ui::WithAlpha(th.accentHi, 120));
    const bool ready = mkw::Disc().HasValidGameData();
    const float cx = 640.0f;
    c.Text(ui::kSmall, std::string("MKW Launcher v") + mkw::kLauncherVersion, cx, kBarY + 14, th.textDim, Align::Center);
    const std::string status = ready ? "Ready to play!" : "Import your disc in Game Files";
    const float sw = c.TextWidth(ui::kLabel, status);
    c.FillCircle({cx - sw * 0.5f - 12, kBarY + 52}, 6, ready ? th.good : th.warn);
    c.Text(ui::kLabel, status, cx + 4, kBarY + 52, th.text, Align::Center, VAlign::Middle);
    if (app.AppletMode()) {
        c.Text(ui::kSmall, "Low memory mode: hold R on a game in HOME, then open the launcher.", cx, kBarY - 24,
               th.warn, Align::Center, VAlign::Middle);
    }

    Screen::Draw(app, c);
}

void MainScreen::Build(app::App& app) {
    ResetWidgets();
    switch (tab_) {
        case Tab::Play: BuildPlay(app); break;
        case Tab::Saves: BuildSaves(app); break;
        case Tab::Licenses: BuildLicenses(app); break;
        case Tab::Miis: BuildMiis(app); break;
        case Tab::GameFiles: BuildGameFiles(app); break;
        case Tab::Settings: BuildSettings(app); break;
        case Tab::Updates: BuildUpdates(app); break;
        case Tab::Count: break;
    }
    BuildBottomBar(app);
}

void MainScreen::BuildBottomBar(app::App& app) {
    // Profile chip: the first license of the selected game's save.
    auto* chip = Add<ui::ListRow>(Rect{48, kBarY + 8, 270, 60}, profileName_.empty() ? std::string("No license yet") : profileName_,
                                  nullptr, [this, &app]() {
                                      profileVariant_ = selected_;
                                      SetTab(app, Tab::Licenses);
                                  });
    chip->SetIcon(ui::icon::kPerson);
    Track(chip);

    auto* launch = Add<ui::Button>(Rect{966, kBarY + 6, 266, 64}, "Launch Game", true, [this, &app]() { Launch(app, selected_); });
    launch->SetIcon(ui::icon::kPlay);
    launch->SetLarge(true);
    launch->enabled = mkw::Disc().HasValidGameData();
    Track(launch);
}

}  // namespace screens
