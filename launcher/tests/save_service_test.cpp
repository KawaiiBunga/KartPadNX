// Host test for SaveService: paths, backup rotation, restore validation and rollback safety.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "mkw/data.h"

namespace fs = std::filesystem;
using mkw::Variant;

namespace {

int g_failures = 0;

void Expect(bool cond, const char* what) {
    std::cout << (cond ? "PASS " : "FAIL ") << what << '\n';
    if (!cond) ++g_failures;
}

void WriteSave(const std::string& path, char fill) {
    fs::create_directories(fs::path(path).parent_path());
    std::string data(0x2BC000, fill);
    data.replace(0, 4, "RKSD");
    std::ofstream(path, std::ios::binary) << data;
}

char SaveFill(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(16);
    char c = 0;
    in.get(c);
    return c;
}

}  // namespace

int main() {
    fs::remove_all("sdmc:");
    auto& saves = mkw::Saves();

    const std::string vanilla = saves.GetSavePath(Variant::Vanilla);
    Expect(vanilla == "sdmc:/switch/kartpad/NAND/title/00010004/524d4350/data/rksys.dat", "vanilla save path");
    Expect(saves.GetSavePath(Variant::RetroRewind) ==
               "sdmc:/switch/kartpad/retro-rewind/riivolution/save/RetroWFC/RMCP/rksys.dat",
           "RR save path default");

    // RR path from the pack XML (RRLoadPack savegame) with token substitution.
    const std::string xml = "sdmc:/switch/kartpad/retro-rewind/RetroRewind6/xml/RetroRewind6.xml";
    fs::create_directories(fs::path(xml).parent_path());
    std::ofstream(xml) << "<patch id=\"RRLoadPack\">\n<savegame clone=\"true\" external=\"/riivolution/save/RetroWFC/{$__gameid}{$__region}\"/>\n</patch>\n"
                          "<patch id=\"RRSave\">\n<savegame external=\"/riivolution/save/RetroWFC2/{$__gameid}{$__region}\"/>\n</patch>\n";
    Expect(saves.GetSavePath(Variant::RetroRewind) ==
               "sdmc:/switch/kartpad/retro-rewind/riivolution/save/RetroWFC/RMCP/rksys.dat",
           "RR save path follows RRLoadPack, not RRSave");
    std::ofstream(xml) << "<patch id=\"RRLoadPack\"><savegame external=\"/../../escape\"/></patch>";
    Expect(saves.GetSavePath(Variant::RetroRewind) ==
               "sdmc:/switch/kartpad/retro-rewind/riivolution/save/RetroWFC/RMCP/rksys.dat",
           "RR save path with '..' falls back to the default");

    Expect(saves.ListBackups(Variant::Vanilla).empty(), "no backups when the backup dir is missing");
    Expect(!saves.BackupCurrent(Variant::Vanilla), "backup fails without a save");

    WriteSave(vanilla, 'a');
    Expect(saves.BackupCurrent(Variant::Vanilla), "backup succeeds");
    for (int i = 0; i < 6; ++i) saves.BackupCurrent(Variant::Vanilla);
    Expect(saves.ListBackups(Variant::Vanilla).size() == 5, "rotation keeps 5 backups");

    // Wrong-size file is rejected and the live save is untouched.
    const std::string bad = "sdmc:/bad-rksys.dat";
    std::ofstream(bad, std::ios::binary) << "RKSD tiny";
    Expect(!saves.ImportSave(Variant::Vanilla, bad), "wrong-size import rejected");
    Expect(SaveFill(vanilla) == 'a', "live save unchanged after rejected import");

    // Valid import replaces the save and backs up the old one first.
    const std::string good = "sdmc:/good-rksys.dat";
    WriteSave(good, 'b');
    Expect(saves.ImportSave(Variant::Vanilla, good), "valid import succeeds");
    Expect(SaveFill(vanilla) == 'b', "live save replaced by import");

    // Restoring the oldest slot works even though the pre-restore backup rotates it out.
    const auto slots = saves.ListBackups(Variant::Vanilla);
    Expect(saves.Restore(Variant::Vanilla, slots.front().name), "restore of the oldest slot succeeds");
    Expect(SaveFill(vanilla) == 'a', "live save restored from backup");
    Expect(!saves.Restore(Variant::Vanilla, "../../etc"), "slot id with '..' rejected");
    Expect(!fs::exists(vanilla + ".part") && !fs::exists(vanilla + ".bak"), "no staging files left behind");

    std::cout << (g_failures == 0 ? "save_service_test: OK\n" : "save_service_test: FAILED\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
