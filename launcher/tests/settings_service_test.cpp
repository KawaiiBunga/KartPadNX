// Host test for SettingsService mods: config.json round trip and Config.toml overlay_roots.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "mkw/data.h"

namespace fs = std::filesystem;
using mkw::Variant;

namespace mkw {
const char* VariantId(Variant v) { return v == Variant::RetroRewind ? "rr" : "vanilla"; }
}  // namespace mkw

namespace {

int g_failures = 0;

void Expect(bool cond, const std::string& what) {
    std::cout << (cond ? "PASS " : "FAIL ") << what << '\n';
    if (!cond) ++g_failures;
}

std::string Read(const char* path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

}  // namespace

int main() {
    fs::create_directories(std::string(mkw::kModsDir) + "/HD Textures");
    fs::create_directories(std::string(mkw::kModsDir) + "/Say \"Hi\"");
    fs::create_directories("sdmc:/switch/kartpad");
    std::ofstream("sdmc:/switch/kartpad/Config.toml") << "[paths]\n# overlay_roots = [\"x\"]\nnand_root = \"NAND\"\n\n[video]\nshow_fps = true\n";

    const auto mods = mkw::Settings().ListMods();
    Expect(mods.size() == 2 && mods[0] == "HD Textures", "ListMods lists folders sorted");

    mkw::LauncherSettings s;
    s.mods[1] = {"HD Textures", "Say \"Hi\"", "Deleted Mod"};
    Expect(mkw::Settings().Save(s), "Save writes both files");
    mkw::LauncherSettings loaded;
    mkw::Settings().Load(loaded);
    Expect(loaded.mods[1].size() == 3 && loaded.mods[0].empty(), "mods round-trip through config.json per game");

    Expect(mkw::Settings().ApplyMods(Variant::RetroRewind, loaded), "ApplyMods writes Config.toml");
    const std::string toml = Read("sdmc:/switch/kartpad/Config.toml");
    Expect(toml.find("overlay_roots = [\"sdmc:/switch/kartpad/mods/HD Textures\", "
                     "\"sdmc:/switch/kartpad/mods/Say \\\"Hi\\\"\"]") != std::string::npos,
           "enabled, existing mods become quoted overlay_roots; a deleted one is dropped");
    Expect(toml.find("nand_root = \"NAND\"") != std::string::npos && toml.find("# overlay_roots") != std::string::npos,
           "other Config.toml lines are kept");
    Expect(toml.find("[paths]\n# overlay_roots = [\"x\"]\nnand_root = \"NAND\"\noverlay_roots") != std::string::npos,
           "overlay_roots lands inside [paths]");

    Expect(mkw::Settings().ApplyMods(Variant::Vanilla, loaded), "ApplyMods for a game with no mods");
    Expect(Read("sdmc:/switch/kartpad/Config.toml").find("overlay_roots = []") != std::string::npos,
           "no mods clears overlay_roots");

    std::cout << (g_failures ? "FAILED\n" : "All settings tests passed.\n");
    return g_failures ? 1 : 0;
}
