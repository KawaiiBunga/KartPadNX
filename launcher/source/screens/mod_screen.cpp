#include "screens/mod_screen.h"

#include <algorithm>
#include <string>

#include "app/app.h"
#include "screens/profiles.h"

namespace screens {
namespace {

constexpr float kLeft = 240.0f, kWidth = 800.0f, kRowH = 52.0f, kGap = 58.0f;
constexpr int kPerPage = 7;

}  // namespace

void ModScreen::OnEnter(app::App& app) {
    if (!loaded_) {
        loaded_ = true;
        const auto& list = app.settings().mods[static_cast<int>(game_)];
        on_ = std::find(list.begin(), list.end(), mod_) != list.end();
        options_ = mkw::Settings().ModOptions(mod_);
    }
    Build(app);
}

void ModScreen::OnLeave(app::App& app) {
    auto& list = app.settings().mods[static_cast<int>(game_)];
    list.erase(std::remove(list.begin(), list.end(), mod_), list.end());
    if (on_) list.push_back(mod_);
    const bool ok = mkw::Settings().Save(app.settings()) && mkw::Settings().SaveModOptions(mod_, options_);
    if (!ok) app.Toast("Could not save the mod settings to the SD card.", 3.5f);
}

void ModScreen::Draw(app::App& app, ui::Canvas& canvas) {
    const std::string kind = options_.empty() ? "File mod" : "Riivolution pack, " + std::to_string(options_.size()) + " options";
    DrawPageHeader(canvas, app.theme(), mod_, kind + "  \xC2\xB7  " + mkw::VariantDisplayName(game_));
    Screen::Draw(app, canvas);
}

void ModScreen::Build(app::App& app) {
    ResetWidgets();
    float y = 160.0f;
    Track(Add<ui::ToggleRow>(ui::Rect{kLeft, y, kWidth, kRowH},
                             std::string("Use with ") + mkw::VariantDisplayName(game_), &on_));
    y += kGap + 10;

    const int pages = std::max(1, static_cast<int>((options_.size() + kPerPage - 1) / kPerPage));
    page_ = std::clamp(page_, 0, pages - 1);
    for (size_t i = page_ * kPerPage; i < options_.size() && i < static_cast<size_t>((page_ + 1) * kPerPage); ++i) {
        mkw::ModOption& o = options_[i];
        std::vector<std::string> labels = {"Off"};
        labels.insert(labels.end(), o.choices.begin(), o.choices.end());
        // A choice that swaps the game's code files can't work in this port: shown, but kept off.
        auto* row = Add<ui::ChoiceRow>(ui::Rect{kLeft, y, kWidth, kRowH},
                                       o.replacesCode ? o.name + " (can't be used here)" : o.name, &o.selected, 0,
                                       static_cast<int>(o.choices.size()), labels);
        row->enabled = !o.replacesCode;
        Track(row);
        y += kGap;
    }
    if (pages > 1) {
        Track(Add<ui::Button>(ui::Rect{kLeft, y + 4, kWidth, 42}, "More options", false, [this, &app, pages]() {
            page_ = (page_ + 1) % pages;
            Build(app);
        }));
    }
    AddHints({{"A", "Change"}, {"B", "Save and go back"}});
}

}  // namespace screens
