#include "disc_reader.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace kartpad::disc {
namespace {

constexpr std::uint32_t kWbfsMagic = 0x57424653u;  // 'W','B','F','S'
constexpr std::uint32_t kWiiMagic = 0x5D1C9EA3u;   // disc header +0x18
constexpr std::size_t kDiscHeaderSize = 0x100;     // wbfs_disc_info disc-header copy

// Single-layer Wii disc, in 0x8000-byte "wii sectors". Mario Kart Wii is
// single-layer, so this is the wlba-table sizing WBFS writers use for it.
constexpr std::uint32_t kWiiSectorsSingleLayer = 143432u;
constexpr int kWiiSectorShift = 15;  // 0x8000

std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) |
           static_cast<std::uint32_t>(p[3]);
}

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace

void DiscReader::fail(const std::string& why) {
    m_error = why;
    close();
}

void DiscReader::close() {
    if (m_file.is_open()) {
        m_file.close();
    }
    m_file.clear();
    m_format = Format::Unknown;
    m_fileSize = 0;
    m_hdSecSz = 0;
    m_wbfsSecSz = 0;
    m_discInfoOff = 0;
    m_wlbaCount = 0;
}

bool DiscReader::open(const std::string& path) {
    close();
    m_error.clear();
    m_path = path;

    m_file.open(path, std::ios::binary);
    if (!m_file.is_open()) {
        m_error = "cannot open disc image: " + path;
        return false;
    }

    m_file.seekg(0, std::ios::end);
    const std::streamoff end = m_file.tellg();
    if (end < 0) {
        fail("cannot determine size of " + path);
        return false;
    }
    m_fileSize = static_cast<std::uint64_t>(end);
    m_file.seekg(0, std::ios::beg);

    if (m_fileSize < kDiscHeaderSize) {
        fail("file is too small to be a disc image: " + path);
        return false;
    }

    std::array<std::uint8_t, 4> magic{};
    if (!readRaw(0, magic.data(), magic.size())) {
        fail("cannot read container magic from " + path);
        return false;
    }

    if (be32(magic.data()) == kWbfsMagic) {
        m_format = Format::Wbfs;
        return parseWbfs();
    }

    // Not WBFS: accept a raw ISO only if it carries the Wii disc magic. This
    // rejects CISO/NKit/GCZ and random files with a clear message instead of
    // silently treating them as a linear image.
    std::array<std::uint8_t, 4> wii{};
    if (readRaw(0x18, wii.data(), wii.size()) && be32(wii.data()) == kWiiMagic) {
        m_format = Format::Iso;
        return true;
    }

    fail("unrecognized disc image (not WBFS, not a raw Wii ISO): " + path);
    return false;
}

bool DiscReader::parseWbfs() {
    std::array<std::uint8_t, 0x0C> head{};
    if (!readRaw(0, head.data(), head.size())) {
        fail("cannot read WBFS header");
        return false;
    }

    const std::uint8_t hdSecSzS = head[0x08];
    const std::uint8_t wbfsSecSzS = head[0x09];
    if (hdSecSzS < 9 || hdSecSzS > 31 || wbfsSecSzS < 15 ||
        wbfsSecSzS < hdSecSzS || wbfsSecSzS > 31) {
        fail("WBFS header has an implausible sector geometry");
        return false;
    }
    m_hdSecSz = 1u << hdSecSzS;
    m_wbfsSecSz = 1u << wbfsSecSzS;

    // Disc table begins at 0x0C and runs to the end of the header sector; a
    // nonzero byte marks a used slot. Find the first used disc.
    const std::uint32_t maxSlots = m_hdSecSz - 0x0C;
    std::uint32_t slot = 0;
    bool found = false;
    for (std::uint32_t i = 0; i < maxSlots; ++i) {
        std::uint8_t used = 0;
        if (!readRaw(0x0C + i, &used, 1)) {
            fail("cannot read WBFS disc table");
            return false;
        }
        if (used != 0) {
            slot = i;
            found = true;
            break;
        }
    }
    if (!found) {
        fail("WBFS container holds no discs");
        return false;
    }

    // Per-disc info block: 0x100-byte header copy + a u16 block map, aligned up
    // to a host sector. Blocks are packed after the header sector.
    const std::uint32_t wbfsSecPerDisc =
        kWiiSectorsSingleLayer >> (wbfsSecSzS - kWiiSectorShift);
    m_wlbaCount = wbfsSecPerDisc;
    const std::uint64_t discInfoSz =
        alignUp(kDiscHeaderSize + static_cast<std::uint64_t>(wbfsSecPerDisc) * 2u, m_hdSecSz);
    m_discInfoOff = static_cast<std::uint64_t>(m_hdSecSz) + static_cast<std::uint64_t>(slot) * discInfoSz;

    if (m_discInfoOff + kDiscHeaderSize > m_fileSize) {
        fail("WBFS disc info block lies past the end of the file");
        return false;
    }
    return true;
}

