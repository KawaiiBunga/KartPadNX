// Host test for the Mii database: creation from nothing, the 74-byte record codec, migration from the
// old 0x1F1E0 size, repair of broken Miis, and refusals. Run from an empty scratch directory: paths are
// relative there on the host (paths::SdRoot() is "." on Windows, "sdmc:" elsewhere).
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "data/game_paths.h"
#include "data/mii_record.h"
#include "mkw/data.h"
#include "wiinand/miidb.h"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Expect(bool cond, const std::string& what) {
    std::cout << (cond ? "PASS " : "FAIL ") << what << std::endl;
    if (!cond) ++g_failures;
}

std::vector<std::uint8_t> ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteFile(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

bool MarkersOk(const std::vector<std::uint8_t>& f) {
    return f.size() >= wiinand::kMiiDatabaseSize && f[0] == 'R' && f[1] == 'N' && f[2] == 'O' && f[3] == 'D' &&
           f[wiinand::kMiiEmptyMarkOffset] == 0x80 && f[0x1D00] == 'R' && f[0x1D01] == 'N' && f[0x1D02] == 'H' &&
           f[0x1D03] == 'D' && f[0x1D04] == 0xFF && f[0x1D05] == 0xFF && f[0x1D06] == 0xFF && f[0x1D07] == 0xFF;
}

bool CrcOk(const std::vector<std::uint8_t>& f) {
    const std::uint16_t stored = static_cast<std::uint16_t>((f[0x1F1DE] << 8) | f[0x1F1DF]);
    return stored == wiinand::Crc16Ccitt(f.data(), wiinand::kMiiCrcOffset);
}

std::vector<std::uint8_t> FromHex(const std::string& hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

// Expected record for the sample below, from an independent encoder written from WheelWizard
// MiiSerializer.Serialize (private/analysis/scratch/mii/expected_mii.py).
constexpr const char* kExpectedRecordHex =
    "732f004100790075000000000000000000000000000064149a1b2c3d01020304ae4083a08a40aeeba0d18b20b8809c6f66d3bd10b6d2004b006100720074005000610064002d004e0058";

// The same values as the Python encoder. Bits MiiEntry does not carry (mingle, downloaded) stay 0.
mkw::MiiEntry SampleEntry() {
    mkw::MiiEntry e;
    e.name = "Ayu";
    e.creator = "KartPad-NX";
    e.female = true;
    e.birthMonth = 12;
    e.birthDay = 25;
    e.favoriteColor = 7;
    e.favorite = true;
    e.height = 100;
    e.weight = 20;
    e.faceShape = 5;
    e.skinColor = 3;
    e.facialFeature = 9;
    e.hairType = 65;
    e.hairColor = 6;
    e.hairFlipped = true;
    e.browType = 17;
    e.browRotation = 9;
    e.browColor = 5;
    e.browSize = 7;
    e.browVertical = 14;
    e.browSpacing = 11;
    e.eyeType = 40;
    e.eyeRotation = 6;
    e.eyeVertical = 17;
    e.eyeColor = 4;
    e.eyeSize = 5;
    e.eyeSpacing = 9;
    e.noseType = 11;
    e.noseSize = 8;
    e.noseVertical = 16;
    e.lipType = 19;
    e.lipColor = 2;
    e.lipSize = 3;
    e.lipVertical = 15;
    e.glassesType = 6;
    e.glassesColor = 3;
    e.glassesSize = 6;
    e.glassesVertical = 19;
    e.mustacheType = 2;
    e.beardType = 3;
    e.facialHairColor = 6;
    e.mustacheSize = 8;
    e.mustacheVertical = 16;
    e.moleOn = true;
    e.moleSize = 6;
    e.moleVertical = 27;
    e.moleHorizontal = 9;
    return e;
}

bool SameFace(const mkw::MiiEntry& a, const mkw::MiiEntry& b) {
    return a.faceShape == b.faceShape && a.skinColor == b.skinColor && a.facialFeature == b.facialFeature &&
           a.hairType == b.hairType && a.hairColor == b.hairColor && a.hairFlipped == b.hairFlipped &&
           a.browType == b.browType && a.browRotation == b.browRotation && a.browColor == b.browColor &&
           a.browSize == b.browSize && a.browVertical == b.browVertical && a.browSpacing == b.browSpacing &&
           a.eyeType == b.eyeType && a.eyeRotation == b.eyeRotation && a.eyeVertical == b.eyeVertical &&
           a.eyeColor == b.eyeColor && a.eyeSize == b.eyeSize && a.eyeSpacing == b.eyeSpacing &&
           a.noseType == b.noseType && a.noseSize == b.noseSize && a.noseVertical == b.noseVertical &&
           a.lipType == b.lipType && a.lipColor == b.lipColor && a.lipSize == b.lipSize &&
           a.lipVertical == b.lipVertical && a.glassesType == b.glassesType && a.glassesColor == b.glassesColor &&
           a.glassesSize == b.glassesSize && a.glassesVertical == b.glassesVertical &&
           a.mustacheType == b.mustacheType && a.beardType == b.beardType &&
           a.facialHairColor == b.facialHairColor && a.mustacheSize == b.mustacheSize &&
           a.mustacheVertical == b.mustacheVertical && a.moleOn == b.moleOn && a.moleSize == b.moleSize &&
           a.moleVertical == b.moleVertical && a.moleHorizontal == b.moleHorizontal;
}

}  // namespace

int main() {
    const std::string root = mkw::paths::SdRoot();
    const std::string dbPath = mkw::paths::MiiDatabasePath();
    std::error_code ec;
    fs::remove_all(root + "/switch", ec);

    mkw::MiiService& miis = mkw::Miis();
    std::string err;

    // ---- 1. Creation from nothing ---------------------------------------------------------
    {
        const bool ok = miis.Load(&err);
        Expect(!ok && !err.empty(), "Load on a missing database reports it instead of inventing one");
        Expect(!fs::exists(dbPath), "Load does not create the database");
    }
    {
        const bool ok = miis.Create("Alice", true, 3, &err);
        Expect(ok, "Create makes the database and the first Mii" + (ok ? std::string() : ": " + err));
    }
    Expect(fs::exists(dbPath), "RFL_DB.dat exists after Create");
    {
        const bool ok = miis.Create("Bob", false, 5, &err);
        Expect(ok, "Create adds a second Mii" + (ok ? std::string() : ": " + err));
    }
    auto bytes = ReadFile(dbPath);
    Expect(bytes.size() == wiinand::kMiiDatabaseSize, "new database is 779,968 bytes");
    Expect(MarkersOk(bytes), "markers: RNOD, 0x80 at 0x1CEC, RNHD at 0x1D00, FF FF FF FF at 0x1D04");
    Expect(CrcOk(bytes), "CRC-16 at 0x1F1DE matches the file");

    // ---- 2. Factory defaults and identity --------------------------------------------------
    miis.Load(&err);
    auto list = miis.List();
    Expect(list.size() == 2, "two Miis are listed");
    if (list.size() == 2) {
        const auto& a = list[0];
        const auto& b = list[1];
        Expect(a.name == "Alice" && a.female && a.favoriteColor == 3, "Alice: name, gender, colour");
        Expect(b.name == "Bob" && !b.female && b.favoriteColor == 5, "Bob: name, gender, colour");
        Expect(a.height == 63 && a.weight == 63, "height and weight are the factory 63");
        Expect(a.birthMonth == 1 && a.birthDay == 1, "birthday is 1/1");
        Expect(a.hairType == 12 && a.eyeType == 4 && a.browType == 0, "female factory face (hair 12, eyes 4, brows 0)");
        Expect(b.hairType == 33 && b.eyeType == 2 && b.browType == 6, "male factory face (hair 33, eyes 2, brows 6)");
        Expect((a.miiId & 0xE0000000u) == 0x80000000u, "Mii ID top bits are 100 (normal)");
        Expect(a.miiId != b.miiId, "Mii IDs differ");
        Expect(a.systemId == b.systemId && a.systemId != 0, "both Miis share the install's system ID");
        Expect(a.creator == "KartPad-NX", "creator name");
    }

    // ---- 3. Codec: encode the sample and compare with the independent encoder ---------------
    {
        std::array<std::uint8_t, 0x4A> raw{};
        const mkw::MiiEntry s = SampleEntry();
        mkw::record::EncodeFields(s, raw.data());
        mkw::record::SetIdentity(raw.data(), 0x9A1B2C3Du, 0x01020304u, s.creator);
        const auto want = FromHex(kExpectedRecordHex);
        Expect(want.size() == 0x4A, "expected record is 74 bytes");
        Expect(std::equal(raw.begin(), raw.end(), want.begin()), "encoded record matches the independent encoder byte for byte");
        if (!std::equal(raw.begin(), raw.end(), want.begin())) {
            for (std::size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] != want[i]) {
                    std::cout << "  first difference at 0x" << std::hex << i << ": got " << int(raw[i]) << " want "
                              << int(want[i]) << std::dec << '\n';
                    break;
                }
            }
        }
        const mkw::MiiEntry back = mkw::record::DecodeFields(want.data(), 0);
        Expect(SameFace(back, s), "decoding the expected record gives back every face field");
        Expect(back.name == "Ayu" && back.birthMonth == 12 && back.birthDay == 25 && back.favorite,
               "decoding gives back name, birthday and favourite");
        Expect(back.miiId == 0x9A1B2C3Du && back.systemId == 0x01020304u, "decoding gives back the IDs");
    }

    // ---- 4. Update keeps identity -----------------------------------------------------------
    {
        mkw::MiiEntry e = miis.List()[0];
        const std::uint32_t id = e.miiId;
        const std::uint32_t sys = e.systemId;
        e.name = "Alicia";
        e.hairType = 20;
        e.eyeRotation = 2;
        e.moleOn = true;
        const bool ok = miis.Update(e);
        Expect(ok, "Update accepts a changed Mii");
        Expect(miis.Dirty(), "Update marks the database dirty");
        Expect(miis.Save(&err), "Save writes the update: " + err);
        miis.Load(&err);
        const auto after = miis.List()[0];
        Expect(after.name == "Alicia" && after.hairType == 20 && after.eyeRotation == 2 && after.moleOn,
               "Update wrote the changed fields");
        Expect(after.miiId == id && after.systemId == sys, "Update leaves the Mii ID and system ID alone");
    }

    // ---- 5. Migration from the old 0x1F1E0 layout -------------------------------------------
    {
        // The current database cut to the old size: the same Miis, in the old layout.
        const auto current = ReadFile(dbPath);
        std::vector<std::uint8_t> legacy(current.begin(), current.begin() + static_cast<std::ptrdiff_t>(wiinand::kLegacyMiiDatabaseSize));
        WriteFile(dbPath, legacy);
        const bool ok = miis.Load(&err);
        Expect(ok, "Load accepts a 0x1F1E0-byte database");
        Expect(miis.List().size() == 2, "migration keeps both Miis");
        bytes = ReadFile(dbPath);
        Expect(bytes.size() == wiinand::kMiiDatabaseSize, "the legacy file is saved back at 779,968 bytes");
        Expect(MarkersOk(bytes) && CrcOk(bytes), "migrated file has the markers and a valid CRC");
    }

    // ---- 6. Repair of a broken Mii -----------------------------------------------------------
    {
        // Break slot 0: zero the face block and set the birthday to 0/0, then write it back with a valid CRC.
        auto parsed = wiinand::MiiDatabase::Parse(ReadFile(dbPath));
        wiinand::Mii m = parsed->Slots()[0];
        for (std::size_t i = 0x20; i < 0x36; ++i) m.raw[i] = 0;
        // Clear only the birthday bits (month 13..10, day 9..5); keep invalid, gender, colour and favourite.
        const std::uint16_t flags = static_cast<std::uint16_t>((m.raw[0] << 8) | m.raw[1]) & 0xC01F;
        m.raw[0] = static_cast<std::uint8_t>(flags >> 8);
        m.raw[1] = static_cast<std::uint8_t>(flags & 0xFF);
        parsed->Put(0, m);
        WriteFile(dbPath, parsed->Build());
        miis.Load(&err);
        const auto fixed = miis.List()[0];
        Expect(fixed.birthMonth == 1 && fixed.birthDay == 1, "repair sets birthday to 1/1");
        Expect(fixed.hairType == 12 && fixed.eyeType == 4 && fixed.browType == 0 && fixed.female,
               "repair restores the female factory face");
        Expect(fixed.name == "Alicia" && fixed.favoriteColor == 3, "repair keeps the name and colour");
        Expect(CrcOk(ReadFile(dbPath)), "repaired file is saved with a valid CRC");
        Expect(fs::exists(root + "/switch/mkw-launcher/saves/mii"), "a backup was kept before the repair was saved");
    }

    // ---- 7. Refusals ---------------------------------------------------------------------------
    {
        const bool ok = miis.Create("", false, 0, &err);
        Expect(!ok, "empty name is refused: " + err);
    }
    {
        const bool ok = miis.Create("ABCDEFGHIJK", false, 0, &err);
        Expect(!ok, "11-character name is refused: " + err);
    }
    {
        const bool ok = miis.Create("Ok", false, 12, &err);
        Expect(!ok, "colour out of range is refused: " + err);
    }
    {
        std::ofstream out(dbPath, std::ios::binary | std::ios::trunc);
        out << "not a mii database";
    }
    const auto damaged = ReadFile(dbPath);
    {
        const bool ok = miis.Load(&err);
        Expect(!ok, "Load refuses the damaged file");
    }
    {
        const bool ok = miis.Create("Carol", false, 0, &err);
        Expect(!ok, "Create refuses a damaged database: " + err);
    }
    Expect(ReadFile(dbPath) == damaged, "a damaged database is not overwritten");

    std::cout << (g_failures == 0 ? "ALL PASS\n" : "FAILURES: " + std::to_string(g_failures) + "\n");
    return g_failures == 0 ? 0 : 1;
}
