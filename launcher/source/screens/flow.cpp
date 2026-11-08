#include "screens/flow.h"

#include <cstdio>

#include "app/app.h"
#include "screens/main_screen.h"

namespace screens {
namespace {

using ui::Align;
using ui::VAlign;

constexpr float kCx = 640.0f;

ui::Rect Centered(float w, float h, float y) { return {kCx - w * 0.5f, y, w, h}; }

std::string BaseName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string SizeText(const mkw::DiscCandidate& c) {
    if (c.isExtractedFolder) return "folder";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f GB", static_cast<double>(c.sizeBytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

}  // namespace

// ---- JobScreen ---------------------------------------------------------------------------

JobScreen::JobScreen(std::string title, std::string subtitle, Poll poll, Cancel cancel, Done done)
    : title_(std::move(title)),
      subtitle_(std::move(subtitle)),
      poll_(std::move(poll)),
      cancel_(std::move(cancel)),
      done_(std::move(done)) {}

void JobScreen::OnEnter(app::App& app) {
    (void)app;
    ResetWidgets();
    Add<ui::Label>(title_, ui::kDisplay, Align::Center)->rect = {0, 150, ui::kLogicalW, 60};
    Add<ui::Label>(subtitle_, ui::kBody, Align::Center)->rect = {0, 220, ui::kLogicalW, 40};
    bar_ = Add<ui::ProgressBar>(ui::Rect{kCx - 400, 330, 800, 18});
    stage_ = Add<ui::Label>("Starting...", ui::kBodyStrong, Align::Center);
    stage_->rect = {0, 370, ui::kLogicalW, 40};
    message_ = Add<ui::Label>("", ui::kSmall, Align::Center);
    message_->rect = {0, 420, ui::kLogicalW, 40};
    message_->SetDim(true);
    auto* cancel = Add<ui::Button>(Centered(240, 60, 520), "Cancel", false, [this]() {
        if (cancel_) cancel_();
    });
    Track(cancel);
    Add<ui::HintBar>(std::vector<ui::HintBar::Hint>{{"B", "Cancel"}})->rect = {0, 672, ui::kLogicalW, 48};
}

void JobScreen::Update(app::App& app, const app::Input& in, float dt) {
    Screen::Update(app, in, dt);
    if (finished_) return;
    const mkw::JobStatus s = poll_ ? poll_() : mkw::JobStatus{};
    bar_->SetFraction(s.fraction);
    if (!s.stage.empty()) stage_->SetText(s.stage);
    switch (s.state) {
        case mkw::JobStatus::State::Idle:
        case mkw::JobStatus::State::Running:
            break;
        case mkw::JobStatus::State::Succeeded:
            Finish(app, true, s.message.empty() ? "Done" : s.message);
            break;
        case mkw::JobStatus::State::Failed:
            Finish(app, false, s.message.empty() ? "Failed" : s.message);
            break;
        case mkw::JobStatus::State::Cancelled:
            Finish(app, false, "Cancelled");
            break;
    }
}

bool JobScreen::OnBack(app::App& app) {
    (void)app;
    // B cancels the running job; the job's own end state then closes the screen.
    if (!finished_ && cancel_) cancel_();
    return true;
}

void JobScreen::Finish(app::App& app, bool ok, const std::string& message) {
    finished_ = true;
    message_->SetText(message);
    app.Toast(message, ok ? 2.5f : 4.0f);
    if (done_) done_(ok);
}

// ---- ImportScreen ------------------------------------------------------------------------

ImportScreen::ImportScreen(std::function<void(bool ok)> done) : done_(std::move(done)) {}

void ImportScreen::OnEnter(app::App& app) { Build(app); }

void ImportScreen::Build(app::App& app) {
    ResetWidgets();
    items_ = mkw::Disc().ScanForSources();
    Add<ui::Label>("Choose your game disc", ui::kDisplay, Align::Center)->rect = {0, 60, ui::kLogicalW, 60};

    if (items_.empty()) {
        Add<ui::Label>("No disc found on SD card", ui::kBodyStrong, Align::Center)->rect = {0, 170, ui::kLogicalW, 40};
        Add<ui::Label>("Copy RMCP01.wbfs or .iso to sd:/iso, or an extracted folder to sd:/switch/kartpad/import.",
                       ui::kBody, Align::Center)->rect = {0, 220, ui::kLogicalW, 40};
        auto* rescan = Add<ui::Button>(Centered(240, 60, 300), "Rescan", true, [this, &app]() { Build(app); });
        Track(rescan);
    } else {
        float y = 150;
        for (const auto& c : items_) {
            const std::string name = BaseName(c.path);
            mkw::DiscCandidate cand = c;
            auto* row = Add<ui::ListRow>(ui::Rect{kCx - 460, y, 920, 64}, name,
                                         [cand]() { return SizeText(cand); },
                                         [cand, &app, done = done_]() {
                                             if (!mkw::Disc().StartImport(cand)) {
                                                 app.Toast("Could not start the import", 3.0f);
                                                 return;
                                             }
                                             // Pops the job, then this picker, then reports the result.
                                             app.Push(std::make_unique<JobScreen>(
                                                 "Importing disc",
                                                 "This takes a few minutes. Keep the console awake.",
                                                 []() { return mkw::Disc().Poll(); },
                                                 []() { mkw::Disc().Cancel(); },
                                                 [&app, done](bool ok) {
                                                     app.Pop();
                                                     app.Pop();
                                                     if (done) done(ok);
                                                 }));
                                         });
            Track(row);
            y += 76;
        }
    }
    Add<ui::HintBar>(std::vector<ui::HintBar::Hint>{{"A", "Select"}, {"B", "Back"}})->rect = {0, 672, ui::kLogicalW, 48};
}

// ---- RrImportScreen ----------------------------------------------------------------------

namespace {
// Where the user is expected to copy the RetroRewind6 folder.
constexpr const char* kRrCandidates[] = {
    "sdmc:/switch/kartpad/retro-rewind/RetroRewind6",
    "sdmc:/RetroRewind6",
};
}  // namespace

RrImportScreen::RrImportScreen(std::function<void(bool ok)> done) : done_(std::move(done)) {}

void RrImportScreen::OnEnter(app::App& app) {
    ResetWidgets();
    Add<ui::Label>("Import Retro Rewind", ui::kDisplay, Align::Center)->rect = {0, 60, ui::kLogicalW, 60};
    Add<ui::Label>("Copy the RetroRewind6 folder to the SD card, then pick it here.",
                   ui::kBody, Align::Center)->rect = {0, 130, ui::kLogicalW, 40};
    Add<ui::Label>("It must be the Retro Rewind version your launcher was built for. Newer version? Rebuild first.",
                   ui::kSmall, Align::Center)->rect = {0, 166, ui::kLogicalW, 24};
    float y = 200;
    for (const char* path : kRrCandidates) {
        const std::string p = path;
        auto* row = Add<ui::ListRow>(ui::Rect{kCx - 460, y, 920, 64}, p, []() { return std::string("Import"); },
                                     [p, &app, done = done_]() {
                                         if (!mkw::Disc().StartRetroRewindImport(p)) {
                                             app.Toast("Could not start the Retro Rewind import", 3.0f);
                                             return;
                                         }
                                         app.Push(std::make_unique<JobScreen>(
                                             "Importing Retro Rewind",
                                             "Checking the XML and Code.pul before staging.",
                                             []() { return mkw::Disc().Poll(); },
                                             []() { mkw::Disc().Cancel(); },
                                             [&app, done](bool ok) {
                                                 app.Pop();
                                                 app.Pop();
                                                 if (done) done(ok);
                                             }));
                                     });
        Track(row);
        y += 76;
    }
    Add<ui::HintBar>(std::vector<ui::HintBar::Hint>{{"A", "Import"}, {"B", "Back"}})->rect = {0, 672, ui::kLogicalW, 48};
}

// ---- SetupWizard -------------------------------------------------------------------------

void SetupWizard::OnEnter(app::App& app) {
    // Returning from the disc picker: advance once the disc is valid.
    if (step_ == Step::Disc && mkw::Disc().HasValidGameData()) step_ = Step::RetroRewind;
    Build(app);
}

bool SetupWizard::OnBack(app::App& app) {
    if (step_ == Step::Welcome || step_ == Step::Done) return false;
    step_ = static_cast<Step>(static_cast<int>(step_) - 1);
    Build(app);
    return true;
}

void SetupWizard::Build(app::App& app) {
    ResetWidgets();
    auto title = [this](const char* t) {
        Add<ui::Label>(t, ui::kDisplay, Align::Center)->rect = {0, 80, ui::kLogicalW, 60};
    };
    auto body = [this](const char* t, float y) {
        Add<ui::Label>(t, ui::kBody, Align::Center)->rect = {0, y, ui::kLogicalW, 40};
    };
    // Step dots: one per step, the current one lit.
    for (int i = 0; i < 4; ++i) {
        Add<ui::Panel>(ui::Rect{kCx - 60 + i * 40.0f, 40, 20, 8}, 4, i == static_cast<int>(step_))->focusable = false;
    }

    switch (step_) {
        case Step::Welcome: {
            title("Welcome to MKW Launcher");
            body("First, import your Mario Kart Wii PAL disc from the SD card.", 180);
            auto* next = Add<ui::Button>(Centered(260, 64, 340), "Begin", true, [this, &app]() {
                step_ = Step::Disc;
                Build(app);
            });
            Track(next);
            break;
        }
        case Step::Disc: {
            title("Import your game disc");
            body("Pick your Mario Kart Wii PAL (RMCP01) image or an extracted folder from SD.", 180);
            auto* choose = Add<ui::Button>(Centered(260, 64, 300), "Choose disc", true, [&app]() {
                app.Push(std::make_unique<ImportScreen>(nullptr));
            });
            Track(choose);
            break;
        }
        case Step::RetroRewind: {
            title("Retro Rewind (optional)");
            std::string version;
            const bool rr = mkw::Disc().HasValidRetroRewind(&version);
            if (rr) {
                body(("Found Retro Rewind " + version + ".").c_str(), 180);
                Track(Add<ui::Button>(Centered(260, 64, 300), "Next", true, [this, &app]() {
                    step_ = Step::Done;
                    Build(app);
                }));
            } else {
                body("Retro Rewind is optional. You can add it later in Game Files.", 180);
                Track(Add<ui::Button>(Centered(260, 64, 300), "Import Retro Rewind", true, [&app]() {
                    app.Push(std::make_unique<RrImportScreen>(nullptr));
                }));
                Track(Add<ui::Button>(Centered(260, 64, 390), "Skip", false, [this, &app]() {
                    step_ = Step::Done;
                    Build(app);
                }));
            }
            break;
        }
        case Step::Done: {
            title("All set");
            body("Pick a game and press Play. L / R switch tabs.", 180);
            Track(Add<ui::Button>(Centered(260, 64, 300), "Start", true, [&app]() {
                mkw::Settings().Save(app.settings());
                app.Replace(std::make_unique<MainScreen>());
            }));
            break;
        }
    }
    Add<ui::HintBar>(std::vector<ui::HintBar::Hint>{{"A", "Select"}, {"B", "Back"}})->rect = {0, 672, ui::kLogicalW, 48};
}

}  // namespace screens
