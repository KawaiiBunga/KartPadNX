// Tiny PNG and TPL writers for the mod tests.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <zlib.h>

namespace test {

inline void Put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s));
}

// RGBA8, filter 0 on every row.
inline std::vector<uint8_t> MakePng(int w, int h, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + y * w * 4, rgba.begin() + (y + 1) * w * 4);
    }
    std::vector<uint8_t> z(compressBound(raw.size()));
    uLongf zl = z.size();
    compress(z.data(), &zl, raw.data(), raw.size());
    z.resize(zl);
    std::vector<uint8_t> png = {137, 80, 78, 71, 13, 10, 26, 10};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        Put32(png, uint32_t(data.size()));
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        Put32(png, 0);  // CRC isn't checked
    };
    std::vector<uint8_t> ihdr;
    Put32(ihdr, w), Put32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return png;
}

// One RGB5A3 image: header at 0x14, data at 0x40.
inline std::vector<uint8_t> MakeTpl(int w, int h) {
    std::vector<uint8_t> t;
    Put32(t, 0x0020AF30), Put32(t, 1), Put32(t, 0xC), Put32(t, 0x14), Put32(t, 0);
    t.push_back(uint8_t(h >> 8)), t.push_back(uint8_t(h)), t.push_back(uint8_t(w >> 8)), t.push_back(uint8_t(w));
    Put32(t, 5), Put32(t, 0x40);
    t.resize(0x40, 0);
    t.resize(0x40 + size_t(w) * h * 2, 0x11);
    return t;
}

// A .zip with deflated entries (name -> data).
inline std::vector<uint8_t> MakeZip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::vector<uint8_t> zip, cd;
    auto le = [](std::vector<uint8_t>& b, uint32_t v, int n) {
        for (int i = 0; i < n; ++i) b.push_back(uint8_t(v >> (8 * i)));
    };
    for (const auto& [name, data] : files) {
        std::vector<uint8_t> z(compressBound(data.size()) + 16);
        z_stream s{};
        deflateInit2(&s, 9, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        s.next_in = (Bytef*)data.data(), s.avail_in = uInt(data.size());
        s.next_out = z.data(), s.avail_out = uInt(z.size());
        deflate(&s, Z_FINISH);
        z.resize(s.total_out);
        deflateEnd(&s);
        const uint32_t offset = uint32_t(zip.size()), crc = uint32_t(crc32(0, (const Bytef*)data.data(), uInt(data.size())));
        le(zip, 0x04034b50, 4), le(zip, 20, 2), le(zip, 0, 2), le(zip, 8, 2), le(zip, 0, 4), le(zip, crc, 4);
        le(zip, uint32_t(z.size()), 4), le(zip, uint32_t(data.size()), 4), le(zip, uint32_t(name.size()), 2), le(zip, 0, 2);
        zip.insert(zip.end(), name.begin(), name.end());
        zip.insert(zip.end(), z.begin(), z.end());
        le(cd, 0x02014b50, 4), le(cd, 20, 2), le(cd, 20, 2), le(cd, 0, 2), le(cd, 8, 2), le(cd, 0, 4), le(cd, crc, 4);
        le(cd, uint32_t(z.size()), 4), le(cd, uint32_t(data.size()), 4), le(cd, uint32_t(name.size()), 2);
        le(cd, 0, 2), le(cd, 0, 2), le(cd, 0, 2), le(cd, 0, 2), le(cd, 0, 4), le(cd, offset, 4);
        cd.insert(cd.end(), name.begin(), name.end());
    }
    const uint32_t cdOff = uint32_t(zip.size());
    zip.insert(zip.end(), cd.begin(), cd.end());
    le(zip, 0x06054b50, 4), le(zip, 0, 2), le(zip, 0, 2), le(zip, uint32_t(files.size()), 2), le(zip, uint32_t(files.size()), 2);
    le(zip, uint32_t(cd.size()), 4), le(zip, cdOff, 4), le(zip, 0, 2);
    return zip;
}

}  // namespace test
