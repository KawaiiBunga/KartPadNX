// Main screen, Game Files and Settings tabs.
#include "screens/main_screen.h"
#include "screens/main_screen_ui.h"

#include <cstdio>
#include <memory>
#include <string>

#include "app/app.h"
#include "mkw/data.h"
#include "screens/flow.h"

namespace screens {

using namespace main_ui;

// ---- Game Files --------------------------------------------------------------------------

void MainScreen::BuildGameFiles(app::App& app) {
    const ui::Theme th = app.theme();
    const float x = 252.0f, w = 776.0f, h = 54.0f, gap = 62.0f;
    const bool gameData = mkw::Disc().HasValidGameData();
    std::string rrVersion;
    const bool rrData = mkw::Disc().HasValidRetroRewind(&rrVersion);

    auto* gameStatus = Add<ui::Label>(gameData ? "Mario Kart Wii: ready" : "Mario Kart Wii: disc not imported", ui::kBodyStrong);
    gameStatus->rect = {x, kBodyTop, w, 28};
    gameStatus->SetColor(gameData ? th.good : th.warn);
    auto* rrStatus = Add<ui::Label>(rrData ? "Retro Rewind: " + rrVersion : "Retro Rewind: not installed", ui::kBodyStrong);
    rrStatus->rect = {x, kBodyTop + 30, w, 28};
    rrStatus->SetColor(rrData ? th.good : th.textDim);
    std::string builds;
    for (const auto& p : mkw::Payloads().List()) {
        builds += (builds.empty() ? "" : "   ") + std::string(mkw::VariantDisplayName(p.variant)) + ": " + ShortStamp(p.buildStamp);
    }
    auto* stamps = Add<ui::Label>(builds, ui::kSmall);
    stamps->rect = {x, kBodyTop + 60, w, 22};
    stamps->SetDim(true);

    float y = kBodyTop + 96;
    auto* a = Add<ui::ListRow>(Rect{x, y, w, h}, "Import disc", []() { return std::string("WBFS, ISO or folder"); },
                               [this, &app]() { app.Push(std::make_unique<ImportScreen>([this, &app](bool) { Build(app); })); });
    a->SetIcon(ui::icon::kFolder);
    Track(a);
    y += gap;
    auto* b = Add<ui::ListRow>(Rect{x, y, w, h}, rrData ? "Update Retro Rewind" : "Add Retro Rewind",
                               []() { return std::string("RetroRewind6 folder"); },
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
    auto* d = Add<ui::ListRow>(Rect{x, y, w, h}, "Free up space", []() { return std::string("~190 MB"); }, [&app]() {
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

namespace {

void InstallUpdate(app::App& app) {
    if (!mkw::Updates().StartDownloadAndInstall()) return;
    PushJob(app, "Installing update", "Downloading and checking the signed release.", []() { return mkw::Updates().Poll(); },
            []() { mkw::Updates().Cancel(); }, [&app](bool ok) {
                if (!ok) return;
                app.overlay().ShowModal(ui::Modal{"Update installed",
                                                  "Restart MKW Launcher to use the new version.",
                                                  {{"Later", nullptr, false},
                                                   {"Restart now", [&app]() {
                                                        if (mkw::Updates().RelaunchIntoNewVersion()) app.RequestExit();
                                                    }, true}}});
            });
}

void OfferUpdate(app::App& app) {
    const mkw::UpdateInfo info = mkw::Updates().Latest();
    if (!info.available) {
        app.Toast("You have the latest version.", 2.5f);
        return;
    }
    char size[32];
    std::snprintf(size, sizeof size, "%.1f MB", static_cast<double>(info.downloadSize) / (1024.0 * 1024.0));
    app.overlay().ShowModal(ui::Modal{"Update to " + info.version + " (" + size + ")?",
                                      info.changelog.empty() ? std::string("No release notes.") : info.changelog,
                                      {{"Later", nullptr, false}, {"Install", [&app]() { InstallUpdate(app); }, true}}});
}

}  // namespace

void MainScreen::BuildSettings(app::App& app) {
    mkw::LauncherSettings& s = app.settings();
    auto dirty = [this](bool) { settingsDirty_ = true; };
    const float left = 72.0f, right = 652.0f, w = 556.0f, h = 46.0f, gap = 54.0f;

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

    y = kBodyTop;
    Track(Add<ui::ToggleRow>(Rect{right, y, w, h}, "Show FPS", &s.showFps, dirty));
    y += gap;
    Track(Add<ui::ToggleRow>(Rect{right, y, w, h}, "Widescreen", &s.widescreen, dirty));
    y += gap;
    Track(Add<ui::ToggleRow>(Rect{right, y, w, h}, "Back up save before playing", &s.backupBeforeLaunch, dirty));
    y += gap;
    Track(Add<ui::ListRow>(Rect{right, y, w, h}, "Launcher colour",
                           [&s]() { return std::string(ui::AccentName(ui::AccentFromIndex(s.accent))); }, [this, &s]() {
                               s.accent = (s.accent + 1) % ui::kAccentCount;
                               settingsDirty_ = true;
                           }));
    y += gap;
    if (mkw::Settings().HasPerformanceOverrides()) {
        Track(Add<ui::ListRow>(Rect{right, y, w, h}, "Reset performance tweaks", []() { return std::string("perf.cfg"); },
                               [this, &app]() {
                                   app.overlay().ShowModal(ui::Modal{
                                       "Reset performance tweaks?",
                                       "Your perf.cfg is kept as perf.cfg.bak and the game goes back to its defaults.",
                                       {{"Cancel", nullptr, false},
                                        {"Reset", [this, &app]() {
                                             app.Toast(mkw::Settings().ResetPerformanceTuning() ? "Back to defaults."
                                                                                                : "Could not reset perf.cfg.",
                                                       3.0f);
                                             Build(app);
                                         }, true}}});
                               }));
        y += gap;
    }
    auto* update = Add<ui::ListRow>(Rect{right, y, w, h}, "Launcher updates", []() {
        const mkw::UpdateInfo info = mkw::Updates().Latest();
        return info.available ? info.version + " available" : std::string("v") + mkw::kLauncherVersion;
    }, [&app]() {
        if (mkw::Updates().Latest().available) {
            OfferUpdate(app);
            return;
        }
        if (!mkw::Updates().StartCheck(true)) return;
        PushJob(app, "Checking for updates", "Asking GitHub for the latest release.", []() { return mkw::Updates().Poll(); },
                []() { mkw::Updates().Cancel(); }, [&app](bool ok) {
                    if (ok) OfferUpdate(app);
                });
    });
    update->SetIcon(ui::icon::kUpdate);
    Track(update);
    settingsDirty_ = true;  // sliders have no change callback
}

}  // namespace screens
