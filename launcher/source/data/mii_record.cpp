#include "data/mii_record.h"

#include <algorithm>
#include <cstring>

#include "data/mii_template.h"
#include "data/text_utf16.h"

namespace mkw::record {
namespace {

// Byte offsets of the packed registers. Each field is a bit range inside one of them.
constexpr std::size_t kFlagsAt = 0x00;
constexpr std::size_t kFaceAt = 0x20;
constexpr std::size_t kHairAt = 0x22;
constexpr std::size_t kBrowAt = 0x24;  // u32
constexpr std::size_t kEyeAt = 0x28;   // u32
constexpr std::size_t kNoseAt = 0x2C;
constexpr std::size_t kLipAt = 0x2E;
constexpr std::size_t kGlassesAt = 0x30;
constexpr std::size_t kFacialHairAt = 0x32;
constexpr std::size_t kMoleAt = 0x34;
constexpr std::size_t kFaceEnd = 0x36;

int Clamp(int v, int lo, int hi) { return std::clamp(v, lo, hi); }

// Writes `value` into bits [shift, shift+width) of a big-endian u16 at p, leaving the rest alone.
void Put16(std::uint8_t* p, unsigned shift, unsigned width, int value) {
    const std::uint16_t mask = static_cast<std::uint16_t>(((1u << width) - 1u) << shift);
    const auto v = static_cast<std::uint16_t>(static_cast<unsigned>(value) << shift);
    text::Write16(p, static_cast<std::uint16_t>((text::Read16(p) & ~mask) | (v & mask)));
}

void Put32(std::uint8_t* p, unsigned shift, unsigned width, int value) {
    const std::uint32_t mask = ((1u << width) - 1u) << shift;
    const std::uint32_t v = static_cast<std::uint32_t>(value) << shift;
    text::Write32(p, (text::Read32(p) & ~mask) | (v & mask));
}

int Get16(const std::uint8_t* p, unsigned shift, unsigned width) {
    return static_cast<int>((text::Read16(p) >> shift) & ((1u << width) - 1u));
}

int Get32(const std::uint8_t* p, unsigned shift, unsigned width) {
    return static_cast<int>((text::Read32(p) >> shift) & ((1u << width) - 1u));
}

}  // namespace

MiiEntry DecodeFields(const std::uint8_t* raw, int slot) {
    MiiEntry e;
    e.slot = slot;
    e.name = text::ReadName(raw + mii_template::kName, 10);
    e.creator = text::ReadName(raw + mii_template::kCreator, 10);
    e.female = Get16(raw + kFlagsAt, 14, 1) != 0;
    e.birthMonth = Get16(raw + kFlagsAt, 10, 4);
    e.birthDay = Get16(raw + kFlagsAt, 5, 5);
    e.favoriteColor = Get16(raw + kFlagsAt, 1, 4);
    e.favorite = Get16(raw + kFlagsAt, 0, 1) != 0;
    e.height = raw[mii_template::kHeight] & 0x7F;
    e.weight = raw[mii_template::kWeight] & 0x7F;
    e.miiId = text::Read32(raw + mii_template::kMiiId);
    e.systemId = text::Read32(raw + mii_template::kSystemId);

    e.faceShape = Get16(raw + kFaceAt, 13, 3);
    e.skinColor = Get16(raw + kFaceAt, 10, 3);
    e.facialFeature = Get16(raw + kFaceAt, 6, 4);

    e.hairType = Get16(raw + kHairAt, 9, 7);
    e.hairColor = Get16(raw + kHairAt, 6, 3);
    e.hairFlipped = Get16(raw + kHairAt, 5, 1) != 0;

    e.browType = Get32(raw + kBrowAt, 27, 5);
    e.browRotation = Get32(raw + kBrowAt, 22, 4);
    e.browColor = Get32(raw + kBrowAt, 13, 3);
    e.browSize = Get32(raw + kBrowAt, 9, 4);
    e.browVertical = Get32(raw + kBrowAt, 4, 5);
    e.browSpacing = Get32(raw + kBrowAt, 0, 4);

    e.eyeType = Get32(raw + kEyeAt, 26, 6);
    e.eyeRotation = Get32(raw + kEyeAt, 21, 3);
    e.eyeVertical = Get32(raw + kEyeAt, 16, 5);
    e.eyeColor = Get32(raw + kEyeAt, 13, 3);
    e.eyeSize = Get32(raw + kEyeAt, 9, 3);
    e.eyeSpacing = Get32(raw + kEyeAt, 5, 4);

    e.noseType = Get16(raw + kNoseAt, 12, 4);
    e.noseSize = Get16(raw + kNoseAt, 8, 4);
    e.noseVertical = Get16(raw + kNoseAt, 3, 5);

    e.lipType = Get16(raw + kLipAt, 11, 5);
    e.lipColor = Get16(raw + kLipAt, 9, 2);
    e.lipSize = Get16(raw + kLipAt, 5, 4);
    e.lipVertical = Get16(raw + kLipAt, 0, 5);

    e.glassesType = Get16(raw + kGlassesAt, 12, 4);
    e.glassesColor = Get16(raw + kGlassesAt, 9, 3);
    e.glassesSize = Get16(raw + kGlassesAt, 5, 3);
    e.glassesVertical = Get16(raw + kGlassesAt, 0, 5);

    e.mustacheType = Get16(raw + kFacialHairAt, 14, 2);
    e.beardType = Get16(raw + kFacialHairAt, 12, 2);
    e.facialHairColor = Get16(raw + kFacialHairAt, 9, 3);
    e.mustacheSize = Get16(raw + kFacialHairAt, 5, 4);
    e.mustacheVertical = Get16(raw + kFacialHairAt, 0, 5);

    e.moleOn = Get16(raw + kMoleAt, 15, 1) != 0;
    e.moleSize = Get16(raw + kMoleAt, 11, 4);
    e.moleVertical = Get16(raw + kMoleAt, 6, 5);
    e.moleHorizontal = Get16(raw + kMoleAt, 1, 5);
    return e;
}

void EncodeFields(const MiiEntry& e, std::uint8_t* raw) {
    // Flags. Bit 15 (the empty-slot marker) is never written here.
    Put16(raw + kFlagsAt, 14, 1, e.female ? 1 : 0);
    Put16(raw + kFlagsAt, 10, 4, Clamp(e.birthMonth, 1, 12));
    Put16(raw + kFlagsAt, 5, 5, Clamp(e.birthDay, 1, 31));
    Put16(raw + kFlagsAt, 1, 4, Clamp(e.favoriteColor, 0, 11));
    Put16(raw + kFlagsAt, 0, 1, e.favorite ? 1 : 0);
    if (!e.name.empty()) text::WriteName(raw + mii_template::kName, 10, e.name);
    raw[mii_template::kHeight] = static_cast<std::uint8_t>(Clamp(e.height, 0, 127));
    raw[mii_template::kWeight] = static_cast<std::uint8_t>(Clamp(e.weight, 0, 127));

    Put16(raw + kFaceAt, 13, 3, Clamp(e.faceShape, 0, 7));
    Put16(raw + kFaceAt, 10, 3, Clamp(e.skinColor, 0, 5));
    Put16(raw + kFaceAt, 6, 4, Clamp(e.facialFeature, 0, 11));

    Put16(raw + kHairAt, 9, 7, Clamp(e.hairType, 0, 71));
    Put16(raw + kHairAt, 6, 3, Clamp(e.hairColor, 0, 7));
    Put16(raw + kHairAt, 5, 1, e.hairFlipped ? 1 : 0);

    Put32(raw + kBrowAt, 27, 5, Clamp(e.browType, 0, 23));
    Put32(raw + kBrowAt, 22, 4, Clamp(e.browRotation, 0, 11));
    Put32(raw + kBrowAt, 13, 3, Clamp(e.browColor, 0, 7));
    Put32(raw + kBrowAt, 9, 4, Clamp(e.browSize, 0, 8));
    Put32(raw + kBrowAt, 4, 5, Clamp(e.browVertical, 3, 18));
    Put32(raw + kBrowAt, 0, 4, Clamp(e.browSpacing, 0, 12));

    Put32(raw + kEyeAt, 26, 6, Clamp(e.eyeType, 0, 47));
    Put32(raw + kEyeAt, 21, 3, Clamp(e.eyeRotation, 0, 7));
    Put32(raw + kEyeAt, 16, 5, Clamp(e.eyeVertical, 0, 18));
    Put32(raw + kEyeAt, 13, 3, Clamp(e.eyeColor, 0, 5));
    Put32(raw + kEyeAt, 9, 3, Clamp(e.eyeSize, 0, 7));
    Put32(raw + kEyeAt, 5, 4, Clamp(e.eyeSpacing, 0, 12));

    Put16(raw + kNoseAt, 12, 4, Clamp(e.noseType, 0, 11));
    Put16(raw + kNoseAt, 8, 4, Clamp(e.noseSize, 0, 8));
    Put16(raw + kNoseAt, 3, 5, Clamp(e.noseVertical, 0, 18));

    Put16(raw + kLipAt, 11, 5, Clamp(e.lipType, 0, 23));
    Put16(raw + kLipAt, 9, 2, Clamp(e.lipColor, 0, 2));
    Put16(raw + kLipAt, 5, 4, Clamp(e.lipSize, 0, 8));
    Put16(raw + kLipAt, 0, 5, Clamp(e.lipVertical, 0, 18));

    Put16(raw + kGlassesAt, 12, 4, Clamp(e.glassesType, 0, 8));
    Put16(raw + kGlassesAt, 9, 3, Clamp(e.glassesColor, 0, 5));
    Put16(raw + kGlassesAt, 5, 3, Clamp(e.glassesSize, 0, 7));
    Put16(raw + kGlassesAt, 0, 5, Clamp(e.glassesVertical, 0, 20));

    Put16(raw + kFacialHairAt, 14, 2, Clamp(e.mustacheType, 0, 3));
    Put16(raw + kFacialHairAt, 12, 2, Clamp(e.beardType, 0, 3));
    Put16(raw + kFacialHairAt, 9, 3, Clamp(e.facialHairColor, 0, 7));
    Put16(raw + kFacialHairAt, 5, 4, Clamp(e.mustacheSize, 0, 8));
    Put16(raw + kFacialHairAt, 0, 5, Clamp(e.mustacheVertical, 0, 16));

    Put16(raw + kMoleAt, 15, 1, e.moleOn ? 1 : 0);
    Put16(raw + kMoleAt, 11, 4, Clamp(e.moleSize, 0, 8));
    Put16(raw + kMoleAt, 6, 5, Clamp(e.moleVertical, 0, 30));
    Put16(raw + kMoleAt, 1, 5, Clamp(e.moleHorizontal, 0, 16));
}

void SetIdentity(std::uint8_t* raw, std::uint32_t miiId, std::uint32_t systemId, const std::string& creator) {
    text::Write32(raw + mii_template::kMiiId, miiId);
    text::Write32(raw + mii_template::kSystemId, systemId);
    if (!creator.empty()) text::WriteName(raw + mii_template::kCreator, 10, creator);
}

bool FaceBlockEmpty(const std::uint8_t* raw) {
    return std::all_of(raw + kFaceAt, raw + kFaceEnd, [](std::uint8_t b) { return b == 0; });
}

void ApplyFactoryFace(MiiEntry& e, bool female) {
    // WheelWizard MiiFactory.CreateDefaultBase, then the male or female overrides.
    e.birthMonth = mii_template::kBirthMonth;
    e.birthDay = mii_template::kBirthDay;
    e.faceShape = 0;      // Teardrop
    e.skinColor = 0;      // Light
    e.facialFeature = 0;  // None
    e.hairFlipped = false;
    e.hairColor = 1;      // Brown
    e.noseType = 1;       // SemiCircle
    e.noseSize = 4;
    e.noseVertical = 9;
    e.lipType = 23;
    e.lipColor = 0;       // Skin
    e.lipSize = 4;
    e.lipVertical = 13;
    e.glassesType = 0;    // None
    e.glassesColor = 0;   // Grey
    e.glassesSize = 4;
    e.glassesVertical = 10;
    e.mustacheType = 0;
    e.beardType = 0;
    e.facialHairColor = 0;  // Black
    e.mustacheSize = 4;
    e.mustacheVertical = 10;
    e.moleOn = false;
    e.moleSize = 4;
    e.moleVertical = 20;
    e.moleHorizontal = 2;
    e.browRotation = 6;
    e.browColor = 1;  // Brown
    e.browSize = 4;
    e.browVertical = 10;
    e.browSpacing = 2;
    e.eyeVertical = 12;
    e.eyeColor = 0;  // Black
    e.eyeSize = 4;
    e.eyeSpacing = 2;
    e.female = female;
    if (female) {
        e.hairType = 12;
        e.browType = 0;
        e.eyeType = 4;
        e.eyeRotation = 3;
    } else {
        e.hairType = 33;
        e.browType = 6;
        e.eyeType = 2;
        e.eyeRotation = 4;
    }
}

MiiEntry FactoryEntry(const std::string& name, bool female, int favoriteColor) {
    MiiEntry e;
    e.name = name;
    e.favoriteColor = favoriteColor;
    e.favorite = false;
    e.height = mii_template::kDefaultHeight;
    e.weight = mii_template::kDefaultWeight;
    e.creator = mii_template::kCreatorName;
    ApplyFactoryFace(e, female);
    return e;
}

}  // namespace mkw::record
