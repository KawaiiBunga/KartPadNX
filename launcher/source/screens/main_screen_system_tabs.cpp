// Main screen, Game Files, Settings and Updates tabs.
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

// ---- Game Files --------------------------------------------------------------------------

void MainScreen::BuildGameFiles(app::App& app) {
    const ui::Theme th = app.theme();
    const float x = 252.0f, w = 776.0f, h = 54.0f, gap = 62.0f;
    const bool gameData = mkw::Disc().HasValidGameData();
    std::string rrVersion;
    const bool rrData = mkw::Disc().HasValidRetroRewind(&rrVersion);

    auto* gameStatus = Add<ui::Label>(gameData ? "Mario Kart Wii: ready (PAL disc files checked)" : "Mario Kart Wii: disc not imported",
                                      ui::kBodyStrong);
    gameStatus->rect = {x, kBodyTop - 4, w, 28};
    gameStatus->SetColor(gameData ? th.good : th.warn);
    auto* rrStatus = Add<ui::Label>(rrData ? "Retro Rewind: version " + rrVersion + " installed" : "Retro Rewind: not installed",
                                    ui::kBodyStrong);
    rrStatus->rect = {x, kBodyTop + 26, w, 28};
    rrStatus->SetColor(rrData ? th.good : th.textDim);

    float y = kBodyTop + 70;
    auto* a = Add<ui::ListRow>(Rect{x, y, w, h}, "Import disc", []() { return std::string("WBFS, ISO or folder"); },
                               [this, &app]() { app.Push(std::make_unique<ImportScreen>([this, &app](bool) { Build(app); })); });
    a->SetIcon(ui::icon::kFolder);
    Track(a);
    y += gap;
    auto* b = Add<ui::ListRow>(Rect{x, y, w, h}, "Add Retro Rewind", []() { return std::string("RetroRewind6 folder"); },
                               [this, &app]() { app.Push(std::make_unique<RrImportScreen>([this, &app](bool) { Build(app); })); });
    b->SetIcon(ui::icon::kRestore);
    Track(b);
    y += gap;
    auto* c = Add<ui::ListRow>(Rect{x, y, w, h}, "Check game files", nullptr, [this, &app]() {
        if (!mkw::Disc().StartVerify()) return;
        PushJob(app, "Checking game files", "Comparing main.dol and StaticR.rel with the PAL disc.",
                []() { return mkw::Disc().Poll(); }, []() { mkw::Disc().Cancel(); }, [this, &app](bool) { Build(app); });
    });
    c->SetIcon(ui::icon::kCheck);
    Track(c);
    y += gap;
    auto* d = Add<ui::ListRow>(Rect{x, y, w, h}, "Free up space", []() { return std::string("Unpacked games, ~190 MB"); }, [&app]() {
        app.overlay().ShowModal(ui::Modal{
            "Free up space?",
            "Removes the unpacked game copies (about 190 MB).\nThey are unpacked again the next time you play.",
            {{"Cancel", nullptr, false},
             {"Remove", [&app]() {
                  app.Toast(mkw::Payloads().RemoveUnpacked() ? "Space freed." : "Could not remove the files.", 3.0f);
              }, true}}});
    });
    d->SetIcon(ui::icon::kTune);
    Track(d);
}

// ---- Settings ----------------------------------------------------------------------------

