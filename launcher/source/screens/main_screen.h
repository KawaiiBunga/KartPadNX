// The home screen, laid out like a desktop game launcher: a header, a tab strip (L/R or touch),
// one content panel per tab, and a bottom bar with the profile, status and Launch Game button.
#pragma once

#include <string>

#include "mkw/services.h"
#include "screens/screen.h"

namespace screens {

class MainScreen : public Screen {
public:
    enum class Tab : int { Play, Saves, Licenses, Miis, GameFiles, Settings, Updates, Count };

    MainScreen() = default;
    void OnEnter(app::App& app) override;
    void OnLeave(app::App& app) override;
    void Update(app::App& app, const app::Input& in, float dt) override;
    void Draw(app::App& app, ui::Canvas& canvas) override;

private:
    void Build(app::App& app);
    void BuildBottomBar(app::App& app);
    void BuildPlay(app::App& app);
    void BuildSaves(app::App& app);
    void BuildLicenses(app::App& app);
    void BuildMiis(app::App& app);
    void BuildGameFiles(app::App& app);
    void BuildSettings(app::App& app);
    void BuildUpdates(app::App& app);
    void SetTab(app::App& app, Tab tab);
    void Launch(app::App& app, mkw::Variant v);
    void RefreshProfile();

    Tab tab_ = Tab::Play;
    mkw::Variant selected_ = mkw::Variant::Vanilla;        // game the Launch button starts
    mkw::Variant profileVariant_ = mkw::Variant::Vanilla;  // which save the Licenses tab edits
    int miiPage_ = 0;
    bool settingsDirty_ = false;
    bool plusWasHeld_ = false;
    std::string profileName_;
};

}  // namespace screens
