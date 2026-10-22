// Host test for the Switch Mii -> Wii Mii conversion (data/switch_mii.cpp).
#include <iostream>

#include "data/mii_record.h"
#include "data/switch_mii.h"
#include "wiinand/miidb.h"

namespace {

int g_failures = 0;
void Expect(bool cond, const char* what) {
    std::cout << (cond ? "PASS " : "FAIL ") << what << '\n';
    if (!cond) ++g_failures;
}

}  // namespace

int main() {
    using mkw::switch_mii::SwitchMii;
    SwitchMii m;
    m.createId = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    m.name = u"Ana";
    m.female = 1;
    m.favoriteColor = 7;
    m.height = 90;
    m.width = 30;
    m.faceShape = 3;
    m.faceColor = 8;      // a Switch-only skin tone
    m.hairStyle = 40;     // a Wii hairstyle
    m.hairColor = 8;      // classic black
    m.eyeStyle = 55;      // Switch only
    m.eyeColor = 12;      // classic blue
    m.browStyle = 5;
    m.browColor = 3;
    m.browPosY = 10;
    m.mouthStyle = 30;    // Switch only
    m.mouthColor = 21;    // pink
    m.glassesStyle = 12;  // Switch only
    m.glassesColor = 15;
    m.eyeSize = 5;
    m.eyePosY = 12;

    const mkw::MiiEntry e = mkw::switch_mii::ToWii(m);
    Expect(e.name == "Ana", "name copied");
    Expect(e.female && e.favoriteColor == 7 && e.height == 90 && e.weight == 30, "profile copied");
    Expect(e.creator == mkw::switch_mii::kCreatorName, "creator marks a Switch Mii");
    Expect(e.faceShape == 3, "Wii face shape kept");
    Expect(e.skinColor == 5, "Switch skin tone 8 maps to Wii 5");
    Expect(e.hairType == 40 && e.hairColor == 0, "hairstyle kept, common black is Wii black");
    Expect(e.eyeType == 4, "Switch-only eyes fall back to the female default");
    Expect(e.eyeColor == 4, "common 12 is Wii blue (4)");
    Expect(e.browType == 5 && e.browColor == 3 && e.browVertical == 10, "eyebrows copied");
    Expect(e.lipType == 23 && e.lipColor == 2, "Switch-only mouth falls back, pink kept");
    Expect(e.glassesType == 1 && e.glassesColor == 2, "Switch-only glasses become type 1, colour 15 is Wii 2");

    const std::uint32_t id = mkw::switch_mii::WiiMiiId(m);
    Expect((id & 0xE0000000u) == 0x80000000u, "Mii ID is a normal Mii's");
    Expect(id == mkw::switch_mii::WiiMiiId(m), "Mii ID is the same on every sync");
    SwitchMii other = m;
    other.createId[0] = 99;
    Expect(mkw::switch_mii::WiiMiiId(other) != id, "another Mii gets another ID");

    // The record round-trips through the 74-byte encoder.
    wiinand::Mii raw{};
    mkw::record::EncodeFields(e, raw.raw.data());
    const mkw::MiiEntry back = mkw::record::DecodeFields(raw.raw.data(), 0);
    Expect(back.eyeColor == 4 && back.hairType == 40 && back.female, "encodes into a Wii record");

    if (g_failures) std::cout << g_failures << " failure(s)\n";
    return g_failures ? 1 : 0;
}
