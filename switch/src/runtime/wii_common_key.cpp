#include "wii_common_key.h"

namespace kartpad::disc {
namespace {

// Public fixed constants, identical to the reference KartPad ports
// (ref/kartpad-main/apple/ios/KartPadDiscFormats.cpp) and Dolphin. Index 0 is
// the retail key used by every commercial PAL/NTSC disc, including MKW RMCP01.
constexpr std::uint8_t kRetailCommonKey[16] = {
    0xeb, 0xe4, 0x2a, 0x22, 0x5e, 0x85, 0x93, 0xe4,
    0x48, 0xd9, 0xc5, 0x45, 0x73, 0x81, 0xaa, 0xf7};
constexpr std::uint8_t kKoreanCommonKey[16] = {
    0x63, 0xb8, 0x2b, 0xb4, 0xf4, 0x61, 0x4e, 0x2e,
    0x13, 0xf2, 0xfe, 0xfb, 0xba, 0x4c, 0x9b, 0x7e};

}  // namespace

const std::uint8_t* wiiCommonKey(std::uint8_t commonKeyIndex) {
    switch (commonKeyIndex) {
        case 0: return kRetailCommonKey;
        case 1: return kKoreanCommonKey;
        default: return nullptr;
    }
}

}  // namespace kartpad::disc
