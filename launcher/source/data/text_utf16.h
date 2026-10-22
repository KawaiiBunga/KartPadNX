// Big-endian UTF-16 name fields shared by RFL_DB.dat (Miis) and rksys.dat (licenses).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mkw::text {

inline std::uint16_t Read16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }

inline void Write16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v & 0xFF);
}

inline std::uint32_t Read32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

inline void Write32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

// Reads up to `chars` UTF-16BE units (stops at a zero) as UTF-8. BMP only; surrogates become '?'.
inline std::string ReadName(const std::uint8_t* p, std::size_t chars) {
    std::string out;
    for (std::size_t i = 0; i < chars; ++i) {
        const std::uint16_t u = Read16(p + i * 2);
        if (u == 0) break;
        if (u < 0x80) {
            out.push_back(static_cast<char>(u));
        } else if (u < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (u >> 6)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            out.push_back('?');
        } else {
            out.push_back(static_cast<char>(0xE0 | (u >> 12)));
            out.push_back(static_cast<char>(0x80 | ((u >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
        }
    }
    return out;
}

// Writes UTF-8 `name` as UTF-16BE into `chars` units, zero-padded and truncated to fit.
// Returns false (and writes nothing) for an empty name or invalid UTF-8.
inline bool WriteName(std::uint8_t* p, std::size_t chars, const std::string& name) {
    std::u16string units;
    for (std::size_t i = 0; i < name.size() && units.size() < chars;) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        char16_t u;
        if (c < 0x80) {
            u = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < name.size()) {
            u = static_cast<char16_t>(((c & 0x1F) << 6) | (name[i + 1] & 0x3F));
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < name.size()) {
            u = static_cast<char16_t>(((c & 0x0F) << 12) | ((name[i + 1] & 0x3F) << 6) | (name[i + 2] & 0x3F));
            i += 3;
        } else {
            return false;  // 4-byte sequences (outside the BMP) or malformed input
        }
        units.push_back(u);
    }
    if (units.empty()) return false;
    for (std::size_t i = 0; i < chars; ++i) Write16(p + i * 2, i < units.size() ? units[i] : 0);
    return true;
}

}  // namespace mkw::text
