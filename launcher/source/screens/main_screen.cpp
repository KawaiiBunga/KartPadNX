// The main screen: frame, tab strip, Launch button and input. Tabs: main_screen_play.cpp (Play,
// Mods), main_screen_profile_tabs.cpp (Saves, Licenses, Miis), main_screen_system_tabs.cpp
// (Game Files, Settings).
#include "screens/main_screen.h"
#include "screens/main_screen_ui.h"

#include <memory>
#include <string>
#include <vector>

#include "app/app.h"
#include "mkw/data.h"
#include "screens/flow.h"

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

std::string ShortStamp(const std::string& stamp) {
    const auto sp = stamp.find(' ');
    std::string s = sp == std::string::npos ? stamp : stamp.substr(sp + 1);
    if (s.size() >= 16 && s[10] == 'T') s = s.substr(0, 10) + " " + s.substr(11, 5);
    return s.empty() ? std::string("Unknown build") : "Build " + s;
}

std::string ShortPath(const std::string& path) {
    std::string p = path.rfind("sdmc:", 0) == 0 ? "SD:" + path.substr(5) : path;
    constexpr size_t kMax = 56;
    if (p.size() > kMax) p = "..." + p.substr(p.size() - (kMax - 3));
    return p;
}

void DrawPanel(ui::Canvas& c, const ui::Theme& th, const Rect& r) {
    c.Shadow(r, 22, 18, 40);
    c.FillRoundRect(r, 22, th.panel);
    c.StrokeRoundRect(r, 22, 1.0f, th.edge);
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
    // Only the first time: coming back from a sub-screen keeps the tab's game choice.
    if (!entered_) {
        entered_ = true;
        selected_ = app.settings().lastVariant == 1 ? Variant::RetroRewind : Variant::Vanilla;
        profileVariant_ = selected_;
    }
    Build(app);
}

void MainScreen::OnLeave(app::App& app) {
    if (settingsDirty_) {
        mkw::Settings().Save(app.settings());
        settingsDirty_ = false;
    }
}

void MainScreen::SetTab(app::App& app, Tab tab) {
    if (settingsDirty_) {
        if (!mkw::Settings().Save(app.settings())) app.Toast("Could not save settings to the SD card.", 3.5f);
        settingsDirty_ = false;
    }
    tab_ = tab;
    Build(app);
}

void MainScreen::AddGameRow(app::App& app, float x, float y, float w) {
    auto* game = Add<ui::ListRow>(Rect{x, y, w, 52}, "Game", [this]() { return std::string(mkw::VariantDisplayName(profileVariant_)); },
                                  [this, &app]() {
                                      profileVariant_ = profileVariant_ == Variant::Vanilla ? Variant::RetroRewind : Variant::Vanilla;
                                      Build(app);
                                  });
    game->SetIcon(ui::icon::kFlag);
    Track(game);
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
    // + anywhere: start the selected game.
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

    c.Shadow(kFrame, 30, 24, 50);
    c.FillRoundRect(kFrame, 30, ui::WithAlpha(white, 150));
    c.StrokeRoundRect(kFrame, 30, 1.0f, ui::WithAlpha(white, 230));
    c.Icon(ui::icon::kFlag, 46, 78, 56, th.text);
    c.Text(ui::kDisplay, "MKW Launcher", 112, 34, th.text);

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
        const float tw = c.TextWidth(ui::kLabel, kTabs[i].name);
        const float x0 = r.x + (r.w - tw - 34) * 0.5f;
        c.Icon(kTabs[i].icon, 26, x0 + 13, r.y + r.h * 0.5f, active ? white : th.textDim);
        c.Text(ui::kLabel, kTabs[i].name, x0 + 34, r.y + r.h * 0.5f, active ? white : th.text, Align::Left, VAlign::Middle);
    }

    DrawPanel(c, th, kContent);

    // Status line only when something needs doing.
    const char* status = !mkw::Disc().HasValidGameData() ? "Import your disc in Game Files to play."
                         : app.AppletMode() ? "Low memory mode: hold R on a game in HOME, then open the launcher."
                                            : nullptr;
    if (status) c.Text(ui::kLabel, status, 64, kBarY + 44, th.warn, Align::Left, VAlign::Middle);

    Screen::Draw(app, c);
}

void MainScreen::Build(app::App& app) {
    ResetWidgets();
    switch (tab_) {
        case Tab::Play: BuildPlay(app); break;
        case Tab::Mods: BuildMods(app); break;
        case Tab::Saves: BuildSaves(app); break;
        case Tab::Licenses: BuildLicenses(app); break;
        case Tab::Miis: BuildMiis(app); break;
        case Tab::GameFiles: BuildGameFiles(app); break;
        case Tab::Settings: BuildSettings(app); break;
        case Tab::Count: break;
    }
    auto* launch = Add<ui::Button>(Rect{966, kBarY + 6, 266, 64}, "Play", true, [this, &app]() { Launch(app, selected_); });
    launch->SetIcon(ui::icon::kPlay);
    launch->SetLarge(true);
    launch->enabled = mkw::Disc().HasValidGameData();
    Track(launch);
}

}  // namespace screens
