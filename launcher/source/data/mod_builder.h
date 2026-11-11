// Mod files that belong inside game archives: models and icons inside .szs archives, voice
// clips inside the sound bank. Mod authors expect players to inject these with editing tools;
// the launcher does it before a launch, writing patched copies of the archives.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mkw/data.h"

namespace mkw::modbuild {

using Bytes = std::vector<uint8_t>;

// ---- Formats (exposed for the host tests) ----
Bytes Yaz0Decode(const Bytes& in);  // input returned unchanged when it isn't Yaz0
Bytes Yaz0Encode(const Bytes& raw);

struct U8Archive {
    struct Node {
        uint8_t type = 0;  // 0 file, 1 directory
        uint32_t nameOffset = 0, a = 0, b = 0;
        std::string path;  // full path inside the archive, e.g. "./award/timg/tt_donky_64x64.tpl"
        Bytes data;        // files only
    };
    std::vector<Node> nodes;
    Bytes strings;
};
bool U8Parse(const Bytes& raw, U8Archive& out);
Bytes U8Write(const U8Archive& a);
// A new archive holding `files` (paths like "./button/timg/x.tpl"), directories in first-seen order.
U8Archive U8Build(const std::vector<std::pair<std::string, Bytes>>& files);

// RGBA8 pixels of a PNG (8-bit, non-interlaced: grey, RGB, palette, grey+alpha, RGBA).
bool PngDecode(const Bytes& png, Bytes& rgba, int& w, int& h);
// A copy of `tpl` whose first image is `rgba` (scaled to the TPL's size), stored as RGB5A3.
bool TplReplace(const Bytes& tpl, const Bytes& rgba, int w, int h, Bytes& out);

// One file of the sound bank to replace: its group's name and the file's index (BrawlBox's
// "<index> <group>.brwsd" export names; -1 when unknown), and the exported data (header file
// followed by its wave archive).
struct SoundFile {
    std::string group;
    long index = -1;
    Bytes data;
};
// Writes a copy of the sound bank `base` to `out` with `files` replaced. Replaced data is
// appended and the bank's tables pointed at it; everything else stays byte-identical.
// `log` gets a line per file. Returns how many files were replaced (0 means `out` isn't written).
int PatchSoundArchive(const std::string& base, const std::string& out, const std::vector<SoundFile>& files,
                      std::string& log);

// ---- The build ----
struct Result {
    std::string root;                // overlay root holding the patched files ("" = none needed)
    std::vector<std::string> files;  // disc paths written under root, e.g. "/Scene/Model/Driver.szs"
};
// `modDirs` are the enabled My Stuff (loose-file) mod folders, highest precedence first. Also
// applies Wheel Wizard / Pulsar patch files ("<name>.<Archive>.szs", "<member>.<Archive>").
// `baseDirs`: enabled mods laid out like the disc, highest precedence first; an archive they replace
// is patched from their copy, not the game's. Rebuilds only when a mod file or a base archive changed.
Result Build(Variant v, const std::vector<std::string>& modDirs, const std::vector<std::string>& baseDirs = {});

}  // namespace mkw::modbuild
