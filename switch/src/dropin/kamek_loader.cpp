#include "dropin/kamek_loader.h"

#include <algorithm>
#include <cstring>

namespace dropin {
namespace {

constexpr uint32_t kMagic0 = 0x4B616D65u, kMagicV2 = 0x6B000002u, kMagicV3 = 0x6B000003u;
constexpr uint32_t kHeaderSize = 0x20;
constexpr uint32_t kShaAddress = 0x800017B0u;

enum Command : uint8_t {
    kAddr32 = 1, kAddr16Lo = 4, kAddr16Hi = 5, kAddr16Ha = 6, kRel24 = 10,
    kWrite32 = 32, kWrite16 = 33, kWrite8 = 34,
    kCondWritePointer = 35, kCondWrite32 = 36, kCondWrite16 = 37, kCondWrite8 = 38,
    kBranch = 64, kBranchLink = 65,
};

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

int ArgCount(uint8_t id) {
    switch (id) {
        case kAddr32: case kAddr16Lo: case kAddr16Hi: case kAddr16Ha: case kRel24:
        case kWrite32: case kWrite16: case kWrite8: case kBranch: case kBranchLink: return 1;
        case kCondWritePointer: case kCondWrite32: case kCondWrite16: case kCondWrite8: return 2;
        default: return -1;
    }
}

void Sha1(const uint8_t* data, size_t size, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    const uint64_t bits = uint64_t(size) * 8;
    const size_t total = (size + 9 + 63) / 64 * 64;
    uint8_t block[64];
    for (size_t off = 0; off < total; off += 64) {
        for (size_t i = 0; i < 64; ++i) {
            const size_t at = off + i;
            block[i] = at < size ? data[at] : at == size ? 0x80 : 0;
        }
        if (off + 64 == total) {
            for (int i = 0; i < 8; ++i) block[56 + i] = uint8_t(bits >> (56 - 8 * i));
        }
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = Be32(block + 4 * i);
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            const uint32_t f = i < 20 ? (b & c) | (~b & d) : i < 40 ? b ^ c ^ d : i < 60 ? (b & c) | (b & d) | (c & d) : b ^ c ^ d;
            const uint32_t k = i < 20 ? 0x5A827999u : i < 40 ? 0x6ED9EBA1u : i < 60 ? 0x8F1BBCDCu : 0xCA62C1D6u;
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    for (int i = 0; i < 20; ++i) out[i] = uint8_t(h[i / 4] >> (24 - 8 * (i % 4)));
}

struct Chunk {
    const uint8_t* base;  // chunk header
    uint32_t size;
};

// Pulsar's combined Code.pul: four big-endian sizes (P, E, J, K) then the chunks in that order.
bool SelectChunk(const uint8_t* data, size_t size, char region, Chunk& out, std::string& error) {
    auto isChunk = [&](size_t off) {
        return off + 8 <= size && Be32(data + off) == kMagic0 && (Be32(data + off + 4) == kMagicV2 || Be32(data + off + 4) == kMagicV3);
    };
    if (size >= 0x10 + kHeaderSize) {
        uint64_t total = 0x10;
        uint32_t sizes[4];
        for (int i = 0; i < 4; ++i) total += sizes[i] = Be32(data + 4 * i);
        if (total == size && isChunk(0x10)) {
            const char order[] = {'P', 'E', 'J', 'K'};
            size_t off = 0x10;
            for (int i = 0; i < 4; ++i) {
                if (order[i] == region) {
                    if (sizes[i] < kHeaderSize || !isChunk(off)) break;
                    out = {data + off, sizes[i]};
                    return true;
                }
                off += sizes[i];
            }
            error = std::string("Code.pul has no chunk for region ") + region;
            return false;
        }
    }
    if (isChunk(0)) {
        out = {data, static_cast<uint32_t>(size)};
        return true;
    }
    error = "not a Kamek Code.pul";
    return false;
}

struct Command_ {
    uint8_t id;
    bool absolute;
    uint32_t address;
    uint32_t arg[2];
};

}  // namespace

KamekLoadResult LoadKamek(const uint8_t* data, size_t size, GuestPointer guest, const KamekLoadOptions& opt) {
    KamekLoadResult r;
    Chunk chunk{};
    if (!SelectChunk(data, size, opt.region, chunk, r.error)) return r;
    const uint8_t* h = chunk.base;
    const uint32_t bss = Be32(h + 8), code = Be32(h + 12), ctorStart = Be32(h + 16), ctorEnd = Be32(h + 20);
    uint32_t chunkSize = Be32(h + 24);
    if (Be32(h + 4) == kMagicV2 && chunkSize == 0) chunkSize = chunk.size;  // v2 leaves it to the size table
    if (chunkSize != chunk.size || chunkSize < kHeaderSize + code || ctorStart > ctorEnd || ctorEnd > code || (ctorEnd - ctorStart) % 4) {
        r.error = "Kamek chunk header is inconsistent";
        return r;
    }

    // Parse every command before touching memory, so a bad file changes nothing.
    std::vector<Command_> commands;
    const uint8_t* p = h + kHeaderSize + code;
    const uint8_t* end = h + chunkSize;
    while (p < end) {
        if (end - p < 4) break;
        const uint32_t word = Be32(p);
        p += 4;
        Command_ c{uint8_t(word >> 24), false, word & 0x00FFFFFFu, {0, 0}};
        const int args = ArgCount(c.id);
        if (args < 0) {
            r.error = "unknown Kamek command " + std::to_string(c.id);
            return r;
        }
        if (c.address == 0x00FFFFFEu) {
            if (end - p < 4) break;
            c.absolute = true;
            c.address = Be32(p);
            p += 4;
        }
        if (end - p < 4 * args) break;
        for (int i = 0; i < args; ++i, p += 4) c.arg[i] = Be32(p);
        commands.push_back(c);
    }
    if (p != end) {
        r.error = "Kamek command stream is truncated";
        return r;
    }

    const uint32_t base = opt.guestBase;
    uint8_t* module = guest(base, code + bss);
    if (!module) {
        r.error = "module does not fit in guest memory";
        return r;
    }
    std::memcpy(module, h + kHeaderSize, code);
    std::memset(module + code, 0, bss);
    Sha1(h + kHeaderSize, code, r.codeSha1);

    auto resolve = [base](uint32_t v) { return (v & 0x80000000u) ? v : base + v; };
    auto put = [&](uint32_t address, int bytes, uint32_t value, const Command_& c) -> bool {
        uint8_t* q = guest(address, bytes);
        if (!q) {
            r.error = "Kamek command writes unmapped address " + std::to_string(address);
            return false;
        }
        for (int i = 0; i < bytes; ++i) q[i] = uint8_t(value >> (8 * (bytes - 1 - i)));
        if (c.absolute) r.writes.push_back({address, uint8_t(bytes), value, c.id});
        return true;
    };
    auto get = [&](uint32_t address, int bytes) -> uint32_t {
        const uint8_t* q = guest(address, bytes);
        uint32_t v = 0;
        for (int i = 0; q && i < bytes; ++i) v = (v << 8) | q[i];
        return v;
    };

    for (const Command_& c : commands) {
        const uint32_t address = c.absolute ? c.address : base + c.address;
        if (c.absolute && std::find(opt.skip.begin(), opt.skip.end(), address) != opt.skip.end()) {
            r.skipped.push_back(address);
            continue;
        }
        const uint32_t target = resolve(c.arg[0]);
        bool ok = true;
        switch (c.id) {
            case kAddr32: ok = put(address, 4, target, c); break;
            case kAddr16Lo: ok = put(address, 2, target & 0xFFFFu, c); break;
            case kAddr16Hi: ok = put(address, 2, target >> 16, c); break;
            case kAddr16Ha: ok = put(address, 2, (target + 0x8000u) >> 16, c); break;
            case kRel24: ok = put(address, 4, (get(address, 4) & 0xFC000003u) | ((target - address) & 0x03FFFFFCu), c); break;
            case kWrite32: ok = put(address, 4, c.arg[0], c); break;
            case kWrite16: ok = put(address, 2, c.arg[0] & 0xFFFFu, c); break;
            case kWrite8: ok = put(address, 1, c.arg[0] & 0xFFu, c); break;
            case kCondWritePointer: if (get(address, 4) == c.arg[1]) ok = put(address, 4, target, c); break;
            case kCondWrite32: if (get(address, 4) == c.arg[1]) ok = put(address, 4, c.arg[0], c); break;
            case kCondWrite16: if (get(address, 2) == (c.arg[1] & 0xFFFFu)) ok = put(address, 2, c.arg[0] & 0xFFFFu, c); break;
            case kCondWrite8: if (get(address, 1) == (c.arg[1] & 0xFFu)) ok = put(address, 1, c.arg[0] & 0xFFu, c); break;
            case kBranch:
            case kBranchLink: {
                const int32_t delta = int32_t(target - address);
                if (delta < -0x02000000 || delta > 0x01FFFFFC || (delta & 3)) {
                    r.error = "Kamek branch out of range at " + std::to_string(address);
                    return r;
                }
                ok = put(address, 4, 0x48000000u | (uint32_t(delta) & 0x03FFFFFCu) | (c.id == kBranchLink), c);
                break;
            }
        }
        if (!ok) return r;
    }

    if (uint8_t* sha = guest(kShaAddress, 20)) std::memcpy(sha, r.codeSha1, 20);
    r.ok = true;
    r.moduleStart = base;
    r.codeSize = code;
    r.bssSize = bss;
    r.ctorStart = base + ctorStart;
    r.ctorEnd = base + ctorEnd;
    return r;
}

}  // namespace dropin
