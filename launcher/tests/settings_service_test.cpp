// Host test for SettingsService mods: config.json round trip and Config.toml overlay_roots.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "data/mod_builder.h"
#include "mkw/data.h"
#include "test_png.h"

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
    // Laid out like the disc, so each stays its own overlay root.
    fs::create_directories(std::string(mkw::kModsDir) + "/HD Textures/Race");
    fs::create_directories(std::string(mkw::kModsDir) + "/Say \"Hi\"/Scene");
    fs::create_directories("sdmc:/switch/kartpad");
    std::ofstream("sdmc:/switch/kartpad/Config.toml") << "[paths]\n# overlay_roots = [\"x\"]\nnand_root = \"NAND\"\n\n[video]\nshow_fps = true\n";

    const auto mods = mkw::Settings().ListMods();
    Expect(mods.size() == 2 && mods[0] == "HD Textures", "ListMods lists folders sorted");

    mkw::LauncherSettings fresh;
    mkw::Settings().Load(fresh);  // no config.json yet
    Expect(fresh.mods[0].size() == 2 && fresh.mods[1].size() == 2 && fresh.knownMods == mods,
           "on a fresh install every mod folder starts on for both games");

    mkw::LauncherSettings s;
    s.mods[1] = {"HD Textures", "Say \"Hi\"", "Deleted Mod"};
    s.knownMods = mods;
    Expect(mkw::Settings().Save(s), "Save writes both files");
    mkw::LauncherSettings loaded;
    mkw::Settings().Load(loaded);
    Expect(loaded.mods[1].size() == 3 && loaded.mods[0].empty(), "mods round-trip through config.json per game");

    fs::create_directories(std::string(mkw::kModsDir) + "/Dropped In");
    mkw::LauncherSettings later;
    mkw::Settings().Load(later);
    Expect(later.mods[0] == std::vector<std::string>{"Dropped In"} && later.mods[1].size() == 4,
           "a mod folder added later is switched on for both games");
    later.mods[0].clear();
    mkw::Settings().Save(later);
    mkw::LauncherSettings again2;
    mkw::Settings().Load(again2);
    Expect(again2.mods[0].empty(), "a new mod the player turned off stays off");
    fs::remove_all(std::string(mkw::kModsDir) + "/Dropped In");
    loaded.knownMods = mods;

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

    // A Riivolution pack shaped like Mario Kart Black: options default to off in its XML.
    const std::string pack = std::string(mkw::kModsDir) + "/Black Pack";
    fs::create_directories(pack + "/riivolution");
    fs::create_directories(pack + "/Black/Tracks");
    fs::create_directories(pack + "/Black/Wiimmfi");
    std::ofstream(pack + "/Black/Tracks/castle_course.szs") << "x";
    std::ofstream(pack + "/Black/Wiimmfi/main.dol") << "x";
    std::ofstream(pack + "/riivolution/Black.xml") <<
        "<!--\n<wiidisc root=\"/Wrong\" version=\"1\">\n-->\n"
        "<wiidisc root=\"/Black\" version=\"1\">\n<id game=\"RMC\" />\n<options>\n"
        "  <section name=\"Mario Kart Black\">\n"
        "    <option name=\"Texture Hacked Tracks\"><choice name=\"Enabled\"><patch id=\"tracks\" /></choice></option>\n"
        "    <option name=\"Shock Plaza (Don't Use on Wiimmfi!)\"><choice name=\"Enabled\"><patch id=\"tracks\" /></choice></option>\n"
        "    <option name=\"Wiimmfi\"><choice name=\"Enabled\"><patch id=\"wiimmfi\" /></choice></option>\n"
        "    <option id=\"speed\" name=\"Speed\"><choice name=\"150cc\" /><choice name=\"200cc\" /></option>\n"
        "  </section>\n</options>\n"
        "<patch id=\"tracks\">\n  <folder external=\"Tracks\" />\n</patch>\n"
        "<patch id=\"wiimmfi\">\n  <folder external=\"Wiimmfi\" />\n</patch>\n</wiidisc>\n";

    auto opts = mkw::Settings().ModOptions("Black Pack");
    Expect(opts.size() == 4 && opts[0].id == "Mario Kart BlackTexture Hacked Tracks" && opts[3].id == "speed" &&
               opts[3].choices.size() == 2 && opts[3].choices[1] == "200cc",
           "pack options read with Riivolution's ids and choice names (comment skipped)");
    Expect(opts[0].selected == 1 && opts[1].selected == 1 && opts[3].selected == 1,
           "options are on by default when the pack has no saved choices");
    Expect(opts[2].replacesCode && opts[2].selected == 0 && !opts[0].replacesCode,
           "an option that swaps main.dol is flagged and left off");

    loaded.mods[0] = {"Black Pack"};
    Expect(mkw::Settings().ApplyMods(Variant::Vanilla, loaded), "ApplyMods with a Riivolution pack");
    const std::string config = Read((pack + "/riivolution/config/RMCP.xml").c_str());
    Expect(config.find("<riivolution version=\"2\">") != std::string::npos &&
               config.find("id=\"Mario Kart BlackTexture Hacked Tracks\" default=\"1\"") != std::string::npos &&
               config.find("id=\"Mario Kart BlackWiimmfi\" default=\"0\"") != std::string::npos,
           "launching saves the default choices where the game reads them");

    opts[0].selected = 0;
    opts[3].selected = 2;
    Expect(mkw::Settings().SaveModOptions("Black Pack", opts), "SaveModOptions writes");
    const auto again = mkw::Settings().ModOptions("Black Pack");
    Expect(again[0].selected == 0 && again[3].selected == 2 && again[1].selected == 1,
           "saved choices are read back instead of the defaults");

    // A Pulsar code pack (Item Fever's layout): its one option loads code, so it stays off.
    const std::string pulsar = std::string(mkw::kModsDir) + "/Pulsar Pack";
    fs::create_directories(pulsar + "/riivolution");
    fs::create_directories(pulsar + "/Fever/Binaries");
    fs::create_directories(pulsar + "/Fever/Disc");
    std::ofstream(pulsar + "/Fever/Binaries/Code.pul") << "x";
    std::ofstream(pulsar + "/Fever/Disc/Common.szs") << "x";
    std::ofstream(pulsar + "/riivolution/Fever.xml") <<
        "<wiidisc version=\"1\"><id game=\"RMC\" /><options><section name=\"Fever\">"
        "<option name=\"Fever\" id=\"Fever\" default=\"1\"><choice name=\"Enabled\"><patch id=\"f\" /></choice></option>"
        "</section></options><patch id=\"f\">"
        "<memory offset=\"0x80004000\" valuefile=\"/Fever/Binaries/Loader.pul\" />"
        "<file disc=\"/Race/Common.szs\" external=\"/Fever/Disc/Common.szs\" /></patch></wiidisc>\n";
    const auto fever = mkw::Settings().ModOptions("Pulsar Pack");
    Expect(fever.size() == 1 && fever[0].replacesCode && fever[0].selected == 0,
           "a pack that loads code into memory (Pulsar) is flagged and left off");

    // A My Stuff mod: loose files in non-disc folders, matched by file name.
    const std::string mystuff = std::string(mkw::kModsDir) + "/Orange";
    fs::create_directories(mystuff + "/Karts and Allkart Files");
    std::ofstream(mystuff + "/Karts and Allkart Files/la_bike-wl.szs") << "x";
    fs::create_directories(std::string(mkw::kGameDataDir) + "/files/Race/Kart");
    std::ofstream(std::string(mkw::kGameDataDir) + "/files/Race/Kart/la_bike-wl.szs") << "x";  // on the disc
    // An earlier build wrote its XML into the mod; that gets cleaned out.
    fs::create_directories(mystuff + "/riivolution/config");
    std::ofstream(mystuff + "/riivolution/KartPadNX My Stuff.xml") << "<wiidisc />";
    std::ofstream(mystuff + "/riivolution/config/RMCP.xml") << "<riivolution />";
    Expect(mkw::Settings().ModOptions("Orange").empty() && !fs::exists(mystuff + "/riivolution"),
           "a My Stuff mod has no options and nothing of ours left in its folder");
    const char* const modsXmlPath = "sdmc:/switch/kartpad/riivolution/KartPadNX Mods.xml";
    loaded.mods[0] = {"Orange"};
    Expect(mkw::Settings().ApplyMods(Variant::Vanilla, loaded), "ApplyMods with a My Stuff mod");
    const std::string msXml = Read(modsXmlPath);
    Expect(msXml.find("<folder external=\"/mods/Orange\" recursive=\"true\" />") != std::string::npos &&
               msXml.find("disc=") == std::string::npos && !fs::exists(mystuff + "/riivolution") &&
               Read("sdmc:/switch/kartpad/Config.toml").find("overlay_roots = [\"sdmc:/switch/kartpad\"]") != std::string::npos &&
               Read("sdmc:/switch/kartpad/riivolution/config/RMCP.xml").find("id=\"KartPadNXMods\" default=\"1\"") != std::string::npos,
           "the launcher's own XML maps the mod's folder by file name, switched on");
    // Character files that replace nothing (a Retro Rewind skin slot) are added where RR puts them.
    fs::create_directories(std::string(mkw::kGameDataDir) + "/files/Race/Kart");
    std::ofstream(std::string(mkw::kGameDataDir) + "/files/Race/Kart/la_kart-fk.szs") << "x";
    const std::string skin = std::string(mkw::kModsDir) + "/Peter fk-8";
    fs::create_directories(skin);
    for (const char* f : {"la_kart-fk.szs", "la_kart-fk-8.szs", "fk-8-allkart.szs", "fk-8.brres", "GRP_VO_FK-8_PC.brwsd", "readme.txt"})
        std::ofstream(skin + "/" + f) << "x";
    loaded.mods[0] = {"Peter fk-8", "Orange"};
    mkw::Settings().ApplyMods(Variant::Vanilla, loaded);
    const std::string skinXml = Read(modsXmlPath);
    Expect(skinXml.find("disc=\"/Race/Kart/la_kart-fk-8.szs\" external=\"/mods/Peter fk-8/la_kart-fk-8.szs\" create=\"true\"") !=
                   std::string::npos &&
               skinXml.find("disc=\"/Scene/Model/Kart/fk-8-allkart.szs\"") != std::string::npos &&
               skinXml.find("disc=\"/Scene/Model/Driver/fk-8.brres\"") != std::string::npos &&
               skinXml.find("disc=\"/sound/GRP_VO_FK-8_PC.brwsd\"") != std::string::npos,
           "new character files go where Retro Rewind's Character folder puts them");
    Expect(skinXml.find("/Race/Kart/la_kart-fk.szs") == std::string::npos && skinXml.find("readme") == std::string::npos,
           "files that replace something stay by-name; unrelated files are left out");
    Expect(skinXml.find("external=\"/mods/Orange\"") < skinXml.find("external=\"/mods/Peter fk-8\""),
           "the first mod in the list comes last in the XML, so it wins");

    // Files that go inside archives: driver models placed by folder name, an icon from a PNG.
    using mkw::modbuild::Bytes;
    auto szs = [](const std::vector<std::pair<std::string, Bytes>>& files) {
        mkw::modbuild::U8Archive a;
        a.strings = {0, '.', 0};
        const uint32_t total = static_cast<uint32_t>(files.size() + 2);
        a.nodes.push_back({1, 0, 0, total, "", {}});
        a.nodes.push_back({1, 1, 0, total, "", {}});
        for (const auto& [name, data] : files) {
            const uint32_t off = static_cast<uint32_t>(a.strings.size());
            a.strings.insert(a.strings.end(), name.begin(), name.end());
            a.strings.push_back(0);
            a.nodes.push_back({0, off, 0, 0, "", data});
        }
        return mkw::modbuild::Yaz0Encode(mkw::modbuild::U8Write(a));
    };
    auto bytes = [](const std::string& s) { return Bytes(s.begin(), s.end()); };
    auto write = [](const std::string& path, const Bytes& b) {
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), b.size());
    };
    auto member = [](const std::string& path, const std::string& name) {
        mkw::modbuild::U8Archive a;
        std::ifstream in(path, std::ios::binary);
        const Bytes raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!mkw::modbuild::U8Parse(mkw::modbuild::Yaz0Decode(raw), a)) return Bytes{};
        for (const auto& n : a.nodes)
            if (n.path == "./" + name) return n.data;
        return Bytes{};
    };
    const std::string discFiles = std::string(mkw::kGameDataDir) + "/files";
    write(discFiles + "/Scene/Model/Driver.szs", szs({{"wl.brres", bytes("disc driver")}, {"dk.brres", bytes("dk")}}));
    write(discFiles + "/Demo/Award.szs", szs({{"wl.brres", bytes("disc award")}}));
    write(discFiles + "/Scene/UI/Race.szs", szs({{"st_waluigi_32x32.tpl", test::MakeTpl(32, 32)}}));
    write(mystuff + "/Driver/wl.brres", bytes("orange driver"));
    write(mystuff + "/Award/wl.brres", bytes("orange award"));
    write(mystuff + "/Icons/MiniMap 32x32.png", test::MakePng(1, 1, {255, 0, 0, 255}));
    loaded.mods[0] = {"Orange"};
    Expect(mkw::Settings().ApplyMods(Variant::Vanilla, loaded), "ApplyMods with files that go inside archives");
    const std::string gen = "sdmc:/switch/kartpad/mods-generated/vanilla";
    Expect(member(gen + "/Scene/Model/Driver.szs", "wl.brres") == bytes("orange driver") &&
               member(gen + "/Scene/Model/Driver.szs", "dk.brres") == bytes("dk") &&
               member(gen + "/Demo/Award.szs", "wl.brres") == bytes("orange award"),
           "each wl.brres goes into the archive its folder names; other members are kept");
    const Bytes icon = member(gen + "/Scene/UI/Race.szs", "st_waluigi_32x32.tpl");
    Expect(icon.size() == 0x40 + 32 * 32 * 2 && icon[0x18 + 3] == 5 && icon[0x40] == 0xFC,
           "a character's PNG icon replaces its TPL (character from the mod's kart files)");
    const std::string genXml = Read(modsXmlPath);
    Expect(genXml.find("disc=\"/Scene/Model/Driver.szs\" external=\"/mods-generated/vanilla/Scene/Model/Driver.szs\"") !=
                   std::string::npos &&
               genXml.find("<patch id=\"archives\">") > genXml.find("external=\"/mods/Orange\""),
           "the patched archives come last in the launcher's XML, above the mods");

    // Wheel Wizard / Pulsar patch files: "<name>.<Archive>.szs" with only the changed members
    // (".delete" drops one), and a loose "<member>.<Archive>".
    const std::string patchy = std::string(mkw::kModsDir) + "/Patchy";
    write(patchy + "/Patchy.Driver.szs",
          szs({{"dk.brres", bytes("patched dk")}, {"new.brres", bytes("new")}, {"wl.brres.delete", {}}}));
    write(patchy + "/extra.bin.Award", bytes("extra"));
    write(patchy + "/Patchy.ini", bytes("[Mod]"));
    loaded.mods[0] = {"Patchy"};
    mkw::Settings().ApplyMods(Variant::Vanilla, loaded);
    Expect(member(gen + "/Scene/Model/Driver.szs", "dk.brres") == bytes("patched dk") &&
               member(gen + "/Scene/Model/Driver.szs", "new.brres") == bytes("new") &&
               member(gen + "/Scene/Model/Driver.szs", "wl.brres").empty() &&
               member(gen + "/Demo/Award.szs", "extra.bin") == bytes("extra") &&
               member(gen + "/Demo/Award.szs", "wl.brres") == bytes("disc award"),
           "patch archives replace, add and delete members; a loose patch adds one");

    // A disc-layout mod made for the US game (Mario Kart Wiimastered): its archives are the base
    // for patching, and its US language archives stand in for the European ones.
    const std::string remaster = std::string(mkw::kModsDir) + "/Remaster";
    write(remaster + "/Scene/Model/Driver.szs", szs({{"wl.brres", bytes("remaster wl")}, {"dk.brres", bytes("remaster dk")}}));
    write(remaster + "/Scene/UI/Race_U.szs", bytes("us english"));
    write(remaster + "/Scene/UI/Race_Q.szs", bytes("us french"));
    write(discFiles + "/Scene/UI/Race_E.szs", bytes("eu english"));
    write(discFiles + "/Scene/UI/Race_F.szs", bytes("eu french"));
    loaded.mods[0] = {"Orange", "Remaster"};
    mkw::Settings().ApplyMods(Variant::Vanilla, loaded);
    Expect(member(gen + "/Scene/Model/Driver.szs", "wl.brres") == bytes("orange driver") &&
               member(gen + "/Scene/Model/Driver.szs", "dk.brres") == bytes("remaster dk"),
           "files injected into an archive a disc-layout mod replaces start from that mod's copy");
    const std::string regionXml = Read(modsXmlPath);
    Expect(regionXml.find("disc=\"/Scene/UI/Race_E.szs\" external=\"/mods/Remaster/Scene/UI/Race_U.szs\"") != std::string::npos &&
               regionXml.find("disc=\"/Scene/UI/Race_F.szs\" external=\"/mods/Remaster/Scene/UI/Race_Q.szs\"") != std::string::npos &&
               Read("sdmc:/switch/kartpad/Config.toml").find("overlay_roots = [\"sdmc:/switch/kartpad\", \"" + remaster + "\"]") !=
                   std::string::npos,
           "US language archives stand in for the European ones");
    loaded.mods[0] = {"Remaster"};
    mkw::Settings().ApplyMods(Variant::Vanilla, loaded);
    Expect(Read(modsXmlPath).find("Race_U.szs") != std::string::npos &&
               Read("sdmc:/switch/kartpad/Config.toml").find("overlay_roots = [\"sdmc:/switch/kartpad\", \"" + remaster + "\"]") !=
                   std::string::npos,
           "the region mapping works with no loose-file mod on");

    // A mod dropped in as a .zip is unpacked, its wrapping folder removed, and switched on.
    write(std::string(mkw::kModsDir) + "/Zipped.zip",
          test::MakeZip({{"Zipped/riivolution/Zipped.xml", "<wiidisc />"}, {"Zipped/Zipped/a.szs", std::string(5000, 'x')}}));
    write(std::string(mkw::kModsDir) + "/Evil.zip", test::MakeZip({{"../escape.txt", "x"}}));
    mkw::LauncherSettings z;
    z.knownMods = mkw::Settings().ListMods();
    mkw::Settings().PickUpNewMods(z);
    Expect(Read((std::string(mkw::kModsDir) + "/Zipped/Zipped/a.szs").c_str()) == std::string(5000, 'x') &&
               fs::exists(std::string(mkw::kModsDir) + "/Zipped/riivolution/Zipped.xml") &&
               std::find(z.mods[0].begin(), z.mods[0].end(), "Zipped") != z.mods[0].end(),
           "a zipped mod is unpacked without its wrapping folder and switched on");
    Expect(!fs::exists("sdmc:/switch/kartpad/escape.txt") && !fs::exists(std::string(mkw::kModsDir) + "/Evil") &&
               fs::exists(std::string(mkw::kModsDir) + "/Evil.zip - could not unpack.txt"),
           "a zip with paths outside its folder is refused, with a note");

    // A disc-shaped mod keeps working by path: no XML is added.
    const std::string discShaped = std::string(mkw::kModsDir) + "/Disc Shaped";
    fs::create_directories(discShaped + "/Race/Course");
    std::ofstream(discShaped + "/Race/Course/castle_course.szs") << "x";
    Expect(mkw::Settings().ModOptions("Disc Shaped").empty() && !fs::exists(discShaped + "/riivolution"),
           "a mod laid out like the disc gets no XML");

    std::cout << (g_failures ? "FAILED\n" : "All settings tests passed.\n");
    return g_failures ? 1 : 0;
}
