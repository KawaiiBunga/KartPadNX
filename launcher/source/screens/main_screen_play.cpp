// Main screen, Play tab: game cards, quick actions and launching.
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

using namespace main_ui;

// ---- Play --------------------------------------------------------------------------------

void MainScreen::BuildPlay(app::App& app) {
    const ui::Theme th = app.theme();
    const auto payloads = mkw::Payloads().List();
    const bool gameData = mkw::Disc().HasValidGameData();
    std::string rrVersion;
    const bool rrData = mkw::Disc().HasValidRetroRewind(&rrVersion);

    struct Card {
        Variant v;
        const char* title;
        std::string blurb;
        const char* image;
        float x;
    };
    const Card cards[] = {
        {Variant::Vanilla, "Vanilla Mario Kart", "Play the classic experience", "romfs:/ui/card-vanilla.png", 68.0f},
        {Variant::RetroRewind, "Retro Rewind",
         "Custom tracks, items and Retro WFC",
         "romfs:/ui/card-rr.png", 466.0f},
    };
    int focusIndex = 0;
    for (const Card& c : cards) {
        const mkw::PayloadInfo* p = FindPayload(payloads, c.v);
        auto* box = Add<ui::ItemBox>(Rect{c.x, 196, 382, 380}, c.title, c.blurb);
        box->SetImage(c.image);
        box->SetSelected(c.v == selected_);
        if (!p) {
            box->SetDisabled("Not included in this launcher build");
        } else if (!gameData) {
            box->SetDisabled("Import your disc in Game Files first");
        } else if (c.v == Variant::RetroRewind && !rrData) {
            box->SetDisabled("Add Retro Rewind in Game Files first");
        } else if (!p->unpacked) {
            box->SetBadge("First start", th.accent);
        } else if (c.v == Variant::RetroRewind) {
            box->SetBadge("v" + rrVersion, th.good);
        }
        box->enabled = p && gameData && (c.v == Variant::Vanilla || rrData);
        // A selects the game; A again (or Launch Game) starts it.
        box->OnActivate([this, &app, v = c.v]() {
            if (selected_ == v) {
                Launch(app, v);
                return;
            }
            selected_ = v;
            app.settings().lastVariant = v == Variant::RetroRewind ? 1 : 0;
            settingsDirty_ = true;
            RefreshProfile();
            Build(app);
        });
        Track(box);
        if (c.v == selected_) focusIndex = static_cast<int>(focus_.Items().size()) - 1;
    }

    // Quick settings: the most used game options.
    mkw::LauncherSettings& s = app.settings();
    auto dirty = [this](bool) { settingsDirty_ = true; };
    const float x = kQuick.x + 14, w = kQuick.w - 28, h = 50.0f, gap = 56.0f;
    float y = kQuick.y + kHeadH + 20;
    Track(Add<ui::ToggleRow>(Rect{x, y, w, h}, "Show FPS", &s.showFps, dirty));
    y += gap;
    Track(Add<ui::ToggleRow>(Rect{x, y, w, h}, "Widescreen", &s.widescreen, dirty));
    y += gap;
    Track(Add<ui::SliderRow>(Rect{x, y, w, h}, "Volume", &s.masterVolume, 0, 100, 5, "%"));
    y += gap;
    Track(Add<ui::ToggleRow>(Rect{x, y, w, h}, "Mute", &s.muted, dirty));
    y += gap;
    Track(Add<ui::ToggleRow>(Rect{x, y, w, h}, "Back up save", &s.backupBeforeLaunch, dirty));
    y += gap;
    auto* more = Add<ui::Button>(Rect{x, y + 4, w, 46}, "More settings", false, [this, &app]() { SetTab(app, Tab::Settings); });
    Track(more);
    settingsDirty_ = true;  // sliders have no change callback
    focus_.SetIndex(focusIndex);
}

void MainScreen::Launch(app::App& app, Variant v) {
    if (!mkw::Disc().HasValidGameData()) {
        app.Toast("Import your disc in Game Files first.", 3.5f);
        return;
    }
    if (v == Variant::RetroRewind && !mkw::Disc().HasValidRetroRewind(nullptr)) {
        app.Toast("Add Retro Rewind in Game Files first.", 3.5f);
        return;
    }
    mkw::LauncherSettings& s = app.settings();
    s.lastVariant = v == Variant::RetroRewind ? 1 : 0;
    mkw::Settings().Save(s);
    settingsDirty_ = false;

    // No save yet (first run) is fine; only an existing save that fails to back up stops the launch.
    if (s.backupBeforeLaunch) {
        if (mkw::Saves().HasSave(v) && !mkw::Saves().BackupCurrent(v)) {
            app.overlay().ShowModal(ui::Modal{
                "Save backup failed",
                "Your save could not be backed up before starting.\nStart anyway?",
                {{"Cancel", nullptr, false},
                 {"Start anyway", [this, &app, v]() {
                      app.settings().backupBeforeLaunch = false;
                      Launch(app, v);
                      app.settings().backupBeforeLaunch = true;
                  }, true}}});
            return;
        }
    }

    if (!mkw::Payloads().StartUnpack(v)) {
        app.Toast("The game is already being prepared.", 3.0f);
        return;
    }
    PushJob(app, std::string("Starting ") + mkw::VariantDisplayName(v),
            "Getting the game ready. The first start takes a few seconds longer.",
            []() { return mkw::Payloads().Poll(); }, nullptr, [&app, v](bool ok) {
                if (!ok) return;
                std::string err;
                // Miis made in System Settings while the launcher was open
                int added = 0, updated = 0;
                mkw::Miis().SyncFromSwitch(&added, &updated, &err);
                err.clear();
                if (!mkw::Payloads().LaunchGame(v, "default", &err)) {
                    app.Toast("Could not start the game: " + err, 4.0f);
                    return;
                }
                app.RequestExit();
            });
}

}  // namespace screens
