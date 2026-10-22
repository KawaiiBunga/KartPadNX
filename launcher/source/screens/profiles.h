// License and Mii editors, pushed from the Licenses and Miis tabs.
#pragma once

#include <functional>
#include <vector>

#include "mkw/data.h"
#include "screens/screen.h"

namespace screens {

// Edits one license of the save loaded in mkw::Licenses(). Changes are kept in memory until Save.
class LicenseEditScreen : public Screen {
public:
    explicit LicenseEditScreen(int index) : index_(index) {}
    void OnEnter(app::App& app) override;
    bool OnBack(app::App& app) override;
    void Draw(app::App& app, ui::Canvas& canvas) override;

private:
    void Build(app::App& app);
    int index_;
    int vr_ = 5000;
    int br_ = 5000;
    bool ratingsLoaded_ = false;
};

// Lists the Miis so one can be chosen (used to give a license a Mii).
class MiiPickerScreen : public Screen {
public:
    using Pick = std::function<void(const mkw::MiiEntry&)>;
    explicit MiiPickerScreen(Pick pick) : pick_(std::move(pick)) {}
    void OnEnter(app::App& app) override;
    void Draw(app::App& app, ui::Canvas& canvas) override;

private:
    void Build(app::App& app);
    Pick pick_;
    int page_ = 0;
};

// Shared page header drawn by the editor screens.
void DrawPageHeader(ui::Canvas& canvas, const ui::Theme& th, const std::string& title, const std::string& subtitle);

}  // namespace screens
