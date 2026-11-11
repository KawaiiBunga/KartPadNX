// Main screen, Play and Mods tabs: game cards, launching, and mod toggles.
#include "screens/main_screen.h"
#include "screens/main_screen_ui.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "app/app.h"
#include "mkw/data.h"
#include "screens/flow.h"
#include "screens/mod_screen.h"

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
        const char* blurb;
        const char* image;
    };
    const Card cards[] = {
        {Variant::Vanilla, "Mario Kart Wii", "The original game", "romfs:/ui/card-vanilla.png"},
        {Variant::RetroRewind, "Retro Rewind", "Custom tracks and Retro WFC online", "romfs:/ui/card-rr.png"},
    };
    constexpr float kCardW = 382.0f, kGap = 40.0f;
    float x = kContent.x + (kContent.w - (2 * kCardW + kGap)) * 0.5f;
    int focusIndex = 0;
    for (const Card& c : cards) {
        const mkw::PayloadInfo* p = FindPayload(payloads, c.v);
        auto* box = Add<ui::ItemBox>(Rect{x, 196, kCardW, 380}, c.title, c.blurb);
        x += kCardW + kGap;
        box->SetImage(c.image);
        box->SetSelected(c.v == selected_);
        const size_t mods = app.settings().mods[static_cast<int>(c.v)].size();
        if (!p) {
            box->SetDisabled("Not in this launcher build");
        } else if (!gameData) {
            box->SetDisabled("Import your disc first");
        } else if (c.v == Variant::RetroRewind && !rrData) {
            box->SetDisabled("Add Retro Rewind in Game Files");
        } else if (mods > 0) {
            box->SetBadge(std::to_string(mods) + (mods == 1 ? " mod" : " mods"), th.accent);
        } else if (c.v == Variant::RetroRewind) {
            box->SetBadge("v" + rrVersion, th.good);
        }
        box->enabled = p && gameData && (c.v == Variant::Vanilla || rrData);
        // A selects the game; A again (or Play) starts it.
        box->OnActivate([this, &app, v = c.v]() {
            if (selected_ == v) {
                Launch(app, v);
                return;
            }
            selected_ = profileVariant_ = v;
            app.settings().lastVariant = v == Variant::RetroRewind ? 1 : 0;
            settingsDirty_ = true;
            Build(app);
        });
        Track(box);
        if (c.v == selected_) focusIndex = static_cast<int>(focus_.Items().size()) - 1;
    }
    focus_.SetIndex(focusIndex);
}

// ---- Mods --------------------------------------------------------------------------------

void MainScreen::BuildMods(app::App& app) {
    const ui::Theme th = app.theme();
    const float x = 252.0f, w = 776.0f;
    AddGameRow(app, x, kBodyTop, w);

    modNames_ = mkw::Settings().ListMods();
    if (modNames_.empty()) {
        auto* msg = Add<ui::Label>("No mods yet. Copy a mod folder to SD:/switch/kartpad/mods, then come back.",
                                   ui::kBody, Align::Center);
        msg->rect = {kContent.x, kBodyTop + 90, kContent.w, 30};
        msg->SetColor(th.textDim);
        auto* how = Add<ui::Label>("A mod folder holds files laid out like the disc (Race/Course/...), "
                                   "or a Riivolution pack with a riivolution folder.",
                                   ui::kSmall, Align::Center);
        how->rect = {kContent.x, kBodyTop + 126, kContent.w, 24};
        how->SetDim(true);
        return;
    }

    const auto& enabled = app.settings().mods[static_cast<int>(profileVariant_)];
    // Five rows fit; a long list scrolls by page with the last row. A opens the mod's own screen.
    constexpr size_t kRows = 5;
    const size_t first = std::min(static_cast<size_t>(modPage_) * kRows, (modNames_.size() - 1) / kRows * kRows);
    float y = kBodyTop + 62;
    for (size_t i = first; i < modNames_.size() && i < first + kRows; ++i) {
        const std::string name = modNames_[i];
        const bool on = std::find(enabled.begin(), enabled.end(), name) != enabled.end();
        auto* row = Add<ui::ListRow>(Rect{x, y, w, 50}, name, [on]() { return std::string(on ? "On" : "Off"); },
                                     [this, &app, name]() { app.Push(std::make_unique<ModScreen>(name, profileVariant_)); });
        row->SetIcon(ui::icon::kTune);
        Track(row);
        y += 56;
    }
    if (modNames_.size() > kRows) {
        Track(Add<ui::Button>(Rect{x, y + 4, w, 42}, "More mods", false, [this, &app]() {
            const int pages = static_cast<int>((modNames_.size() + kRows - 1) / kRows);
            modPage_ = (modPage_ + 1) % pages;
            Build(app);
        }));
    }
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
    mkw::Settings().PickUpNewMods(s);  // folders copied over while the launcher was open
    mkw::Settings().Save(s);
    settingsDirty_ = false;
    if (!mkw::Settings().ApplyMods(v, s)) {
        app.Toast("Could not write the mod list to Config.toml.", 3.5f);
        return;
    }

    // No save yet (first run) is fine; only an existing save that fails to back up stops the launch.
    if (s.backupBeforeLaunch && mkw::Saves().HasSave(v) && !mkw::Saves().BackupCurrent(v)) {
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
