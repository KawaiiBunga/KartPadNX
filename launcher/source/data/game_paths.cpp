#include "data/game_paths.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace mkw::paths {
namespace {

std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

// Reads a string value from `[section] key = "value"` in the game's Config.toml.
std::string ConfigString(const std::string& section, const std::string& key) {
    std::ifstream in(GameConfigPath());
    bool inSection = false;
    for (std::string line; std::getline(in, line);) {
        const std::string t = Trim(line);
        if (t.empty() || t[0] == '#') continue;
        if (t[0] == '[') {
            inSection = t == "[" + section + "]";
            continue;
        }
        if (!inSection) continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos || Trim(t.substr(0, eq)) != key) continue;
        std::string v = Trim(t.substr(eq + 1));
        if (v.size() >= 2 && v.front() == '"') {
            const auto close = v.find('"', 1);
            if (close != std::string::npos) return v.substr(1, close - 1);
        }
        return {};
    }
    return {};
}

// Config values are absolute "sdmc:/..." paths on console; anything else falls back.
std::string ConfiguredOr(const char* key, const std::string& fallback) {
    const std::string v = ConfigString("paths", key);
    if (v.rfind("sdmc:/", 0) == 0) return SdRoot() + v.substr(5);
    return fallback;
}

std::string KartpadDir() { return SdRoot() + "/switch/kartpad"; }

// The RR pack's <savegame external="..."/> inside patch `patchId`, with Riivolution tokens filled.
std::string SavegameExternal(const std::string& patchId) {
    std::ifstream file(RetroRewindRoot() + "/RetroRewind6/xml/RetroRewind6.xml");
    if (!file) return {};
    std::stringstream buffer;
    buffer << file.rdbuf();
    const std::string xml = buffer.str();
    const auto start = xml.find("<patch id=\"" + patchId + "\">");
    if (start == std::string::npos) return {};
    const auto end = xml.find("</patch>", start);
    const std::string block = xml.substr(start, end == std::string::npos ? std::string::npos : end - start);
    static const std::regex kSavegame(R"raw(<savegame[^>]*external="([^"]+)")raw");
    std::smatch m;
    if (!std::regex_search(block, m, kSavegame)) return {};
    std::string ext = m[1].str();
    for (const auto& [token, value] : {std::pair<std::string, std::string>{"{$__gameid}", "RMC"},
                                       std::pair<std::string, std::string>{"{$__region}", "P"}}) {
        for (auto pos = ext.find(token); pos != std::string::npos; pos = ext.find(token)) ext.replace(pos, token.size(), value);
    }
    if (ext.empty() || ext.front() != '/' || ext.find("..") != std::string::npos || ext.find('{') != std::string::npos) return {};
    return ext;
}

}  // namespace

std::string SdRoot() {
#ifdef _WIN32
    return ".";
#else
    return "sdmc:";
#endif
}

std::string GameConfigPath() { return KartpadDir() + "/Config.toml"; }
std::string NandRoot() { return ConfiguredOr("nand_root", KartpadDir() + "/NAND"); }
std::string DvdRoot() { return ConfiguredOr("dvd_root", KartpadDir() + "/data"); }
std::string RetroRewindRoot() { return ConfiguredOr("retro_rewind_root", KartpadDir() + "/retro-rewind"); }

std::vector<std::string> RetroRewindSaveCandidates() {
    // The RR build resolves externals against the pack's parent (riivolution.cpp RiivoFindXmls).
    std::vector<std::string> out;
    for (const char* patch : {"RRLoadPack", "RRSave"}) {
        std::string ext = SavegameExternal(patch);
        if (ext.empty()) ext = std::string("/riivolution/save/") + (std::string(patch) == "RRSave" ? "RetroWFC2" : "RetroWFC") + "/RMCP";
        const std::string p = RetroRewindRoot() + ext + "/rksys.dat";
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    }
    return out;
}

std::string SavePath(Variant v) {
    if (v == Variant::Vanilla) return NandRoot() + "/title/00010004/524d4350/data/rksys.dat";
    const auto candidates = RetroRewindSaveCandidates();
    std::string best;
    fs::file_time_type bestTime{};
    for (const auto& c : candidates) {
        std::error_code ec;
        const auto t = fs::last_write_time(c, ec);
        if (!ec && (best.empty() || t > bestTime)) {
            best = c;
            bestTime = t;
        }
    }
    return best.empty() ? candidates.front() : best;
}

std::string MiiDatabasePath() { return NandRoot() + "/shared2/menu/FaceLib/RFL_DB.dat"; }

}  // namespace mkw::paths
