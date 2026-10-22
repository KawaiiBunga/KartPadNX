// WBFS / raw-ISO container reader for the user-supplied Mario Kart Wii dump.
//
// The user places their own disc dump on SD (e.g. sd:/iso/RMCP01.wbfs). No
// Nintendo data ships with the port. This layer opens that container, exposes a
// *linear* disc-offset read (undoing WBFS block scattering), and validates the
// unencrypted disc header so the app can reject the wrong disc before doing any
// further work.
//
// Scope: container + header only. It does NOT decrypt Wii partitions or parse
// the FST — that needs the Wii common key (user-supplied, not shipped) and is a
// separate ingestion stage. Everything here is pure format logic with no keys,
// so it is unit-tested off-device against synthetic containers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace kartpad::disc {

enum class Format {
    Unknown,
    Iso,   // raw 1:1 disc image (disc offset == file offset)
    Wbfs,  // WBFS container (block-scattered)
};

// Parsed fields from the unencrypted 0x100-byte disc header ("boot.bin" head).
struct DiscId {
    char id[7] = {};        // 6-char game id + NUL, e.g. "RMCP01"
    std::uint8_t discNumber = 0;
    std::uint8_t version = 0;  // disc revision; 0 == rev0
    char title[65] = {};       // 64-char game title + NUL
    bool wiiMagic = false;     // 0x5D1C9EA3 present at 0x18
};

struct ValidationResult {
    bool ok = false;
    std::string message;  // human-readable, safe to show the user
};

enum class PartitionType : std::uint32_t {
    Data = 0,
    Update = 1,
    Channel = 2,
    Unknown = 0xFFFFFFFFu,
};

struct PartitionInfo {
    std::uint64_t discOffset = 0;  // linear disc offset of the partition header
    PartitionType type = PartitionType::Unknown;
    std::uint32_t rawType = 0;     // original type word (for logging unknowns)
};

// The unencrypted partition header (ticket + TMD/cert/H3/data pointers). All
// offsets/sizes are absolute LINEAR disc offsets. This is the key-free map to
// the encrypted region and to the encrypted title key; actually decrypting the
// title key (with the Wii common key) and the clusters is a later stage.
struct PartitionHeader {
    std::uint64_t partitionOffset = 0;
    std::uint8_t encryptedTitleKey[16] = {};  // ticket +0x1BF
    std::uint8_t titleKeyIv[16] = {};         // title id (ticket +0x1DC) + zero pad
    std::uint8_t commonKeyIndex = 0;          // ticket +0x1F1 (0 retail, 1 Korean)
    std::uint64_t tmdOffset = 0, tmdSize = 0;
    std::uint64_t certOffset = 0, certSize = 0;
    std::uint64_t h3Offset = 0;               // H3 table is a fixed 0x18000 bytes
    std::uint64_t dataOffset = 0, dataSize = 0;  // encrypted cluster region
};

class DiscReader {
public:
    DiscReader() = default;
    ~DiscReader() = default;
    DiscReader(const DiscReader&) = delete;
    DiscReader& operator=(const DiscReader&) = delete;

    // Open a WBFS or raw-ISO file and parse container metadata. On failure
    // returns false and sets error().
    bool open(const std::string& path);
    void close();
    bool isOpen() const { return m_file.is_open(); }

    Format format() const { return m_format; }

    // Read `len` bytes from LINEAR disc offset `off` into `dst`. WBFS block
    // scattering is undone; unallocated WBFS sectors and reads past the end of
    // the container read back as zero. Returns false only on an I/O error.
    bool read(std::uint64_t off, void* dst, std::size_t len);

    // Like read(), but rejects data not physically present in the container.
    // Extraction uses this so a truncated ISO or an unmapped/truncated WBFS
    // sector cannot decrypt as a plausible all-zero cluster.
    bool readExact(std::uint64_t off, void* dst, std::size_t len);

    // Read and parse the 0x100-byte disc header via read().
    bool readDiscId(DiscId& out);

    // Verify the dump is Mario Kart Wii PAL (RMCP01), disc revision 0.
    ValidationResult validateIsMarioKartWiiPalRev0();

    // Enumerate partitions from the (unencrypted) partition table at 0x40000.
    // This reaches the partition boundaries only; the partition *contents* are
    // AES-encrypted and need the user-supplied Wii common key to extract.
    bool listPartitions(std::vector<PartitionInfo>& out);

    // Locate the DATA partition (type 0). Returns false if none is present.
    bool findDataPartition(PartitionInfo& out);

    // Parse a partition's unencrypted header (ticket + section pointers). The
    // H3/data/cert/TMD offsets are resolved to absolute linear disc offsets.
    bool readPartitionHeader(const PartitionInfo& part, PartitionHeader& out);

    const std::string& error() const { return m_error; }

private:
    bool parseWbfs();
    bool readImpl(std::uint64_t off, void* dst, std::size_t len, bool allowSparse);
    bool readRaw(std::uint64_t fileOffset, void* dst, std::size_t len);  // no zero-fill past EOF
    void fail(const std::string& why);

    std::ifstream m_file;
    std::string m_path;
    std::string m_error;
    Format m_format = Format::Unknown;
    std::uint64_t m_fileSize = 0;

    // WBFS geometry (valid only when m_format == Wbfs).
    std::uint32_t m_hdSecSz = 0;      // host device sector size (bytes)
    std::uint32_t m_wbfsSecSz = 0;    // WBFS sector size (bytes)
    std::uint64_t m_discInfoOff = 0;  // file offset of the selected disc's info block
    std::uint32_t m_wlbaCount = 0;    // number of u16 entries in the block map
};

}  // namespace kartpad::disc
