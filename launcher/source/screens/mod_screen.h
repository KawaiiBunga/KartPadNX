// One mod, opened from the Mods tab: a switch for the mod, then (for a Riivolution pack) each of
// its options with Off or one of its choices, like the Riivolution menu. Saved on leaving.
#pragma once

#include <string>
#include <vector>

#include "mkw/data.h"
#include "screens/screen.h"

namespace screens {

class ModScreen : public Screen {
public:
    ModScreen(std::string mod, mkw::Variant game) : mod_(std::move(mod)), game_(game) {}
    void OnEnter(app::App& app) override;
    void OnLeave(app::App& app) override;
    void Draw(app::App& app, ui::Canvas& canvas) override;

private:
    void Build(app::App& app);

    std::string mod_;
    mkw::Variant game_;
    bool loaded_ = false;
    bool on_ = false;
    std::vector<mkw::ModOption> options_;
    int page_ = 0;
};

}  // namespace screens
