#include "screens/profiles.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

#include "app/app.h"
#include "ui/keyboard.h"

namespace screens {
namespace {

using ui::Align;

constexpr float kLeft = 240.0f;
constexpr float kWidth = 800.0f;
constexpr float kRowH = 58.0f;
constexpr float kGap = 66.0f;
constexpr float kTop = 170.0f;
constexpr int kPerPage = 7;

}  // namespace

void DrawPageHeader(ui::Canvas& canvas, const ui::Theme& th, const std::string& title, const std::string& subtitle) {
    canvas.Text(ui::kDisplay, title, 64, 48, th.text);
    canvas.Text(ui::kSmall, subtitle, 66, 104, th.textDim);
    canvas.FillRect(ui::Rect{64, 140, ui::kLogicalW - 128, 1}, th.edge);
}

// ---- LicenseEditScreen -------------------------------------------------------------------

void LicenseEditScreen::OnEnter(app::App& app) {
    // Only take the stored ratings the first time; returning from the Mii picker keeps slider edits.
    if (!ratingsLoaded_) {
        ratingsLoaded_ = true;
        for (const auto& l : mkw::Licenses().List()) {
        if (l.index == index_) {
                vr_ = l.vr;
                br_ = l.br;
            }
        }
    }
    Build(app);
}

void LicenseEditScreen::Build(app::App& app) {
    ResetWidgets();
    mkw::LicenseEntry lic;
    for (const auto& l : mkw::Licenses().List()) {
        if (l.index == index_) lic = l;
    }
    const bool writable = mkw::Licenses().Writable();
    float y = kTop;

    auto* name = Add<ui::ListRow>(ui::Rect{kLeft, y, kWidth, kRowH}, "Name", [lic]() { return lic.name; },
                                  [this, &app, lic]() {
                                      std::string out;
                                      if (ui::PromptText("License name (10 characters)", lic.name, 10, out)) {
                                          if (!mkw::Licenses().SetName(index_, out)) app.Toast("That name cannot be used.", 3.0f);
                                          Build(app);
                                      }
                                  });
    name->enabled = writable;
    Track(name);
    y += kGap;

    auto* mii = Add<ui::ListRow>(ui::Rect{kLeft, y, kWidth, kRowH}, "Mii", []() { return std::string("Choose a Mii"); },
                                 [this, &app]() {
                                     std::string err;
                                     if (!mkw::Miis().Loaded() && !mkw::Miis().Load(&err)) {
                                         app.Toast(err, 4.0f);
                                         return;
                                     }
                                     app.Push(std::make_unique<MiiPickerScreen>([this, &app](const mkw::MiiEntry& m) {
                                         if (mkw::Licenses().LinkMii(index_, m)) {
                                             app.Toast("License now uses " + m.name + ".", 3.0f);
                                         }
                                         app.Pop();
                                     }));
                                 });
    mii->enabled = writable;
    Track(mii);
    y += kGap;

    auto* vr = Add<ui::SliderRow>(ui::Rect{kLeft, y, kWidth, kRowH}, "VR (race rating)", &vr_, 1, 9999, 100);
    vr->enabled = writable;
    Track(vr);
    y += kGap;
    auto* br = Add<ui::SliderRow>(ui::Rect{kLeft, y, kWidth, kRowH}, "BR (battle rating)", &br_, 1, 9999, 100);
    br->enabled = writable;
    Track(br);
    y += kGap;

    auto* unlock = Add<ui::ListRow>(ui::Rect{kLeft, y, kWidth, kRowH}, "Unlock everything",
                                    []() { return std::string("Characters, karts, cups"); }, [this, &app]() {
                                        app.overlay().ShowModal(ui::Modal{
                                            "Unlock everything?",
                                            "Sets every character, vehicle and cup unlock for this license.\nYour save is backed up when you press Save.",
                                            {{"Cancel", nullptr, false},
                                             {"Unlock", [this, &app]() {
                                                  mkw::Licenses().UnlockEverything(index_);
                                                  app.Toast("Unlocked. Press Save to keep it.", 3.0f);
                                              }, true}}});
                                    });
    unlock->enabled = writable;
    Track(unlock);
    y += kGap + 10;

    auto* save = Add<ui::Button>(ui::Rect{kLeft, y, 390, 60}, "Save", true, [this, &app]() {
        mkw::Licenses().SetRatings(index_, vr_, br_);
        std::string err;
        if (mkw::Licenses().Save(&err)) {
            app.Toast("License saved.", 3.0f);
            app.Pop();
        } else {
            app.Toast(err, 4.5f);
        }
    });
    save->enabled = writable;
    Track(save);
    Track(Add<ui::Button>(ui::Rect{kLeft + 410, y, 390, 60}, "Cancel", false, [&app]() {
        mkw::Licenses().Load(mkw::Licenses().LoadedVariant(), nullptr);  // drop edits
        app.Pop();
    }));

    AddHints({{"A", "Select"}, {"<>", "Adjust"}, {"B", "Back"}});
}

bool LicenseEditScreen::OnBack(app::App& app) {
    if (!mkw::Licenses().Dirty()) return false;
    app.overlay().ShowModal(ui::Modal{"Discard changes?", "Your edits to this license are not saved.",
                                      {{"Keep editing", nullptr, false},
                                       {"Discard", [&app]() {
                                            mkw::Licenses().Load(mkw::Licenses().LoadedVariant(), nullptr);
                                            app.Pop();
                                        }, true}}});
    return true;
}

void LicenseEditScreen::Draw(app::App& app, ui::Canvas& canvas) {
    const ui::Theme th = app.theme();
    std::string sub = std::string(mkw::VariantDisplayName(mkw::Licenses().LoadedVariant())) + " save, license " +
                      std::to_string(index_ + 1);
    if (!mkw::Licenses().Writable()) sub += "  (read-only: unrecognised checksum)";
    DrawPageHeader(canvas, th, "Edit License", sub);
    Screen::Draw(app, canvas);
}

// ---- MiiPickerScreen ---------------------------------------------------------------------

void MiiPickerScreen::OnEnter(app::App& app) { Build(app); }

void MiiPickerScreen::Build(app::App& app) {
    ResetWidgets();
    const auto miis = mkw::Miis().List();
    const int pages = std::max(1, static_cast<int>((miis.size() + kPerPage - 1) / kPerPage));
    page_ = std::min(page_, pages - 1);
    float y = kTop;
    for (int i = page_ * kPerPage; i < static_cast<int>(miis.size()) && i < (page_ + 1) * kPerPage; ++i) {
        const mkw::MiiEntry m = miis[static_cast<size_t>(i)];
        Track(Add<ui::ListRow>(ui::Rect{kLeft, y, kWidth, 54}, m.name,
                               [m]() { return std::string(mkw::MiiColorName(m.favoriteColor)); },
                               [this, m]() { pick_(m); }));
        y += 60;
    }
    if (pages > 1) {
        auto* prev = Add<ui::Button>(ui::Rect{kLeft, 600, 390, 54}, "Previous page", false, [this, &app]() {
            --page_;
            Build(app);
        });
        prev->enabled = page_ > 0;
        Track(prev);
        auto* next = Add<ui::Button>(ui::Rect{kLeft + 410, 600, 390, 54}, "Next page", false, [this, &app]() {
            ++page_;
            Build(app);
        });
        next->enabled = page_ + 1 < pages;
        Track(next);
    }
    AddHints({{"A", "Choose"}, {"B", "Back"}});
}

void MiiPickerScreen::Draw(app::App& app, ui::Canvas& canvas) {
    DrawPageHeader(canvas, app.theme(), "Choose a Mii", "The license takes this Mii's name and face.");
    Screen::Draw(app, canvas);
}

}  // namespace screens
