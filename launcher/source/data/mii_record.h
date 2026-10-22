// Reads and writes the 74-byte Mii record through mkw::MiiEntry. Layout: docs/launcher/research/mii-format.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "mkw/data.h"

namespace mkw::record {

inline constexpr std::size_t kSize = 0x4A;

// Every field MiiEntry carries, from the record. Bits it does not carry (the face's mingle and
// downloaded flags, reserved bits) are not read and not written.
MiiEntry DecodeFields(const std::uint8_t* raw, int slot);

// Writes the fields MiiEntry carries into `raw`. Values are clamped to each field's bit width and
// valid range. Bits outside those fields are kept. Identity (Mii ID, system ID, creator) is not touched.
void EncodeFields(const MiiEntry& e, std::uint8_t* raw);

// Writes the Mii ID, system ID and creator name.
void SetIdentity(std::uint8_t* raw, std::uint32_t miiId, std::uint32_t systemId, const std::string& creator);

// True when the face block (0x20..0x35) is all zero. A Mii with that block has no face.
bool FaceBlockEmpty(const std::uint8_t* raw);

// Sets the face, hair, eyes, eyebrows, nose, lips, glasses, facial hair, mole and birthday to the
// Mii Channel's defaults for the gender (WheelWizard MiiFactory). Name, colour, size and identity are kept.
void ApplyFactoryFace(MiiEntry& e, bool female);

// A new entry: the factory defaults for `female`, with the given name and favourite colour.
MiiEntry FactoryEntry(const std::string& name, bool female, int favoriteColor);

}  // namespace mkw::record
