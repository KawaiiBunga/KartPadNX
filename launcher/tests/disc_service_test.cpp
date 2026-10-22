// Host test for DiscService verification. Builds a fixture data tree whose ".sha256" sidecars
// stand in for the on-device SHA-256 (the Switch path hashes the real files).
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "mkw/services.h"

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
const std::string kRoot = ".";
#else
const std::string kRoot = "sdmc:";
#endif

int g_failures = 0;

void Expect(bool cond, const char* what) {
    std::cout << (cond ? "PASS " : "FAIL ") << what << '\n';
    if (!cond) ++g_failures;
}

void WriteFile(const std::string& path, const std::string& content) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream(path, std::ios::binary) << content;
}

mkw::JobStatus RunVerify() {
    mkw::Disc().StartVerify();
    mkw::JobStatus st;
    do {
        st = mkw::Disc().Poll();
    } while (st.state == mkw::JobStatus::State::Running);
    return st;
}

}  // namespace

int main() {
    const std::string data = kRoot + "/switch/kartpad/data";
    const std::string verified = kRoot + "/switch/mkw-launcher/data-verified.json";
    fs::remove_all(kRoot);

    const std::string dol = data + "/sys/main.dol";
    const std::string rel = data + "/files/rel/StaticR.rel";
    WriteFile(dol, "dol");
    WriteFile(rel, "rel");

    // Wrong digest: must fail and must not write the verified marker.
    WriteFile(dol + ".sha256", "80d18895" + std::string(56, '0'));
    WriteFile(rel + ".sha256", "16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d");
    Expect(RunVerify().state == mkw::JobStatus::State::Failed, "prefix-only main.dol digest is rejected");
    Expect(!fs::exists(verified), "no verified marker after a failed verify");

    // Correct digests: must succeed and write the marker.
    WriteFile(dol + ".sha256", "80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05");
    Expect(RunVerify().state == mkw::JobStatus::State::Succeeded, "matching digests verify");
    Expect(fs::exists(verified), "verified marker written");
    Expect(mkw::Disc().HasValidGameData(), "HasValidGameData after verify");

    // Missing file: must fail.
    fs::remove(rel);
    Expect(RunVerify().state == mkw::JobStatus::State::Failed, "missing StaticR.rel fails verify");

    // Extracted folder in the import dir with a bad digest: import fails and the user's files stay put.
    const std::string drop = kRoot + "/switch/kartpad/import/MyDump";
    WriteFile(drop + "/DATA/sys/main.dol", "dol");
    WriteFile(drop + "/DATA/sys/main.dol.sha256", std::string(64, '0'));
    WriteFile(drop + "/DATA/files/rel/StaticR.rel", "rel");
    WriteFile(drop + "/DATA/files/rel/StaticR.rel.sha256", "16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d");
    mkw::Disc().StartImport({drop, true, 0});
    mkw::JobStatus imp;
    do { imp = mkw::Disc().Poll(); } while (imp.state == mkw::JobStatus::State::Running);
    Expect(imp.state == mkw::JobStatus::State::Failed, "bad extracted dump is rejected");
    Expect(fs::exists(drop + "/DATA/sys/main.dol") && fs::exists(drop + "/DATA/files/rel/StaticR.rel"),
           "rejected dump's files are returned to the import folder");
    Expect(!fs::exists(data + ".part"), "no staging folder left behind");

    // Same dump with the right digest: moved into place and verified.
    WriteFile(drop + "/DATA/sys/main.dol.sha256", "80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05");
    mkw::Disc().StartImport({drop, true, 0});
    do { imp = mkw::Disc().Poll(); } while (imp.state == mkw::JobStatus::State::Running);
    Expect(imp.state == mkw::JobStatus::State::Succeeded, "good extracted dump imports");
    Expect(mkw::Disc().HasValidGameData(), "game data valid after import");

    // Retro Rewind: a folder that contains RetroRewind6 is accepted and moved into place.
    Expect(!mkw::Disc().HasValidRetroRewind(nullptr), "no RR pack before import");
    const std::string rrDrop = kRoot + "/rr-download";
    WriteFile(rrDrop + "/RetroRewind6/xml/RetroRewind6.xml", "<wiidisc/>");
    WriteFile(rrDrop + "/RetroRewind6/Binaries/Code.pul", "pul");
    WriteFile(rrDrop + "/RetroRewind6/version.txt", "6.12.8\n");
    mkw::Disc().StartRetroRewindImport(rrDrop);
    mkw::JobStatus rr;
    do { rr = mkw::Disc().Poll(); } while (rr.state == mkw::JobStatus::State::Running);
    Expect(rr.state == mkw::JobStatus::State::Succeeded, "RR import succeeds");
    std::string version;
    Expect(mkw::Disc().HasValidRetroRewind(&version) && version == "6.12.8", "RR pack valid with version 6.12.8");

    // A folder without Code.pul is rejected.
    WriteFile(kRoot + "/bad-rr/RetroRewind6/xml/RetroRewind6.xml", "<wiidisc/>");
    mkw::Disc().StartRetroRewindImport(kRoot + "/bad-rr");
    do { rr = mkw::Disc().Poll(); } while (rr.state == mkw::JobStatus::State::Running);
    Expect(rr.state == mkw::JobStatus::State::Failed, "RR pack without Code.pul is rejected");
    Expect(mkw::Disc().HasValidRetroRewind(nullptr), "previous RR pack still valid after a rejected import");

    std::cout << (g_failures == 0 ? "disc_service_test: OK\n" : "disc_service_test: FAILED\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
