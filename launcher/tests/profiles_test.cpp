// Host test for the license (rksys.dat) and Mii (RFL_DB.dat) editors on synthetic files.
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include <zlib.h>

#include "data/game_paths.h"
#include "data/text_utf16.h"
#include "mkw/data.h"
#include "wiinand/miidb.h"

namespace fs = std::filesystem;
using mkw::Variant;

namespace {

int g_failures = 0;
void Expect(bool cond, const char* what) {
    std::cout << (cond ? "PASS " : "FAIL ") << what << '\n';
    if (!cond) ++g_failures;
}

std::vector<std::uint8_t> ReadAll(const std::string& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteAll(const std::string& p, const std::vector<std::uint8_t>& b) {
    fs::create_directories(fs::path(p).parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

std::uint32_t Crc(const std::vector<std::uint8_t>& f) { return static_cast<std::uint32_t>(crc32(0L, f.data(), 0x27FFC)); }

// A save with license 0 named "Kart" (VR 5000), CRC stored big-endian.
std::vector<std::uint8_t> MakeSave() {
    std::vector<std::uint8_t> f(0x2BC000, 0);
    std::copy_n("RKSD0006", 8, f.begin());
    std::uint8_t* l = f.data() + 0x08;
    std::copy_n("RKPD", 4, l);
    mkw::text::WriteName(l + 0x14, 10, "Kart");
    mkw::text::Write16(l + 0xB0, 5000);
    mkw::text::Write16(l + 0xB2, 5000);
    mkw::text::Write32(f.data() + 0x27FFC, Crc(f));
    return f;
}

}  // namespace

int main() {
    fs::remove_all("sdmc:");

    // ---- Licenses ----
    const std::string save = mkw::paths::SavePath(Variant::Vanilla);
    WriteAll(save, MakeSave());
    auto& lic = mkw::Licenses();
    std::string err;
    Expect(lic.Load(Variant::Vanilla, &err), "license save loads");
    Expect(lic.Writable(), "big-endian CRC recognised");
    auto list = lic.List();
    Expect(list.size() == 4 && list[0].used && list[0].name == "Kart" && list[0].vr == 5000, "license 0 read");
    Expect(!list[1].used, "license 1 empty");
    Expect(!lic.SetName(1, "Nope"), "empty license slot cannot be edited");
    Expect(lic.SetName(0, "Kawaii") && lic.SetRatings(0, 9999, 1234) && lic.UnlockEverything(0), "edits accepted");
    Expect(lic.Save(&err), "license save written");

    auto f = ReadAll(save);
    Expect(mkw::text::Read32(f.data() + 0x27FFC) == Crc(f), "CRC recomputed (big-endian)");
    Expect(mkw::text::ReadName(f.data() + 0x08 + 0x14, 10) == "Kawaii", "name written");
    Expect(mkw::text::Read16(f.data() + 0x08 + 0xB0) == 9999 && mkw::text::Read16(f.data() + 0x08 + 0xB2) == 1234, "VR/BR written");
    Expect(f[0x08 + 0x30] == 0xFF && f[0x08 + 0x38] == 0xFF, "unlock bytes set");
    Expect(mkw::Saves().ListBackups(Variant::Vanilla).size() == 1, "original backed up before writing");

    // Unknown checksum: read-only and the file is untouched.
    auto bad = MakeSave();
    bad[0x27FFC] ^= 0xFF;
    WriteAll(save, bad);
    Expect(lic.Load(Variant::Vanilla, &err) && !lic.Writable(), "unknown CRC is read-only");
    Expect(!lic.SetName(0, "X") && !lic.Save(&err), "read-only save refuses edits");
    Expect(ReadAll(save) == bad, "read-only save untouched");

    // ---- Miis ----
    auto db = wiinand::MiiDatabase::Empty();
    wiinand::Mii m;
    mkw::text::Write16(m.raw.data(), 0x0000 | (3 << 1));  // male, colour 3
    mkw::text::WriteName(m.raw.data() + 0x02, 10, "Bunga");
    m.raw[0x16] = 60;
    m.raw[0x17] = 70;
    mkw::text::Write32(m.raw.data() + 0x18, 0x81234567);
    mkw::text::Write32(m.raw.data() + 0x1C, 0xCAFEBABE);
    db.Put(0, m);
    WriteAll(mkw::paths::MiiDatabasePath(), db.Build());

    auto& miis = mkw::Miis();
    Expect(miis.Load(&err), "Mii database loads");
    auto ml = miis.List();
    Expect(ml.size() == 1 && ml[0].name == "Bunga" && ml[0].favoriteColor == 3 && ml[0].height == 60 && !ml[0].female,
           "Mii fields read");
    ml[0].name = "Luigi";
    ml[0].female = true;
    ml[0].favoriteColor = 11;
    ml[0].height = 127;
    Expect(miis.Update(ml[0]), "Mii update accepted");
    Expect(miis.Duplicate(0, &err), "Mii duplicated");
    Expect(miis.Save(&err), "Mii database saved");

    bool crcOk = false;
    auto parsed = wiinand::MiiDatabase::Parse(ReadAll(mkw::paths::MiiDatabasePath()), &crcOk);
    Expect(parsed && crcOk, "saved database has a valid CRC-16");
    Expect(parsed && parsed->Count() == 2, "two Miis after duplicate");
    Expect(miis.Load(&err), "reload");
    ml = miis.List();
    Expect(ml.size() == 2 && ml[0].name == "Luigi" && ml[0].female && ml[0].favoriteColor == 11 && ml[0].height == 127,
           "edits persisted");
    Expect(ml.size() == 2 && ml[1].miiId != ml[0].miiId && (ml[1].miiId & 0xFF000000u) == 0x81000000u,
           "duplicate has its own ID with the same type bits");

    // Link the license to the Mii.
    WriteAll(save, MakeSave());
    Expect(lic.Load(Variant::Vanilla, &err) && lic.LinkMii(0, ml[0]) && lic.Save(&err), "license linked to Mii");
    f = ReadAll(save);
    Expect(mkw::text::Read32(f.data() + 0x08 + 0x28) == ml[0].miiId && mkw::text::Read32(f.data() + 0x08 + 0x2C) == ml[0].systemId &&
               mkw::text::ReadName(f.data() + 0x08 + 0x14, 10) == "Luigi",
           "license holds the Mii's IDs and name");

    std::cout << (g_failures == 0 ? "profiles_test: OK\n" : "profiles_test: FAILED\n");
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
