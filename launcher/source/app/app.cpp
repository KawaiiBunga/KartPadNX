#include "app/app.h"

#include <algorithm>
#include <cstdio>

#include <SDL_ttf.h>
#include <switch.h>

#include "mkw/data.h"
#include "mkw/services.h"
#include "screens/flow.h"
#include "screens/main_screen.h"
#include "screens/screen.h"
#include "ui/fonts.h"

#ifdef MKW_PREVIEW
bool PreviewFrame(SDL_Renderer* renderer);
#endif

namespace app {
namespace {

constexpr int kWindowW = 1280;
constexpr int kWindowH = 720;
constexpr float kMaxFrameTime = 0.1f;

}  // namespace

App::App() = default;

App::~App() { Shutdown(); }

ui::Theme App::theme() const {
    return ui::MakeTheme(ui::AccentFromIndex(settings_.accent));
}

bool App::AppletMode() const { return appletGetAppletType() != AppletType_Application; }

bool App::Init() {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "[launcher] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    sdlReady_ = true;
    if (TTF_Init() != 0) {
        std::fprintf(stderr, "[launcher] TTF_Init failed: %s\n", TTF_GetError());
        return false;
    }

    #ifdef MKW_PREVIEW
    window_ = SDL_CreateWindow("MKW Launcher", 0, 0, kWindowW, kWindowH, 0);
#else
    window_ = SDL_CreateWindow("MKW Launcher", 0, 0, kWindowW, kWindowH, SDL_WINDOW_FULLSCREEN);
#endif
    if (!window_) {
        std::fprintf(stderr, "[launcher] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    #ifdef MKW_PREVIEW
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
#else
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
#endif
    if (!renderer_) {
        std::fprintf(stderr, "[launcher] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_RenderSetLogicalSize(renderer_, kWindowW, kWindowH);
    canvas_ = std::make_unique<ui::Canvas>(renderer_);

    input_.Init();
    mkw::Settings().Load(settings_);
    return true;
}

void App::Shutdown() {
    // Order matters: screens and textures first, then fonts, then the subsystems.
    ops_.clear();
    stack_.clear();
    canvas_.reset();
    ui::CloseFonts();
    if (renderer_) {
        SDL_DestroyRenderer(renderer_);
        renderer_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    if (sdlReady_) {
        TTF_Quit();
        SDL_Quit();
        sdlReady_ = false;
    }
}

void App::Push(std::unique_ptr<screens::Screen> screen) {
    ops_.push_back({OpKind::Push, std::move(screen)});
}

void App::Pop() { ops_.push_back({OpKind::Pop, nullptr}); }

void App::Replace(std::unique_ptr<screens::Screen> screen) {
    ops_.push_back({OpKind::Replace, std::move(screen)});
}

void App::ApplyDeferred() {
    for (auto& op : ops_) {
        switch (op.kind) {
            case OpKind::Push:
                if (!stack_.empty()) stack_.back()->OnLeave(*this);
                stack_.push_back(std::move(op.screen));
                stack_.back()->OnEnter(*this);
                break;
            case OpKind::Pop:
                if (!stack_.empty()) {
                    stack_.back()->OnLeave(*this);
                    stack_.pop_back();
                }
                if (!stack_.empty()) stack_.back()->OnEnter(*this);
                break;
            case OpKind::Replace:
                if (!stack_.empty()) {
                    stack_.back()->OnLeave(*this);
                    stack_.pop_back();
                }
                stack_.push_back(std::move(op.screen));
                stack_.back()->OnEnter(*this);
                break;
        }
    }
    ops_.clear();
}

void App::Frame(float dt) {
    Input in;
    input_.Read(dt, in);

    if (overlay_.ModalOpen()) {
        overlay_.HandleInput(in);
    } else if (!stack_.empty()) {
        screens::Screen& top = *stack_.back();
        if (in.back && top.HandleBack(*this)) {
            // The screen consumed B.
        } else if (in.back && stack_.size() > 1) {
            Pop();
        } else {
            top.Update(*this, in, dt);
        }
    }
    overlay_.Update(dt);
    ApplyDeferred();
}

int App::Run() {
    if (!Init()) return 1;

    // Start on the setup wizard until a disc is imported, then on the main screen.
    if (mkw::Disc().HasValidGameData()) {
        // The Switch's Miis into the games' Mii database, so a new licence can pick them.
        int added = 0, updated = 0;
        std::string err;
        if (!mkw::Miis().SyncFromSwitch(&added, &updated, &err) || !err.empty()) {
            std::fprintf(stderr, "[launcher] Switch Miis: %s\n", err.c_str());
        }
        Push(std::make_unique<screens::MainScreen>());
    } else {
        Push(std::make_unique<screens::SetupWizard>());
    }
    ApplyDeferred();

    Uint64 last = SDL_GetPerformanceCounter();
    const double freq = static_cast<double>(SDL_GetPerformanceFrequency());
    while (!exitRequested_) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            // The launcher has no windowed mode; SDL quit events are ignored on Switch.
        }

        const Uint64 now = SDL_GetPerformanceCounter();
        const float dt = std::min(kMaxFrameTime, static_cast<float>(static_cast<double>(now - last) / freq));
        last = now;

        Frame(dt);
        if (exitRequested_) break;

        const ui::Theme th = theme();
        SDL_SetRenderDrawColor(renderer_, th.bg.r, th.bg.g, th.bg.b, 255);
        SDL_RenderClear(renderer_);
        canvas_->FillGradientRoundRect(ui::Rect{0, 0, ui::kLogicalW, ui::kLogicalH}, 0.0f, th.bg, th.bgLow);
        if (!stack_.empty()) stack_.back()->Draw(*this, *canvas_);
        overlay_.Draw(*canvas_, th);
#ifdef MKW_PREVIEW
        // Host preview (launcher/tools/preview): save frames listed in MKW_PREVIEW_SHOTS, then quit.
        if (PreviewFrame(renderer_)) break;
#endif
        SDL_RenderPresent(renderer_);
    }
    return 0;
}

}  // namespace app
