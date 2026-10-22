#pragma once

// RFL_DB.dat: the Wii's Mii database.
//
// One file at /shared2/menu/FaceLib/RFL_DB.dat holding up to 100 Miis, read by every game.
// A new database is 779,968 bytes (WheelWizard ForceCreateDatabase). The layout used here:
//   "RNOD" at 0x00, 100 slots of 74 bytes from 0x04 (the table ends at 0x1CEC),
//   0x80 at 0x1CEC, "RNHD" at 0x1D00 followed by FF FF FF FF, and a CRC-16/CCITT of
//   everything before 0x1F1DE stored big-endian at 0x1F1DE. Bytes after 0x1F1E0 are kept as they are.
//
// Older launcher builds wrote 0x1F1E0 bytes. Parse() extends such a file to the full size and reports
// it as migrated; the caller saves it back.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wiinand {

inline constexpr std::size_t kMiiDatabaseSize = 779968;     // a new or complete RFL_DB.dat
inline constexpr std::size_t kLegacyMiiDatabaseSize = 0x1F1E0;  // what older launcher builds wrote
inline constexpr std::size_t kMiiEntrySize = 0x4A;
inline constexpr std::size_t kMiiSlots = 100;
inline constexpr std::size_t kMiiEntriesOffset = 0x04;
inline constexpr std::size_t kMiiEmptyMarkOffset = 0x1CEC;  // 0x80 in a new database
inline constexpr std::size_t kMiiParadeOffset = 0x1D00;
inline constexpr std::size_t kMiiCrcOffset = 0x1F1DE;

// CRC-16/CCITT (polynomial 0x1021, zero initial value).
std::uint16_t Crc16Ccitt(const std::uint8_t* data, std::size_t length);

// One Mii as its 74 bytes plus the fields worth showing without decoding the face.
struct Mii {
    std::array<std::uint8_t, kMiiEntrySize> raw{};

    bool Used() const;
};

class MiiDatabase {
public:
    // A new, empty database: 779,968 bytes with the markers and CRC set.
    static MiiDatabase Empty();

    // Parses a file of at least kLegacyMiiDatabaseSize bytes with the RNOD and RNHD magics.
    // `checksumValid` reports the CRC; a stale CRC is still readable. A legacy-size file is extended
    // to kMiiDatabaseSize and `migrated` is set. Returns nothing for any other file.
    static std::optional<MiiDatabase> Parse(const std::vector<std::uint8_t>& file, bool* checksumValid = nullptr,
                                            bool* migrated = nullptr);

    const std::array<Mii, kMiiSlots>& Slots() const { return mSlots; }
    std::size_t Count() const;  // how many slots are used
    std::optional<std::size_t> FirstFree() const;

    // Places a Mii in a slot, or removes one. Slots keep their positions.
    bool Put(std::size_t slot, const Mii& mii);
    bool Remove(std::size_t slot);

    // The whole file (the size it was parsed at, never smaller than kMiiDatabaseSize), with the
    // slots written and the CRC recomputed.
    std::vector<std::uint8_t> Build() const;

private:
    std::array<Mii, kMiiSlots> mSlots{};
    // The whole file image; everything outside the slot table is carried through untouched.
    std::vector<std::uint8_t> mRest;
};

}  // namespace wiinand