void MainScreen::BuildSettings(app::App& app) {
    mkw::LauncherSettings& s = app.settings();
    auto dirty = [this](bool) { settingsDirty_ = true; };
    const float left = 72.0f, right = 652.0f, w = 556.0f, h = 46.0f, gap = 52.0f;

    float y = kBodyTop;
    Track(Add<ui::SliderRow>(Rect{left, y, w, h}, "Volume", &s.masterVolume, 0, 100, 5, "%"));
    y += gap;
    Track(Add<ui::SliderRow>(Rect{left, y, w, h}, "Music", &s.musicVolume, 0, 100, 5, "%"));
    y += gap;
    Track(Add<ui::SliderRow>(Rect{left, y, w, h}, "Sound effects", &s.effectsVolume, 0, 100, 5, "%"));
    y += gap;
    Track(Add<ui::SliderRow>(Rect{left, y, w, h}, "Voices", &s.voicesVolume, 0, 100, 5, "%"));
    y += gap;
    Track(Add<ui::ToggleRow>(Rect{left, y, w, h}, "Mute", &s.muted, dirty));
    y += gap;
    Track(Add<ui::ToggleRow>(Rect{left, y, w, h}, "Show FPS", &s.showFps, dirty));

    y = kBodyTop;
    Track(Add<ui::ToggleRow>(Rect{right, y, w, h}, "Widescreen", &s.widescreen, dirty));
    y += gap;
    Track(Add<ui::ListRow>(Rect{right, y, w, h}, "Launcher colour",
                           [&s]() { return std::string(ui::AccentName(ui::AccentFromIndex(s.accent))); }, [this, &s]() {
                               s.accent = (s.accent + 1) % ui::kAccentCount;
                               settingsDirty_ = true;
                           }));
    y += gap;
    Track(Add<ui::ToggleRow>(Rect{right, y, w, h}, "Back up save before playing", &s.backupBeforeLaunch, dirty));
    y += gap;
    Track(Add<ui::ToggleRow>(Rect{right, y, w, h}, "Audio helper thread", &s.mixWorker, dirty));
    y += gap;
    Track(Add<ui::ListRow>(Rect{right, y, w, h}, "Performance tweaks",
                           []() { return std::string(mkw::Settings().HasPerformanceOverrides() ? "Custom" : "Default"); }, [&app]() {
                               if (!mkw::Settings().HasPerformanceOverrides()) {
                                   app.Toast("Already using the default performance settings.", 2.5f);
                                   return;
                               }
                               app.overlay().ShowModal(ui::Modal{
                                   "Reset performance tweaks?",
                                   "Your perf.cfg is kept as perf.cfg.bak and the game goes back to its defaults.",
                                   {{"Cancel", nullptr, false},
                                    {"Reset", [&app]() {
                                         app.Toast(mkw::Settings().ResetPerformanceTuning() ? "Back to defaults." : "Could not reset perf.cfg.", 3.0f);
                                     }, true}}});
                           }));
    settingsDirty_ = true;  // sliders have no change callback
}

// ---- Updates -----------------------------------------------------------------------------

void MainScreen::BuildUpdates(app::App& app) {
    const ui::Theme th = app.theme();
    const float x = 252.0f, w = 776.0f;
    Add<ui::Label>(std::string("MKW Launcher ") + mkw::kLauncherVersion + " by " + mkw::kLauncherAuthor, ui::kBodyStrong)
        ->rect = {x, kBodyTop - 4, w, 28};
    float y = kBodyTop + 28;
    for (const auto& p : mkw::Payloads().List()) {
        auto* l = Add<ui::Label>(std::string(mkw::VariantDisplayName(p.variant)) + ": " + ShortStamp(p.buildStamp), ui::kSmall);
        l->rect = {x, y, w, 24};
        l->SetDim(true);
        y += 24;
    }

    const mkw::UpdateInfo info = mkw::Updates().Latest();
    y += 14;
    auto* check = Add<ui::Button>(Rect{x, y, w, 52}, "Check for updates", !info.available, [this, &app]() {
        if (!mkw::Updates().StartCheck(true)) return;
        PushJob(app, "Checking for updates", "Asking GitHub for the latest release.", []() { return mkw::Updates().Poll(); },
                []() { mkw::Updates().Cancel(); }, [this, &app](bool) { Build(app); });
    });
    check->SetIcon(ui::icon::kUpdate);
    Track(check);
    y += 66;

    if (info.available) {
        char size[32];
        std::snprintf(size, sizeof size, "%.1f MB", static_cast<double>(info.downloadSize) / (1024.0 * 1024.0));
        auto* head = Add<ui::Label>("New version: " + info.version + " (" + size + ")", ui::kBodyStrong);
        head->rect = {x, y, w, 28};
        head->SetColor(th.good);
        y += 32;
        auto* notes = Add<ui::Label>(info.changelog.empty() ? std::string("No release notes.") : info.changelog, ui::kSmall);
        notes->rect = {x, y, w, 60};
        notes->SetDim(true);
        y += 44;
        Track(Add<ui::Button>(Rect{x, y, w, 52}, "Download and install", true, [&app]() {
            if (!mkw::Updates().StartDownloadAndInstall()) return;
            PushJob(app, "Installing update", "Downloading and checking the signed release.",
                    []() { return mkw::Updates().Poll(); }, []() { mkw::Updates().Cancel(); }, [&app](bool ok) {
                        if (!ok) return;
                        app.overlay().ShowModal(ui::Modal{
                            "Update installed",
                            "Restart MKW Launcher to use the new version.",
                            {{"Later", nullptr, false},
                             {"Restart now", [&app]() {
                                  if (mkw::Updates().RelaunchIntoNewVersion()) app.RequestExit();
                              }, true}}});
                    });
        }));
    } else {
        auto* none = Add<ui::Label>("No update found yet. Press Check for updates.", ui::kSmall);
        none->rect = {x, y, w, 24};
        none->SetDim(true);
    }
}

}  // namespace screens
