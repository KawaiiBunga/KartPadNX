// MKW Launcher entry point.
//
// The launcher ships no Nintendo data. It mounts its own romfs (fonts, UI sounds are generated),
// runs the UI loop, and when the user picks a game it hands off with envSetNextLoad (see
// PayloadService::LaunchGame) and returns from main so the loader starts the game.

#include <cstdio>

#include <switch.h>

#include "app/app.h"
#include "mkw/services.h"

int main(int argc, char** argv) {
    if (argc > 0) mkw::SetLauncherSelfPath(argv[0]);

    if (R_FAILED(romfsInit())) {
        std::fprintf(stderr, "[launcher] romfsInit failed\n");
        return 1;
    }

    // socketInitialize / nxlink are left out of the UI build. The updater module brings up
    // sockets when it needs them.
    int rc = 0;
    {
        app::App launcher;
        rc = launcher.Run();
    }

    romfsExit();
    return rc;
}