bool DiscReader::readRaw(std::uint64_t fileOffset, void* dst, std::size_t len) {
    if (!m_file.is_open()) {
        return false;
    }
    m_file.clear();
    m_file.seekg(static_cast<std::streamoff>(fileOffset), std::ios::beg);
    if (!m_file) {
        return false;
    }
    m_file.read(static_cast<char*>(dst), static_cast<std::streamsize>(len));
    return static_cast<std::size_t>(m_file.gcount()) == len;
}

bool DiscReader::read(std::uint64_t off, void* dst, std::size_t len) {
    return readImpl(off, dst, len, true);
}

bool DiscReader::readExact(std::uint64_t off, void* dst, std::size_t len) {
    return readImpl(off, dst, len, false);
}

bool DiscReader::readImpl(std::uint64_t off, void* dst, std::size_t len, bool allowSparse) {
    auto* out = static_cast<std::uint8_t*>(dst);

    if (len > 0 && off > UINT64_MAX - len) return false;

    if (m_format == Format::Iso) {
        if (!allowSparse && (off > m_fileSize || len > m_fileSize - off)) return false;
        // Zero-fill any portion past the end of the file.
        std::size_t avail = 0;
        if (off < m_fileSize) {
            avail = static_cast<std::size_t>(
                std::min<std::uint64_t>(len, m_fileSize - off));
        }
        if (avail > 0 && !readRaw(off, out, avail)) {
            return false;
        }
        std::memset(out + avail, 0, len - avail);
        return true;
    }

    if (m_format != Format::Wbfs) {
        return false;
    }

    std::size_t done = 0;
    while (done < len) {
        const std::uint64_t cur = off + done;
        const std::uint32_t wbfsSec = static_cast<std::uint32_t>(cur / m_wbfsSecSz);
        const std::uint32_t secOff = static_cast<std::uint32_t>(cur % m_wbfsSecSz);
        const std::size_t chunk =
            std::min<std::size_t>(len - done, m_wbfsSecSz - secOff);

        std::uint16_t phys = 0;
        if (wbfsSec < m_wlbaCount) {
            std::uint8_t entry[2] = {};
            const std::uint64_t entryOff =
                m_discInfoOff + kDiscHeaderSize + static_cast<std::uint64_t>(wbfsSec) * 2u;
            if (!readRaw(entryOff, entry, 2)) {
                return false;
            }
            phys = be16(entry);
        }

        if (phys == 0) {
            if (!allowSparse) return false;
            // Sparse hole (never written) or beyond the map: reads as zero.
            std::memset(out + done, 0, chunk);
        } else {
            const std::uint64_t fileOff =
                static_cast<std::uint64_t>(phys) * m_wbfsSecSz + secOff;
            if (fileOff >= m_fileSize) {
                if (!allowSparse) return false;
                std::memset(out + done, 0, chunk);
            } else {
                const std::size_t avail = static_cast<std::size_t>(
                    std::min<std::uint64_t>(chunk, m_fileSize - fileOff));
                if (avail > 0 && !readRaw(fileOff, out + done, avail)) {
                    return false;
                }
                if (!allowSparse && avail != chunk) return false;
                std::memset(out + done + avail, 0, chunk - avail);
            }
        }
        done += chunk;
    }
    return true;
}

bool DiscReader::readDiscId(DiscId& out) {
    std::array<std::uint8_t, kDiscHeaderSize> hdr{};
    if (!read(0, hdr.data(), hdr.size())) {
        m_error = "cannot read disc header";
        return false;
    }
    out = DiscId{};
    std::memcpy(out.id, hdr.data(), 6);
    out.id[6] = '\0';
    out.discNumber = hdr[6];
    out.version = hdr[7];
    out.wiiMagic = be32(hdr.data() + 0x18) == kWiiMagic;
    std::memcpy(out.title, hdr.data() + 0x20, 64);
    out.title[64] = '\0';
    return true;
}

