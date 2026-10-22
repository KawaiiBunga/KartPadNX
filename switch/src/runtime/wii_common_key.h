// Embedded Wii common keys, so on-console extraction needs no user-supplied key
// (matching the PC/mobile KartPad and WiiCompiled ports, and Dolphin).
//
// The Wii common key is a fixed 16-byte constant, not Nintendo game code or
// assets and not part of the user's dump. The retail and Korean keys are the
// same public constants the reference KartPad ports embed
// (ref/kartpad-main/apple/ios/KartPadDiscFormats.cpp) and are selected per disc
// by the ticket's common_key_index. Kept in its own file so wii_crypto stays
// pure logic and off-device tests that inject their own key need not link this.
#pragma once

#include <cstdint>

namespace kartpad::disc {

// Returns the 16-byte common key for `commonKeyIndex` (0 = retail, 1 = Korean),
// or nullptr for an index this port does not carry a key for.
const std::uint8_t* wiiCommonKey(std::uint8_t commonKeyIndex);

}  // namespace kartpad::disc
