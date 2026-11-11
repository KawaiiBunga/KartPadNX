// Host test for mod_builder's formats: Yaz0, U8, PNG -> TPL, and sound-bank file replacement.
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

#include <zlib.h>

#include "data/mod_builder.h"
#include "test_png.h"

using namespace mkw::modbuild;

namespace mkw {
const char* VariantId(Variant v) { return v == Variant::RetroRewind ? "rr" : "vanilla"; }
}  // namespace mkw

namespace {

int g_failures = 0;
void Expect(bool cond, const std::string& what) {
    std::cout << (cond ? "PASS " : "FAIL ") << what << '\n';
    if (!cond) ++g_failures;
}

uint32_t Be32(const Bytes& b, size_t o) { return (uint32_t(b[o]) << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3]; }
void Put32(Bytes& b, size_t o, uint32_t v) {
    if (b.size() < o + 4) b.resize(o + 4, 0);
    b[o] = uint8_t(v >> 24), b[o + 1] = uint8_t(v >> 16), b[o + 2] = uint8_t(v >> 8), b[o + 3] = uint8_t(v);
}
Bytes Str(const char* s) { return Bytes(s, s + std::strlen(s)); }

// A sound bank with one group "GRP_A" holding an RWSD (file 0) and an RBNK (file 1).
Bytes MakeSoundArchive() {
    Bytes f(0x2C0, 0);
    std::memcpy(f.data(), "RSAR\xFE\xFF\x01\x04", 8);
    // SYMB at 0x40: body = string table offset, then the table with one name.
    const size_t symb = 0x40;
    std::memcpy(&f[symb], "SYMB", 4);
    Put32(f, symb + 4, 0x40);
    Put32(f, symb + 8, 0x14);
    Put32(f, symb + 8 + 0x14, 1);
    Put32(f, symb + 8 + 0x18, 0x1C);
    std::memcpy(&f[symb + 8 + 0x1C], "GRP_A", 5);
    // INFO at 0x80. Body offsets (relative to 0x88).
    const size_t info = 0x80, B = info + 8;
    std::memcpy(&f[info], "INFO", 4);
    Put32(f, B + 0x18 + 4, 0x40);  // file table
    Put32(f, B + 0x20 + 4, 0x60);  // group table
    Put32(f, B + 0x40, 2);
    Put32(f, B + 0x48, 0x80);
    Put32(f, B + 0x50, 0xA0);
    Put32(f, B + 0x80, 0x20), Put32(f, B + 0x84, 0x40);  // file 0: header 0x20, wave 0x40
    Put32(f, B + 0xA0, 0x20), Put32(f, B + 0xA4, 0x20);  // file 1: header 0x20, wave 0x20
    Put32(f, B + 0x60, 1);
    Put32(f, B + 0x68, 0xC0);
    const size_t g = B + 0xC0;  // group: name 0, offsets filled below, item table at 0x100
    Put32(f, g + 0x24, 0x100);
    Put32(f, B + 0x100, 2);
    Put32(f, B + 0x108, 0x120);
    Put32(f, B + 0x110, 0x140);
    Put32(f, B + 0x120, 0), Put32(f, B + 0x124, 0), Put32(f, B + 0x128, 0x20), Put32(f, B + 0x12C, 0), Put32(f, B + 0x130, 0x40);
    Put32(f, B + 0x140, 1), Put32(f, B + 0x144, 0x20), Put32(f, B + 0x148, 0x20), Put32(f, B + 0x14C, 0x40), Put32(f, B + 0x150, 0x20);
    Put32(f, info + 4, 0x200 - info);
    // FILE at 0x200: headers at 0x220 (RWSD, RBNK), waves at 0x260 (0x40 + 0x20).
    const size_t file = 0x200;
    std::memcpy(&f[file], "FILE", 4);
    std::memcpy(&f[0x220], "RWSD", 4), Put32(f, 0x228, 0x20);
    std::memcpy(&f[0x240], "RBNK", 4), Put32(f, 0x248, 0x20);
    std::memset(&f[0x260], 0xAA, 0x40);
    std::memset(&f[0x2A0], 0xBB, 0x20);
    Put32(f, g + 0x10, 0x220), Put32(f, g + 0x14, 0x40), Put32(f, g + 0x18, 0x260), Put32(f, g + 0x1C, 0x60);
    Put32(f, file + 4, 0x2C0 - file);
    Put32(f, 8, 0x2C0);
    Put32(f, 0x10, symb), Put32(f, 0x14, 0x40), Put32(f, 0x18, info), Put32(f, 0x1C, 0x180), Put32(f, 0x20, file),
        Put32(f, 0x24, 0xC0);
    return f;
}

}  // namespace

