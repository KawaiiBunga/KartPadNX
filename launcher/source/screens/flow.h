// Multi-step flows that call the backend services: a generic progress screen, disc and
// Retro Rewind import pickers, and the first-run setup wizard.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "mkw/services.h"
#include "screens/screen.h"
#include "ui/widgets.h"

namespace screens {

// Shows a backend job until it finishes. Cancel calls `cancel`; the job reports its end state.
class JobScreen : public Screen {
public:
    using Poll = std::function<mkw::JobStatus()>;
    using Cancel = std::function<void()>;
    // Called once with success. The owner decides what to do next (usually pops this screen).
    using Done = std::function<void(bool ok)>;

    JobScreen(std::string title, std::string subtitle, Poll poll, Cancel cancel, Done done);

    void OnEnter(app::App& app) override;
    void Update(app::App& app, const app::Input& in, float dt) override;
    bool OnBack(app::App& app) override;

private:
    void Finish(app::App& app, bool ok, const std::string& message);

    std::string title_;
    std::string subtitle_;
    Poll poll_;
    Cancel cancel_;
    Done done_;
    ui::ProgressBar* bar_ = nullptr;
    ui::Label* stage_ = nullptr;
    ui::Label* message_ = nullptr;
    bool finished_ = false;
};

// Lists the discs the service found on SD and starts the import of the chosen one.
// Pops itself when the import ends, then reports the result.
class ImportScreen : public Screen {
public:
    explicit ImportScreen(std::function<void(bool ok)> done);
    void OnEnter(app::App& app) override;

private:
    void Build(app::App& app);
    std::vector<mkw::DiscCandidate> items_;
    std::function<void(bool ok)> done_;
};

// Lists the RetroRewind6 folders the user can import. Same finish behaviour as ImportScreen.
class RrImportScreen : public Screen {
public:
    explicit RrImportScreen(std::function<void(bool ok)> done);
    void OnEnter(app::App& app) override;

private:
    std::function<void(bool ok)> done_;
};

// First-run setup: Welcome -> Disc -> Retro Rewind (optional) -> Driver -> Done -> main screen.
class SetupWizard : public Screen {
public:
    enum class Step : int { Welcome, Disc, RetroRewind, Driver, Done };

    void OnEnter(app::App& app) override;
    bool OnBack(app::App& app) override;

private:
    void Build(app::App& app);

    Step step_ = Step::Welcome;
};

}  // namespace screens
