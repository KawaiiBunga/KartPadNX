#include "data/switch_mii.h"

#include <algorithm>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "data/mii_record.h"
#include "data/mii_template.h"

namespace mkw::switch_mii {
namespace {

// Switch common colours are 100 entries; the Wii's classic colours are a subset of them (nn::mii's
// Ver3 tables). Hair, eyebrow and beard: Wii 0..7 = common 8, 1..7. Eyes: Wii 0..5 = common 8..13.
// Glasses: Wii 0 = common 8, Wii 1..5 = common 14..18. Lips: Ver3 0..4 = common 19..23, of which the
// Wii has the first three (orange, red, pink).
int HairColor(int common) {
    if (common >= 1 && common <= 7) return common;
    return 0;  // 8 is black; colours the Wii does not have become black as well
}

int EyeColor(int common) { return common >= 8 && common <= 13 ? common - 8 : 0; }

int GlassesColor(int common) { return common >= 14 && common <= 18 ? common - 13 : 0; }

int LipColor(int common) {
    static constexpr int kVer3ToWii[] = {0, 1, 2, 0, 1};  // peach as orange, black-red as red
    return common >= 19 && common <= 23 ? kVer3ToWii[common - 19] : 0;
}

// Faceline colour: the Switch's 10 against the Wii's 6 (nn::mii Ver3FacelineColorTable).
int SkinColor(int faceColor) {
    static constexpr int kTable[] = {0, 1, 2, 3, 4, 5, 0, 1, 5, 4};
    return faceColor >= 0 && faceColor < 10 ? kTable[faceColor] : 0;
}

// The Wii's one "facial feature" from the Switch's makeup and wrinkle styles. Wii: 0 none, 1 blush,
// 2 makeup and blush, 3 freckles, 4 bags under the eyes, 5 cheek wrinkles, 6 wrinkles near the eyes,
// 7 chin wrinkle, 8 makeup, 9 stubble, 10 wrinkles near the mouth, 11 wrinkles.
int FacialFeature(int makeup, int wrinkles) {
    if (makeup >= 1 && makeup <= 2) return 1;
    if (makeup >= 3 && makeup <= 5) return 8;
    if (makeup >= 6 && makeup <= 8) return 2;
    if (makeup >= 9) return 3;
    static constexpr int kWrinkles[] = {0, 4, 4, 4, 5, 5, 6, 6, 7, 9, 10, 11};
    return wrinkles >= 0 && wrinkles < 12 ? kWrinkles[wrinkles] : 0;
}

// A part the Wii has keeps its number; a Switch-only one becomes `fallback`.
int Part(int value, int wiiCount, int fallback) { return value >= 0 && value < wiiCount ? value : fallback; }

std::string Utf8(const std::u16string& s) {
    std::string out;
    for (const char16_t c : s) {
        if (c == 0) break;
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

}  // namespace

MiiEntry ToWii(const SwitchMii& m) {
    const bool female = m.female != 0;
    MiiEntry e = record::FactoryEntry(Utf8(m.name), female, std::clamp(m.favoriteColor, 0, kMiiFavoriteColors - 1));
    const MiiEntry defaults = e;
    e.creator = kCreatorName;
    e.height = std::clamp(m.height, 0, 127);
    e.weight = std::clamp(m.width, 0, 127);

    e.faceShape = Part(m.faceShape, 8, defaults.faceShape);
    e.skinColor = SkinColor(m.faceColor);
    e.facialFeature = FacialFeature(m.makeup, m.wrinkles);

    e.hairType = Part(m.hairStyle, 72, defaults.hairType);
    e.hairColor = HairColor(m.hairColor);
    e.hairFlipped = m.hairFlip != 0;

    e.eyeType = Part(m.eyeStyle, 48, defaults.eyeType);
    e.eyeColor = EyeColor(m.eyeColor);
    e.eyeSize = m.eyeSize;
    e.eyeRotation = m.eyeAngle;
    e.eyeSpacing = m.eyePosX;
    e.eyeVertical = m.eyePosY;

    e.browType = Part(m.browStyle, 24, defaults.browType);
    e.browColor = HairColor(m.browColor);
    e.browSize = m.browSize;
    e.browRotation = m.browAngle;
    e.browSpacing = m.browPosX;
    e.browVertical = m.browPosY;

    e.noseType = Part(m.noseStyle, 12, defaults.noseType);
    e.noseSize = m.noseSize;
    e.noseVertical = m.nosePos;

    e.lipType = Part(m.mouthStyle, 24, defaults.lipType);
    e.lipColor = LipColor(m.mouthColor);
    e.lipSize = m.mouthSize;
    e.lipVertical = m.mouthPos;

    e.facialHairColor = HairColor(m.facialHairColor);
    e.beardType = Part(m.beardStyle, 4, 3);
    e.mustacheType = Part(m.mustacheStyle, 4, 3);
    e.mustacheSize = m.mustacheSize;
    e.mustacheVertical = m.mustachePos;

    e.glassesType = Part(m.glassesStyle, 9, 1);
    e.glassesColor = GlassesColor(m.glassesColor);
    e.glassesSize = m.glassesSize;
    e.glassesVertical = m.glassesPos;

    e.moleOn = m.moleOn != 0;
    e.moleSize = m.moleSize;
    e.moleHorizontal = m.molePosX;
    e.moleVertical = m.molePosY;
    return e;
}

std::uint32_t WiiMiiId(const SwitchMii& m) {
    std::uint32_t h = 2166136261u;  // FNV-1a
    for (const std::uint8_t b : m.createId) h = (h ^ b) * 16777619u;
    return mii_template::kNormalMiiIdBit | (h & mii_template::kTimestampMask);
}

#ifdef __SWITCH__
namespace {

// libnx documents the nickname as UTF-16BE; nn::mii stores char16_t (little-endian here). Read it
// the way that gives plain characters.
std::u16string Nickname(const u16* raw) {
    std::u16string le, be;
    int beHints = 0, leHints = 0;
    for (int i = 0; i < 10 && raw[i] != 0; ++i) {
        const u16 v = raw[i];
        const u16 swapped = static_cast<u16>((v >> 8) | (v << 8));
        le += static_cast<char16_t>(v);
        be += static_cast<char16_t>(swapped);
        if (v < 0x100) ++leHints;
        if (swapped < 0x100) ++beHints;
    }
    return beHints > leHints ? be : le;
}

SwitchMii FromCharInfo(const MiiCharInfo& c) {
    SwitchMii m;
    std::copy(c.create_id.uuid.uuid, c.create_id.uuid.uuid + 16, m.createId.begin());
    m.name = Nickname(c.mii_name);
    m.favoriteColor = c.mii_color;
    m.female = c.mii_sex;
    m.height = c.mii_height;
    m.width = c.mii_width;
    m.faceShape = c.mii_face_shape;
    m.faceColor = c.mii_face_color;
    m.wrinkles = c.mii_wrinkles_style;
    m.makeup = c.mii_makeup_style;
    m.hairStyle = c.mii_hair_style;
    m.hairColor = c.mii_hair_color;
    m.hairFlip = c.mii_has_hair_flipped;
    m.eyeStyle = c.mii_eye_style;
    m.eyeColor = c.mii_eye_color;
    m.eyeSize = c.mii_eye_size;
    m.eyeAngle = c.mii_eye_angle;
    m.eyePosX = c.mii_eye_pos_x;
    m.eyePosY = c.mii_eye_pos_y;
    m.browStyle = c.mii_eyebrow_style;
    m.browColor = c.mii_eyebrow_color;
    m.browSize = c.mii_eyebrow_size;
    m.browAngle = c.mii_eyebrow_angle;
    m.browPosX = c.mii_eyebrow_pos_x;
    m.browPosY = c.mii_eyebrow_pos_y;
    m.noseStyle = c.mii_nose_style;
    m.noseSize = c.mii_nose_size;
    m.nosePos = c.mii_nose_pos;
    m.mouthStyle = c.mii_mouth_style;
    m.mouthColor = c.mii_mouth_color;
    m.mouthSize = c.mii_mouth_size;
    m.mouthPos = c.mii_mouth_pos;
    m.facialHairColor = c.mii_facial_hair_color;
    m.beardStyle = c.mii_beard_style;
    m.mustacheStyle = c.mii_mustache_style;
    m.mustacheSize = c.mii_mustache_size;
    m.mustachePos = c.mii_mustache_pos;
    m.glassesStyle = c.mii_glasses_style;
    m.glassesColor = c.mii_glasses_color;
    m.glassesSize = c.mii_glasses_size;
    m.glassesPos = c.mii_glasses_pos;
    m.moleOn = c.mii_has_mole;
    m.moleSize = c.mii_mole_size;
    m.molePosX = c.mii_mole_pos_x;
    m.molePosY = c.mii_mole_pos_y;
    return m;
}

}  // namespace

bool ReadSystemMiis(std::vector<SwitchMii>* out, std::string* error) {
    out->clear();
    if (R_FAILED(miiInitialize(MiiServiceType_User))) {
        if (error) *error = "The Switch's Mii service did not start.";
        return false;
    }
    MiiDatabase db;
    bool ok = R_SUCCEEDED(miiOpenDatabase(&db, MiiSpecialKeyCode_Normal));
    if (ok) {
        s32 count = 0;
        ok = R_SUCCEEDED(miiDatabaseGetCount(&db, &count, MiiSourceFlag_Database));
        if (ok && count > 0) {
            std::vector<MiiCharInfo> infos(static_cast<std::size_t>(count));
            s32 total = 0;
            ok = R_SUCCEEDED(miiDatabaseGet1(&db, MiiSourceFlag_Database, infos.data(), count, &total));
            for (s32 i = 0; ok && i < std::min(total, count); ++i) out->push_back(FromCharInfo(infos[static_cast<std::size_t>(i)]));
        }
        miiDatabaseClose(&db);
    }
    miiExit();
    if (!ok && error) *error = "Could not read the Switch's Miis.";
    return ok;
}

bool ShowMiiEditor(std::string* error) {
    if (R_FAILED(miiLaShowMiiEdit(MiiSpecialKeyCode_Normal))) {
        if (error) *error = "The Mii editor could not be opened.";
        return false;
    }
    return true;
}
#else
bool ReadSystemMiis(std::vector<SwitchMii>* out, std::string*) {
    out->clear();
    return true;
}

bool ShowMiiEditor(std::string* error) {
    if (error) *error = "The Mii editor is part of the Switch.";
    return false;
}
#endif

}  // namespace mkw::switch_mii
