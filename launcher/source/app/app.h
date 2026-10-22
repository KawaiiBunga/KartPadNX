// The launcher application: SDL setup, the main loop, the screen stack, and app-wide services
// (theme, sound, overlays, settings). Screen changes are deferred to the end of each frame so a
// screen never frees itself while its own Update is running.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <SDL.h>

#include "app/input.h"
#include "mkw/data.h"
#include "ui/canvas.h"
#include "ui/overlay.h"
#include "ui/theme.h"

namespace screens {
class Screen;
}

namespace app {

class App {
public:
    App();
    ~App();
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Runs until RequestExit(). Returns the process exit code.
    int Run();

    // Deferred stack operations, applied after the current frame.
    void Push(std::unique_ptr<screens::Screen> screen);
    void Pop();
    void Replace(std::unique_ptr<screens::Screen> screen);
    void RequestExit() { exitRequested_ = true; }

    ui::Overlay& overlay() { return overlay_; }
    ui::Canvas& canvas() { return *canvas_; }
    mkw::LauncherSettings& settings() { return settings_; }
    ui::Theme theme() const;

    // True when the launcher was not started as a full application (hbmenu applet, library applet).
    // The game needs full RAM, so the UI warns in that case.
    bool AppletMode() const;

    void Toast(std::string text, float seconds = 2.5f) { overlay_.Toast(std::move(text), seconds); }

private:
    bool Init();
    void Shutdown();
    void ApplyDeferred();
    void Frame(float dt);

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    std::unique_ptr<ui::Canvas> canvas_;
    InputReader input_;
    ui::Overlay overlay_;
    mkw::LauncherSettings settings_;

    enum class OpKind : std::uint8_t { Push, Pop, Replace };
    struct Op {
        OpKind kind;
        std::unique_ptr<screens::Screen> screen;
    };

    std::vector<std::unique_ptr<screens::Screen>> stack_;
    std::vector<Op> ops_;
    bool exitRequested_ = false;
    bool sdlReady_ = false;
};

}  // namespace app
