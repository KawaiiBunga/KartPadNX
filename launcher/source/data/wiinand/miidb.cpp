#include "wiinand/miidb.h"

#include <algorithm>
#include <cstring>

namespace wiinand {
namespace {

// The flags word at 0x00 of a Mii.
constexpr std::size_t kFlagsOffset = 0x00;
constexpr std::uint16_t kInvalidFlag = 0x8000;  // set while a slot is empty

std::uint16_t Read16(const std::uint8_t* data) {
    return static_cast<std::uint16_t>((data[0] << 8) | data[1]);
}

// The fixed bytes of a database: magics, the 0x80 at the end of the table, and the parade fill.
void StampMarkers(std::vector<std::uint8_t>& file) {
    std::memcpy(file.data(), "RNOD", 4);
    file[kMiiEmptyMarkOffset] = 0x80;
    std::memcpy(file.data() + kMiiParadeOffset, "RNHD", 4);
    std::memset(file.data() + kMiiParadeOffset + 4, 0xFF, 4);
}

bool HasMagics(const std::vector<std::uint8_t>& file) {
    return std::memcmp(file.data(), "RNOD", 4) == 0 && std::memcmp(file.data() + kMiiParadeOffset, "RNHD", 4) == 0;
}

}  // namespace

std::uint16_t Crc16Ccitt(const std::uint8_t* data, std::size_t length) {
    std::uint16_t crc = 0;
    for (std::size_t index = 0; index < length; index++) {
        crc ^= static_cast<std::uint16_t>(data[index]) << 8;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000u) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021u)
                                  : static_cast<std::uint16_t>(crc << 1);
        }
    }
    return crc;
}

bool Mii::Used() const {
    if (std::all_of(raw.begin(), raw.end(), [](std::uint8_t byte) { return byte == 0; })) {
        return false;
    }
    return (Read16(raw.data() + kFlagsOffset) & kInvalidFlag) == 0;
}

// static
MiiDatabase MiiDatabase::Empty() {
    MiiDatabase database;
    database.mRest.assign(kMiiDatabaseSize, 0);
    StampMarkers(database.mRest);
    return database;
}

// static
std::optional<MiiDatabase> MiiDatabase::Parse(const std::vector<std::uint8_t>& file, bool* checksumValid,
                                              bool* migrated) {
    if (file.size() < kLegacyMiiDatabaseSize || !HasMagics(file)) {
        return std::nullopt;
    }
    if (checksumValid != nullptr) {
        const std::uint16_t stored = Read16(file.data() + kMiiCrcOffset);
        *checksumValid = stored == Crc16Ccitt(file.data(), kMiiCrcOffset);
    }

    MiiDatabase database;
    database.mRest = file;
    if (file.size() == kLegacyMiiDatabaseSize) {
        // Old launcher format: extend to the complete size, with the markers a new database has.
        database.mRest.resize(kMiiDatabaseSize, 0);
        StampMarkers(database.mRest);
        if (migrated != nullptr) *migrated = true;
    } else if (migrated != nullptr) {
        *migrated = false;
    }
    for (std::size_t slot = 0; slot < kMiiSlots; slot++) {
        const std::size_t offset = kMiiEntriesOffset + slot * kMiiEntrySize;
        std::memcpy(database.mSlots[slot].raw.data(), database.mRest.data() + offset, kMiiEntrySize);
    }
    return database;
}

std::size_t MiiDatabase::Count() const {
    return static_cast<std::size_t>(
        std::count_if(mSlots.begin(), mSlots.end(), [](const Mii& mii) { return mii.Used(); }));
}

std::optional<std::size_t> MiiDatabase::FirstFree() const {
    for (std::size_t slot = 0; slot < kMiiSlots; slot++) {
        if (!mSlots[slot].Used()) {
            return slot;
        }
    }
    return std::nullopt;
}

bool MiiDatabase::Put(std::size_t slot, const Mii& mii) {
    if (slot >= kMiiSlots) {
        return false;
    }
    mSlots[slot] = mii;
    return true;
}

bool MiiDatabase::Remove(std::size_t slot) {
    if (slot >= kMiiSlots) {
        return false;
    }
    mSlots[slot] = Mii{};
    return true;
}

std::vector<std::uint8_t> MiiDatabase::Build() const {
    std::vector<std::uint8_t> file = mRest;
    if (file.size() < kMiiDatabaseSize) {
        file.resize(kMiiDatabaseSize, 0);
    }
    // The markers came in with the file (or with Empty/migration), so Build keeps them as they are.
    for (std::size_t slot = 0; slot < kMiiSlots; slot++) {
        std::memcpy(file.data() + kMiiEntriesOffset + slot * kMiiEntrySize, mSlots[slot].raw.data(), kMiiEntrySize);
    }
    const std::uint16_t crc = Crc16Ccitt(file.data(), kMiiCrcOffset);
    file[kMiiCrcOffset] = static_cast<std::uint8_t>(crc >> 8);
    file[kMiiCrcOffset + 1] = static_cast<std::uint8_t>(crc & 0xFF);
    return file;
}

}  // namespace wiinand
