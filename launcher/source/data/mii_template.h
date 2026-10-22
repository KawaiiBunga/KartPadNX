// Constants for the 74-byte Mii record and for the Miis the launcher creates.
// Bit layout: docs/launcher/research/mii-format.md (checked against WheelWizard MiiSerializer).
#pragma once

#include <cstddef>
#include <cstdint>

namespace mkw::mii_template {

// Byte offsets inside one Mii record.
inline constexpr std::size_t kFlags = 0x00;     // u16 BE: invalid(15) female(14) month(13..10) day(9..5) colour(4..1) favourite(0)
inline constexpr std::size_t kName = 0x02;      // 10 x UTF-16BE
inline constexpr std::size_t kHeight = 0x16;
inline constexpr std::size_t kWeight = 0x17;
inline constexpr std::size_t kMiiId = 0x18;     // u32 BE
inline constexpr std::size_t kSystemId = 0x1C;  // u32 BE
inline constexpr std::size_t kFaceBegin = 0x20; // face block, 0x20..0x35
inline constexpr std::size_t kFaceEnd = 0x36;
inline constexpr std::size_t kCreator = 0x36;   // 10 x UTF-16BE

inline constexpr std::uint16_t kFemaleBit = 0x4000;
inline constexpr std::uint16_t kInvalidBit = 0x8000;

// Mii ID: top three bits 100 (normal Mii), then a timestamp in 4-second steps since 2006-01-01 UTC.
inline constexpr std::uint32_t kNormalMiiIdBit = 0x80000000u;
inline constexpr std::int64_t kWiiEpochUnix = 1136073600;
inline constexpr std::uint32_t kTimestampMask = 0x1FFFFFFFu;

// Values a new Mii starts with. Height and weight are MiiFactory's 63.
inline constexpr int kBirthMonth = 1;
inline constexpr int kBirthDay = 1;
inline constexpr std::uint8_t kDefaultHeight = 63;
inline constexpr std::uint8_t kDefaultWeight = 63;

// The creator field holds 10 characters.
inline constexpr const char* kCreatorName = "KartPad-NX";

}  // namespace mkw::mii_template
