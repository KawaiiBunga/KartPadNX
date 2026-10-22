// Where the game keeps things on SD, resolved the way the game does it: Config.toml [paths]
// overrides first, then the defaults from runtime_config.h.
#pragma once

#include <string>
#include <vector>

#include "mkw/services.h"

namespace mkw::paths {

std::string SdRoot();            // "sdmc:" on console; "sdmc:" relative dir in host tests
std::string GameConfigPath();    // .../switch/kartpad/Config.toml
std::string NandRoot();          // [paths] nand_root, default .../switch/kartpad/NAND
std::string DvdRoot();           // [paths] dvd_root, default .../switch/kartpad/data
std::string RetroRewindRoot();   // [paths] retro_rewind_root, default .../switch/kartpad/retro-rewind

// Every place the RR build can keep its save (RRLoadPack and the optional RRSave patch).
std::vector<std::string> RetroRewindSaveCandidates();
// The save file the game uses for `v`. For RR, the newest existing candidate, else the default.
std::string SavePath(Variant v);
// The Wii Mii database both variants read.
std::string MiiDatabasePath();

}  // namespace mkw::paths
