// The Switch's own Miis (the system Mii database, made in System Settings' Mii editor) as Wii Miis,
// so both games list them when a licence picks its Mii. MiiService::SyncFromSwitch writes them into
// RFL_DB.dat; nothing on the Switch side changes.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "mkw/data.h"

namespace mkw::switch_mii {

// The fields of a Switch Mii (nn::mii::CharInfo, libnx MiiCharInfo) the conversion reads.
struct SwitchMii {
    std::array<std::uint8_t, 16> createId{};  // the Mii's UUID: its identity across syncs
    std::u16string name;
    int favoriteColor = 0, female = 0, height = 64, width = 64;
    int faceShape = 0, faceColor = 0, wrinkles = 0, makeup = 0;
    int hairStyle = 0, hairColor = 0, hairFlip = 0;
    int eyeStyle = 0, eyeColor = 0, eyeSize = 0, eyeAngle = 0, eyePosX = 0, eyePosY = 0;
    int browStyle = 0, browColor = 0, browSize = 0, browAngle = 0, browPosX = 0, browPosY = 0;
    int noseStyle = 0, noseSize = 0, nosePos = 0;
    int mouthStyle = 0, mouthColor = 0, mouthSize = 0, mouthPos = 0;
    int facialHairColor = 0, beardStyle = 0, mustacheStyle = 0, mustacheSize = 0, mustachePos = 0;
    int glassesStyle = 0, glassesColor = 0, glassesSize = 0, glassesPos = 0;
    int moleOn = 0, moleSize = 0, molePosX = 0, molePosY = 0;
};

// The creator name a synced Mii carries in RFL_DB.dat.
inline constexpr const char* kCreatorName = "Switch";

// The Wii fields for a Switch Mii. Parts and colours the Wii has keep their values (the Switch kept
// the Wii's part numbers; classic colours map one to one). Parts only the Switch has become the
// Wii default for the gender, and its extra colours the Wii's first colour.
MiiEntry ToWii(const SwitchMii& mii);

// A Wii Mii ID for the Mii (normal type), the same on every sync: from its UUID.
std::uint32_t WiiMiiId(const SwitchMii& mii);

// The console's own Miis (mii:u). False with a message when the Mii service cannot be read; an empty
// list is not an error. Off the Switch it always returns an empty list.
bool ReadSystemMiis(std::vector<SwitchMii>* out, std::string* error);

// The System Settings Mii editor as a library applet (create, edit, delete). Switch only.
bool ShowMiiEditor(std::string* error);

}  // namespace mkw::switch_mii