bool DiscReader::listPartitions(std::vector<PartitionInfo>& out) {
    out.clear();

    // Partition group table at 0x40000: 4 groups of {count, table_offset>>2}.
    std::uint8_t groups[4 * 8] = {};
    if (!read(0x40000, groups, sizeof(groups))) {
        m_error = "cannot read partition group table";
        return false;
    }

    constexpr std::uint32_t kMaxPartitions = 8;  // Wii discs have at most a few
    for (int g = 0; g < 4; ++g) {
        const std::uint32_t count = be32(groups + g * 8);
        const std::uint64_t tableOff =
            static_cast<std::uint64_t>(be32(groups + g * 8 + 4)) << 2;
        if (count == 0 || count > kMaxPartitions || tableOff == 0) {
            continue;
        }
        for (std::uint32_t i = 0; i < count && out.size() < kMaxPartitions; ++i) {
            std::uint8_t entry[8] = {};
            if (!read(tableOff + static_cast<std::uint64_t>(i) * 8, entry, sizeof(entry))) {
                m_error = "cannot read partition entry";
                return false;
            }
            PartitionInfo info;
            info.discOffset = static_cast<std::uint64_t>(be32(entry)) << 2;
            info.rawType = be32(entry + 4);
            switch (info.rawType) {
                case 0: info.type = PartitionType::Data; break;
                case 1: info.type = PartitionType::Update; break;
                case 2: info.type = PartitionType::Channel; break;
                default: info.type = PartitionType::Unknown; break;
            }
            out.push_back(info);
        }
    }
    return true;
}

bool DiscReader::findDataPartition(PartitionInfo& out) {
    std::vector<PartitionInfo> parts;
    if (!listPartitions(parts)) {
        return false;
    }
    for (const auto& p : parts) {
        if (p.type == PartitionType::Data) {
            out = p;
            return true;
        }
    }
    m_error = "no DATA partition found in the disc";
    return false;
}

bool DiscReader::readPartitionHeader(const PartitionInfo& part, PartitionHeader& out) {
    // Ticket (0x2A4) + the section-pointer block that follows it. 0x2C0 bytes
    // covers the ticket and the tmd/cert/h3/data offset+size words at 0x2A4..0x2BF.
    std::uint8_t hdr[0x2C0] = {};
    if (!read(part.discOffset, hdr, sizeof(hdr))) {
        m_error = "cannot read partition header";
        return false;
    }

    out = PartitionHeader{};
    out.partitionOffset = part.discOffset;
    std::memcpy(out.encryptedTitleKey, hdr + 0x1BF, 16);
    // Title-key IV = 8-byte title id (ticket +0x1DC) followed by zero padding.
    std::memcpy(out.titleKeyIv, hdr + 0x1DC, 8);
    out.commonKeyIndex = hdr[0x1F1];  // ticket common_key_index

    // TMD/cert sizes are byte counts; every *offset* (and data_size) is stored
    // >>2, relative to the partition start.
    out.tmdSize = be32(hdr + 0x2A4);
    out.tmdOffset = part.discOffset + (static_cast<std::uint64_t>(be32(hdr + 0x2A8)) << 2);
    out.certSize = be32(hdr + 0x2AC);
    out.certOffset = part.discOffset + (static_cast<std::uint64_t>(be32(hdr + 0x2B0)) << 2);
    out.h3Offset = part.discOffset + (static_cast<std::uint64_t>(be32(hdr + 0x2B4)) << 2);
    out.dataOffset = part.discOffset + (static_cast<std::uint64_t>(be32(hdr + 0x2B8)) << 2);
    out.dataSize = static_cast<std::uint64_t>(be32(hdr + 0x2BC)) << 2;
    return true;
}

ValidationResult DiscReader::validateIsMarioKartWiiPalRev0() {
    DiscId id{};
    if (!readDiscId(id)) {
        return {false, m_error};
    }
    if (!id.wiiMagic) {
        return {false, "not a Wii disc (missing 0x5D1C9EA3 magic)"};
    }
    if (std::strcmp(id.id, "RMCP01") != 0) {
        return {false, std::string("wrong game: found id '") + id.id +
                           "', expected 'RMCP01' (Mario Kart Wii, PAL)"};
    }
    if (id.version != 0) {
        return {false, "wrong disc revision: found rev" +
                           std::to_string(id.version) + ", expected rev0"};
    }
    return {true, "Mario Kart Wii (PAL, RMCP01, rev0) verified"};
}

}  // namespace kartpad::disc
