// The home screen: a header, a tab strip (L/R or touch), one content panel per tab, and the
// Launch Game button.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "mkw/services.h"
#include "screens/screen.h"

namespace screens {

class MainScreen : public Screen {
public:
    enum class Tab : int { Play, Mods, Saves, Licenses, Miis, GameFiles, Settings, Count };

    MainScreen() = default;
    void OnEnter(app::App& app) override;
    void OnLeave(app::App& app) override;
    void Update(app::App& app, const app::Input& in, float dt) override;
    void Draw(app::App& app, ui::Canvas& canvas) override;

private:
    void Build(app::App& app);
    void BuildPlay(app::App& app);
    void BuildMods(app::App& app);
    void BuildSaves(app::App& app);
    void BuildLicenses(app::App& app);
    void BuildMiis(app::App& app);
    void BuildGameFiles(app::App& app);
    void BuildSettings(app::App& app);
    // "Game" row that flips profileVariant_ and rebuilds the tab.
    void AddGameRow(app::App& app, float x, float y, float w);
    void SetTab(app::App& app, Tab tab);
    void Launch(app::App& app, mkw::Variant v);

    Tab tab_ = Tab::Play;
    mkw::Variant selected_ = mkw::Variant::Vanilla;        // game the Launch button starts
    mkw::Variant profileVariant_ = mkw::Variant::Vanilla;  // which game the Licenses and Mods tabs edit
    int miiPage_ = 0;
    int modPage_ = 0;
    bool settingsDirty_ = false;
    bool plusWasHeld_ = false;
    std::vector<std::string> modNames_;   // Mods tab rows
    std::unique_ptr<bool[]> modOn_;       // their toggles
};

}  // namespace screens
