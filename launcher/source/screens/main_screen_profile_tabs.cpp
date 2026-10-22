// Main screen, Saves, Licenses and Miis tabs.
#include "screens/main_screen.h"
#include "screens/main_screen_ui.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "app/app.h"
#include "data/switch_mii.h"
#include "mkw/data.h"
#include "screens/flow.h"
#include "screens/profiles.h"

namespace screens {

using namespace main_ui;

// ---- Saves -------------------------------------------------------------------------------

void MainScreen::BuildSaves(app::App& app) {
    const ui::Theme th = app.theme();
    const float colW = 556.0f;
    const Variant variants[] = {Variant::Vanilla, Variant::RetroRewind};
    for (int c = 0; c < 2; ++c) {
        const Variant v = variants[c];
        const float x = c == 0 ? 72.0f : 652.0f;
        Add<ui::Label>(mkw::VariantDisplayName(v), ui::kTitle)->rect = {x, kBodyTop - 6, colW, 32};

        const std::string savePath = mkw::Saves().GetSavePath(v);
        const bool hasSave = mkw::Saves().HasSave(v);
        auto* status = Add<ui::Label>(hasSave ? "Save found" : "No save yet. Play once to create it.", ui::kSmall);
        status->rect = {x, kBodyTop + 30, colW, 22};
        status->SetColor(hasSave ? th.good : th.textDim);
        auto* where = Add<ui::Label>(ShortPath(savePath), ui::kSmall);
        where->rect = {x, kBodyTop + 52, colW, 22};
        where->SetColor(th.textFaint);

        float y = kBodyTop + 84;
        auto* backup = Add<ui::Button>(Rect{x, y, colW, 48}, "Back up now", true, [this, &app, v]() {
            const bool ok = mkw::Saves().BackupCurrent(v);
            app.Toast(ok ? "Save backed up." : "Backup failed. Check the SD card has free space.", 3.0f);
            Build(app);
        });
        backup->SetIcon(ui::icon::kSave);
        backup->enabled = hasSave;
        Track(backup);
        y += 60;

        const auto slots = mkw::Saves().ListBackups(v);
        if (slots.empty()) {
            auto* none = Add<ui::Label>("No backups yet.", ui::kSmall);
            none->rect = {x, y + 6, colW, 22};
            none->SetDim(true);
        }
        int shown = 0;
        for (auto it = slots.rbegin(); it != slots.rend() && shown < 4; ++it, ++shown) {
            const std::string slot = it->name;
            std::string label = slot;
            if (slot.size() >= 15 && slot[8] == '-') {
                label = slot.substr(0, 4) + "-" + slot.substr(4, 2) + "-" + slot.substr(6, 2) + "  " + slot.substr(9, 2) +
                        ":" + slot.substr(11, 2);
            }
            auto* row = Add<ui::ListRow>(Rect{x, y, colW, 42}, label, []() { return std::string("Restore"); },
                                         [this, &app, v, slot, label]() {
                                             app.overlay().ShowModal(ui::Modal{
                                                 "Restore this backup?",
                                                 "Your current save is backed up first, then replaced by the backup from\n" + label + ".",
                                                 {{"Cancel", nullptr, false},
                                                  {"Restore", [this, &app, v, slot]() {
                                                       const bool ok = mkw::Saves().Restore(v, slot);
                                                       app.Toast(ok ? "Save restored." : "Restore failed. Your save was not changed.", 3.5f);
                                                       RefreshProfile();
                                                       Build(app);
                                                   }, true}}});
                                         });
            row->SetIcon(ui::icon::kRestore);
            Track(row);
            y += 48;
        }
    }
}

// ---- Licenses ----------------------------------------------------------------------------

void MainScreen::BuildLicenses(app::App& app) {
    const ui::Theme th = app.theme();
    const float x = 252.0f, w = 776.0f, h = 52.0f, gap = 60.0f;
    float y = kBodyTop;
    auto* game = Add<ui::ListRow>(Rect{x, y, w, h}, "Game", [this]() { return std::string(mkw::VariantDisplayName(profileVariant_)); },
                                  [this, &app]() {
                                      profileVariant_ = profileVariant_ == Variant::Vanilla ? Variant::RetroRewind : Variant::Vanilla;
                                      Build(app);
                                  });
    game->SetIcon(ui::icon::kFlag);
    Track(game);
    y += gap + 4;

    std::string err;
    if (!mkw::Licenses().Load(profileVariant_, &err)) {
        auto* msg = Add<ui::Label>(err, ui::kBody, Align::Center);
        msg->rect = {kContent.x, y + 20, kContent.w, 30};
        msg->SetColor(th.textDim);
        return;
    }
    if (!mkw::Licenses().Writable()) {
        auto* msg = Add<ui::Label>("This save's checksum format is not recognised, so it is read-only.", ui::kSmall, Align::Center);
        msg->rect = {kContent.x, y - 8, kContent.w, 22};
        msg->SetColor(th.warn);
        y += 18;
    }
    for (const auto& l : mkw::Licenses().List()) {
        const int index = l.index;
        ui::ListRow* row;
        if (!l.used) {
            row = Add<ui::ListRow>(Rect{x, y, w, h}, "License " + std::to_string(index + 1), []() { return std::string("Empty"); },
                                   nullptr);
            row->enabled = false;
        } else {
            char value[48];
            std::snprintf(value, sizeof value, "VR %d   BR %d", l.vr, l.br);
            const std::string v = value;
            row = Add<ui::ListRow>(Rect{x, y, w, h}, l.name, [v]() { return v; },
                                   [&app, index]() { app.Push(std::make_unique<LicenseEditScreen>(index)); });
        }
        row->SetIcon(ui::icon::kPerson);
        Track(row);
        y += gap;
    }
}

// ---- Miis --------------------------------------------------------------------------------

// The Miis both games list when a licence picks one: the Switch's own Miis, copied in at every
// start (data/switch_mii.h), and any made on a Wii. They are made and edited with the Switch's Mii
// editor; a licence's Mii is picked in the game or under Licenses.
void MainScreen::BuildMiis(app::App& app) {
    const ui::Theme th = app.theme();
    const float x = 252.0f, w = 776.0f;

    auto* edit = Add<ui::Button>(Rect{x, kBodyTop, w, 48}, "Make or edit Miis (Switch Mii editor)", true, [this, &app]() {
        std::string err;
        if (!mkw::switch_mii::ShowMiiEditor(&err)) {
            app.Toast(err, 4.0f);
            return;
        }
        int added = 0, updated = 0;
        if (!mkw::Miis().SyncFromSwitch(&added, &updated, &err)) {
            app.Toast(err.empty() ? "Could not copy the Switch's Miis." : err, 4.0f);
        } else if (added + updated > 0) {
            app.Toast(std::to_string(added + updated) + " Mii(s) copied for the games.", 3.0f);
        }
        Build(app);
    });
    edit->SetIcon(ui::icon::kFace);
    Track(edit);

    std::string err;
    const bool loaded = mkw::Miis().Load(&err);
    const auto miis = loaded ? mkw::Miis().List() : std::vector<mkw::MiiEntry>{};
    if (miis.empty()) {
        auto* msg = Add<ui::Label>("No Miis yet. Make one with the Switch's Mii editor; the games will list it.",
                                   ui::kBody, Align::Center);
        msg->rect = {kContent.x, kBodyTop + 90, kContent.w, 30};
        msg->SetColor(th.textDim);
        return;
    }
    auto* hint = Add<ui::Label>("Pick a Mii when you make a licence in the game, or change a licence's Mii under Licenses.",
                                ui::kSmall, Align::Center);
    hint->rect = {kContent.x, kBodyTop + 56, kContent.w, 22};
    hint->SetColor(th.textDim);
    constexpr int kPerPage = 4;
    const int pages = std::max(1, static_cast<int>((miis.size() + kPerPage - 1) / kPerPage));
    miiPage_ = std::clamp(miiPage_, 0, pages - 1);
    float y = kBodyTop + 84;
    for (int i = miiPage_ * kPerPage; i < static_cast<int>(miis.size()) && i < (miiPage_ + 1) * kPerPage; ++i) {
        const mkw::MiiEntry m = miis[static_cast<size_t>(i)];
        const std::string origin = m.creator == mkw::switch_mii::kCreatorName ? "Switch" : "Wii";
        const std::string value = origin + ", " + (m.female ? "female" : "male") + ", " + mkw::MiiColorName(m.favoriteColor);
        auto* row = Add<ui::ListRow>(Rect{x, y, w, 50}, m.name, [value]() { return value; }, nullptr);
        row->SetIcon(ui::icon::kFace);
        y += 56;
    }
    if (pages > 1) {
        auto* prev = Add<ui::Button>(Rect{x, 548, 380, 42}, "Previous page", false, [this, &app]() {
            --miiPage_;
            Build(app);
        });
        prev->enabled = miiPage_ > 0;
        Track(prev);
        auto* next = Add<ui::Button>(Rect{x + 396, 548, 380, 42}, "Next page", false, [this, &app]() {
            ++miiPage_;
            Build(app);
        });
        next->enabled = miiPage_ + 1 < pages;
        Track(next);
    }
}

}  // namespace screens