int main() {
    Bytes raw;
    for (int i = 0; i < 1000; ++i) raw.push_back(uint8_t(i * 7));
    Expect(Yaz0Decode(Yaz0Encode(raw)) == raw, "Yaz0 round trip");
    // A real back-reference: "ab" then copy 4 bytes from distance 2.
    const Bytes compressed = {'Y', 'a', 'z', '0', 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 'a', 'b', 0x20, 0x01};
    Expect(Yaz0Decode(compressed) == Str("ababab"), "Yaz0 back-references decode");

    U8Archive a;
    a.strings = Bytes{0, '.', 0, 'w', 'l', '.', 'b', 'r', 'r', 'e', 's', 0, 't', 'i', 'm', 'g', 0, 'x', '.', 't', 'p', 'l', 0};
    a.nodes.resize(5);
    a.nodes[0] = {1, 0, 0, 5, "", {}};
    a.nodes[1] = {1, 1, 0, 5, "", {}};
    a.nodes[2] = {0, 3, 0, 0, "", Str("model")};
    a.nodes[3] = {1, 12, 1, 5, "", {}};
    a.nodes[4] = {0, 17, 0, 0, "", Str("icon")};
    U8Archive back;
    Expect(U8Parse(Yaz0Decode(Yaz0Encode(U8Write(a))), back) && back.nodes.size() == 5 &&
               back.nodes[2].path == "./wl.brres" && back.nodes[4].path == "./timg/x.tpl" && back.nodes[4].data == Str("icon"),
           "U8 write/parse keeps paths and data");
    back.nodes[2].data = Str("a much longer replacement model");
    U8Archive again;
    Expect(U8Parse(U8Write(back), again) && again.nodes[2].data == back.nodes[2].data && again.nodes[4].data == Str("icon"),
           "a replaced member is written back with the others intact");

    const Bytes png = test::MakePng(2, 2, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 0});
    Bytes rgba;
    int w = 0, h = 0;
    Expect(PngDecode(png, rgba, w, h) && w == 2 && h == 2 && rgba[0] == 255 && rgba[5] == 255 && rgba[15] == 0,
           "PNG decodes to RGBA");
    const Bytes tpl = test::MakeTpl(4, 4);
    Bytes outTpl;
    Expect(TplReplace(tpl, rgba, w, h, outTpl) && Be32(outTpl, 0x14 + 4) == 5 && outTpl.size() == 0x40 + 32 &&
               outTpl[0x40] == 0xFC && outTpl[0x41] == 0x00,
           "PNG becomes the TPL's image as RGB5A3, scaled to its size");

    {
        std::ofstream("bank.brsar", std::ios::binary).write(reinterpret_cast<const char*>(MakeSoundArchive().data()), 0x2C0);
    }
    Bytes mod(0x30, 0);
    std::memcpy(mod.data(), "RWSD", 4);
    Put32(mod, 8, 0x30);
    mod.resize(0x30 + 0x50, 0xCC);  // header 0x30, wave 0x50
    std::string log;
    const int n = PatchSoundArchive("bank.brsar", "out.brsar", {{"grp_a", 0, mod}, {"GRP_MISSING", -1, mod}}, log);
    std::ifstream in("out.brsar", std::ios::binary);
    const Bytes o((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t B = 0x88, g = B + 0xC0;
    const bool ok = n == 1 && o.size() > 0x2C0 && Be32(o, 8) == o.size() && Be32(o, 0x24) == o.size() - 0x200 &&
                    Be32(o, 0x204) == o.size() - 0x200;
    Expect(ok, "sound bank grows and its header and FILE section cover the appended data");
    if (ok) {
        const uint32_t hoff = Be32(o, g + 0x10), woff = Be32(o, g + 0x18);
        Expect(std::memcmp(&o[hoff], "RWSD", 4) == 0 && Be32(o, B + 0x128) == 0x30 && Be32(o, B + 0x130) == 0x50 &&
                   o[woff] == 0xCC && Be32(o, B + 0x80) == 0x30 && Be32(o, B + 0x84) == 0x50,
               "the replaced file's data and sizes are in place");
        Expect(std::memcmp(&o[hoff + Be32(o, B + 0x144)], "RBNK", 4) == 0 && o[woff + Be32(o, B + 0x14C)] == 0xBB,
               "the group's other file moves with it, unchanged");
        Expect(std::memcmp(&o[0x220], "RWSD", 4) == 0 && log.find("GRP_MISSING") != std::string::npos,
               "old data stays put; an unknown group is logged");
    }

    std::cout << (g_failures ? "FAILED\n" : "All mod builder tests passed.\n");
    return g_failures ? 1 : 0;
}
