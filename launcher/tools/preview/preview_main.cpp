// Host preview of the launcher UI. Renders headless (SDL dummy video, software renderer) and
// saves screenshots, driven by a scripted button sequence instead of a controller.
//
//   MKW_PREVIEW_SCRIPT  comma-separated steps run one per 8 frames:
//                       A B L R up down left right plus, or shot:<name> to save build/preview/<name>.png
//   MKW_PREVIEW_APPLET  set to 1 to preview the low-memory (applet) banner
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <SDL.h>
#include <SDL_image.h>
#include <openssl/sha.h>

#include "app/app.h"
#include "app/input.h"
#include "mkw/services.h"
#include "switch.h"

namespace {

std::vector<std::string>& Steps() {
    static std::vector<std::string> steps = [] {
        std::vector<std::string> out;
        const char* env = std::getenv("MKW_PREVIEW_SCRIPT");
        std::stringstream ss(env ? env : "shot:home");
        for (std::string item; std::getline(ss, item, ',');) out.push_back(item);
        return out;
    }();
    return steps;
}

int g_frame = 0;
size_t g_step = 0;
std::string g_pending;  // button step for the input reader this frame

constexpr int kFramesPerStep = 8;

}  // namespace

AppletType appletGetAppletType() {
    const char* a = std::getenv("MKW_PREVIEW_APPLET");
    return a && a[0] == '1' ? AppletType_LibraryApplet : AppletType_Application;
}

void sha256ContextCreate(Sha256Context* ctx) { SHA256_Init(reinterpret_cast<SHA256_CTX*>(ctx)); }
void sha256ContextUpdate(Sha256Context* ctx, const void* src, std::size_t size) {
    SHA256_Update(reinterpret_cast<SHA256_CTX*>(ctx), src, size);
}
void sha256ContextGetHash(Sha256Context* ctx, void* dst) {
    SHA256_Final(static_cast<unsigned char*>(dst), reinterpret_cast<SHA256_CTX*>(ctx));
}

// Called by App::Run once per frame (MKW_PREVIEW builds). Returns true to stop.
bool PreviewFrame(SDL_Renderer* renderer) {
    ++g_frame;
    if (g_frame % kFramesPerStep != 0) return false;
    if (g_step >= Steps().size()) return true;
    const std::string step = Steps()[g_step++];
    if (step.rfind("shot:", 0) == 0) {
        SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, 1280, 720, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch);
        const std::string path = "build/preview/" + step.substr(5) + ".png";
        IMG_SavePNG(s, path.c_str());
        SDL_FreeSurface(s);
        std::printf("saved %s\n", path.c_str());
    } else {
        g_pending = step;
    }
    return false;
}

namespace app {

void InputReader::Init() { initialised_ = true; }

void InputReader::Read(float dt, Input& out) {
    (void)dt;
    out = Input{};
    if (g_pending.empty()) return;
    const std::string s = g_pending;
    g_pending.clear();
    out.confirm = s == "A";
    out.back = s == "B";
    out.tabPrev = s == "L";
    out.tabNext = s == "R";
    out.plusHeld = s == "plus";
    if (s == "up") out.nav = Dir::Up;
    if (s == "down") out.nav = Dir::Down;
    if (s == "left") out.nav = Dir::Left;
    if (s == "right") out.nav = Dir::Right;
}

}  // namespace app

int main(int argc, char** argv) {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    if (argc > 0) mkw::SetLauncherSelfPath(argv[0]);
    app::App launcher;
    return launcher.Run();
}
