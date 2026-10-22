#include "disc_extractor.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#if defined(__SWITCH__)
#include <switch.h>
#endif

#include "disc_reader.h"
#include "wii_cluster.h"
#include "wii_common_key.h"
#include "wii_crypto.h"
#include "wii_fst.h"

namespace kartpad::disc {
namespace {

std::uint32_t readBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// Offsets inside the decrypted partition's boot.bin (logical offset 0). On Wii
// these disc offsets are stored >>2.
constexpr std::uint64_t kBootBinSize = 0x440;
constexpr std::uint64_t kBi2Size = 0x2000;          // logical [0x440, 0x2440)
constexpr std::uint64_t kApploaderOffset = 0x2440;  // logical
constexpr std::size_t kBootDolOffset = 0x420;
constexpr std::size_t kBootFstOffset = 0x424;
constexpr std::size_t kBootFstSize = 0x428;
constexpr char kCompletionManifest[] = ".kartpad-extraction-complete";
constexpr std::uint64_t kMaxFstSize = 64ull << 20;
constexpr std::uint64_t kMaxSystemFileSize = 16ull << 20;

std::string parentDir(const std::string& path);
bool report(const ExtractOptions& opts, const ExtractProgress& p);

bool rangeInside(std::uint64_t offset, std::uint64_t size, std::uint64_t total) {
    return offset <= total && size <= total - offset;
}

// FNV-1a is an accidental-corruption check, not an authenticity guarantee.
std::uint64_t fnv1a(const std::uint8_t* data, std::size_t size,
                    std::uint64_t hash = 14695981039346656037ull) {
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

// Reads decrypted *logical* partition bytes on demand, caching one cluster.
class PartitionDataSource {
public:
    PartitionDataSource(DiscReader& disc, const PartitionHeader& hdr, AesCbcDecryptFn aes,
                        const std::uint8_t titleKey[16])
        : m_disc(disc), m_hdr(hdr), m_aes(aes), m_cluster(kWiiClusterSize) {
        std::memcpy(m_titleKey, titleKey, 16);
    }

    // Read `len` decrypted logical bytes at logical `off` into `dst`.
    bool read(std::uint64_t off, void* dst, std::size_t len) {
        const std::uint64_t logicalSize =
            (m_hdr.dataSize / kWiiClusterSize) * kWiiClusterDataSize;
        if (!rangeInside(off, len, logicalSize)) {
            m_error = "read exceeds declared DATA partition";
            return false;
        }
        auto* out = static_cast<std::uint8_t*>(dst);
        for (const ClusterRun& run : wiiClusterRuns(off, len)) {
            if (!loadCluster(run.clusterIndex, run.encClusterOffset)) {
                return false;
            }
            const std::uint8_t* payload = m_cluster.data() + kWiiClusterHeaderSize;
            std::memcpy(out + (run.logicalOffset - off), payload + run.payloadOffset,
                        static_cast<std::size_t>(run.length));
        }
        return true;
    }

    const std::string& error() const { return m_error; }

private:
    bool loadCluster(std::uint64_t clusterIndex, std::uint64_t encClusterOffset) {
        if (m_cachedIndex == static_cast<std::int64_t>(clusterIndex)) {
            return true;
        }
        const std::uint64_t physOff = m_hdr.dataOffset + encClusterOffset;
        if (!rangeInside(encClusterOffset, kWiiClusterSize, m_hdr.dataSize)) {
            m_error = "cluster exceeds declared DATA partition";
            return false;
        }
        if (!m_disc.readExact(physOff, m_cluster.data(), kWiiClusterSize)) {
            m_error = "disc read failed at cluster " + std::to_string(clusterIndex);
            m_cachedIndex = -1;
            return false;
        }
        // Decrypts the 0x7C00 payload in place using the IV at cluster+0x3D0.
        decryptClusterData(m_aes, m_titleKey, m_cluster.data());
        m_cachedIndex = static_cast<std::int64_t>(clusterIndex);
        return true;
    }

    DiscReader& m_disc;
    PartitionHeader m_hdr;
    AesCbcDecryptFn m_aes;
    std::uint8_t m_titleKey[16] = {};
    std::vector<std::uint8_t> m_cluster;
    std::int64_t m_cachedIndex = -1;
    std::string m_error;
};

bool writeAll(int fd, const std::uint8_t* data, std::size_t len) {
    while (len != 0) {
        const ssize_t n = write(fd, data, len);
        if (n <= 0) return false;
        data += n;
        len -= static_cast<std::size_t>(n);
    }
    return true;
}

bool fsyncParent(const std::string& path) {
#if defined(__SWITCH__)
    // libnx's SD devoptab exposes files, not directory descriptors. fsync() of
    // the staged file occurs before rename; FAT metadata persistence is left to
    // Horizon's SD filesystem and cannot be asserted more strongly here.
    if (path.compare(0, 6, "sdmc:/") == 0)
        (void)fsdevCommitDevice("sdmc");
    return true;
#else
    const std::string parent = parentDir(path);
    const int fd = open((parent.empty() ? "." : parent.c_str()), O_RDONLY);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
#endif
}

// Publish a completed staged file. The staging directory is outside the game
// tree; failures leave it unusable and the completion marker absent.
template <typename Writer>
bool writeStaged(const std::string& path, const std::string& stagingDir, Writer writer) {
    // Keep staging outside files/ and sys/: a .part name must never collide with
    // a legitimate FST entry or be visible to the DVD HLE.
    const std::string temp = stagingDir + "/output.part";
    unlink(temp.c_str());
    const int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    const bool wrote = writer(fd);
    const bool synced = wrote && fsync(fd) == 0;
    const bool closed = close(fd) == 0;
    if (!synced || !closed) return false;
#if defined(__SWITCH__)
    // libnx maps rename directly to fsFsRenameFile, which does not replace an
    // existing destination. The completion marker was removed before any write,
    // so a power loss between these operations remains a safe retry state.
    unlink(path.c_str());
#endif
    if (rename(temp.c_str(), path.c_str()) != 0) return false;
    return fsyncParent(path);
}

bool writeFileBytes(const std::string& path, const std::string& stagingDir,
                    const std::uint8_t* data, std::size_t len) {
    return writeStaged(path, stagingDir, [=](int fd) { return writeAll(fd, data, len); });
}

// Stream a logical range in chunks, retaining only one decrypted cluster in
// memory. Reporting after every chunk lets the NRO poll cancellation while a
// large archive is being written.
bool writeLogicalRange(PartitionDataSource& src, std::uint64_t logicalOffset, std::uint64_t size,
                       const std::string& path, const std::string& stagingDir,
                       std::uint64_t& bytesOut,
                       std::uint64_t& contentHash, const ExtractOptions& opts,
                       ExtractProgress* progress, bool& aborted) {
    aborted = false;
    contentHash = 14695981039346656037ull;
    return writeStaged(path, stagingDir, [&](int fd) {
        std::vector<std::uint8_t> buf(kWiiClusterDataSize);
        std::uint64_t remaining = size;
        std::uint64_t cur = logicalOffset;
        while (remaining != 0) {
            const std::size_t chunk =
                static_cast<std::size_t>(remaining < buf.size() ? remaining : buf.size());
            if (!src.read(cur, buf.data(), chunk)) return false;
            if (!writeAll(fd, buf.data(), chunk)) return false;
            contentHash = fnv1a(buf.data(), chunk, contentHash);
            cur += chunk;
            remaining -= chunk;
            bytesOut += chunk;
            if (progress != nullptr) {
                progress->bytesDone = bytesOut;
                if (!report(opts, *progress)) {
                    aborted = true;
                    return false;
                }
            }
        }
        return true;
    });
}

struct ManifestEntry { std::uint64_t size, hash; };

bool writeManifest(const std::string& path, const std::string& stagingDir, std::uint64_t fstHash,
                   const std::vector<ManifestEntry>& entries) {
    std::ostringstream out;
    out << "KARTPAD-DVD-EXTRACT-V1\n" << "fst " << std::hex << fstHash << "\n";
    for (const ManifestEntry& e : entries)
        out << "file " << std::dec << e.size << " " << std::hex << e.hash << "\n";
    const std::string bytes = out.str();
    return writeFileBytes(path, stagingDir,
                          reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
}

std::string parentDir(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// DOL file size = the largest (section file offset + section size) across the 7
// text and 11 data sections. Header is 0x100 bytes.
std::uint64_t dolFileSize(const std::uint8_t hdr[0x100]) {
    std::uint64_t end = 0x100;
    for (int i = 0; i < 7; ++i) {
        const std::uint64_t off = readBe32(hdr + 0x00 + i * 4);
        const std::uint64_t sz = readBe32(hdr + 0x90 + i * 4);
        if (off + sz > end) end = off + sz;
    }
    for (int i = 0; i < 11; ++i) {
        const std::uint64_t off = readBe32(hdr + 0x1C + i * 4);
        const std::uint64_t sz = readBe32(hdr + 0xAC + i * 4);
        if (off + sz > end) end = off + sz;
    }
    return end;
}

bool report(const ExtractOptions& opts, const ExtractProgress& p) {
    if (opts.progress) {
        return opts.progress(p, opts.progressUser);
    }
    return true;
}

ExtractResult fail(std::string msg) {
    ExtractResult r;
    r.ok = false;
    r.message = std::move(msg);
    return r;
}

}  // namespace

bool makeDirs(const std::string& path) {
    if (path.empty()) {
        return true;
    }
    std::string acc;
    std::size_t i = 0;
    // libnx paths have a device prefix (for example sdmc:/...). It is not a
    // host directory and mkdir("sdmc:") fails on Horizon.
    const std::size_t colon = path.find(':');
    if (colon != std::string::npos && colon + 1 < path.size() && path[colon + 1] == '/') {
        acc = path.substr(0, colon + 2);
        i = colon + 2;
    }
    // Preserve a leading '/'.
    if (i == 0 && path[0] == '/') {
        acc = "/";
        i = 1;
    }
    while (i <= path.size()) {
        if (i == path.size() || path[i] == '/') {
            if (!acc.empty() && acc != "/") {
                if (mkdir(acc.c_str(), 0777) != 0 && errno != EEXIST) {
                    return false;
                }
            }
            if (i < path.size()) {
                acc += '/';
            }
        } else {
            acc += path[i];
        }
        ++i;
    }
    return true;
}

ExtractResult extractDataPartition(const std::string& discPath, const std::string& dvdRootOut,
                                   const ExtractOptions& opts, AesCbcDecryptFn aes) {
    if (aes == nullptr) {
        return fail("No AES primitive supplied.");
    }

    DiscReader disc;
    if (!disc.open(discPath)) {
        return fail("Cannot open dump: " + disc.error());
    }

    ValidationResult v = disc.validateIsMarioKartWiiPalRev0();
    if (!v.ok) {
        return fail("Not a supported dump: " + v.message);
    }

    PartitionInfo dataPart;
    if (!disc.findDataPartition(dataPart)) {
        return fail("No DATA partition found in the dump.");
    }

    PartitionHeader hdr;
    if (!disc.readPartitionHeader(dataPart, hdr)) {
        return fail("Failed to read the DATA partition header: " + disc.error());
    }

    // No user key needed: use the embedded common key selected by the ticket,
    // unless the caller passed an explicit override.
    const std::uint8_t* commonKey =
        opts.commonKey ? opts.commonKey : wiiCommonKey(hdr.commonKeyIndex);
    if (commonKey == nullptr) {
        return fail("Unsupported disc common-key index " +
                    std::to_string(hdr.commonKeyIndex) + ".");
    }

    std::uint8_t titleKey[16];
    unwrapTitleKey(aes, commonKey, hdr.encryptedTitleKey, hdr.titleKeyIv, titleKey);

    PartitionDataSource src(disc, hdr, aes, titleKey);

    // boot.bin gives the DOL and FST locations (Wii stores them >>2).
    std::uint8_t bootBin[kBootBinSize];
    if (!src.read(0, bootBin, sizeof(bootBin))) {
        return fail("Failed to read partition boot.bin: " + src.error());
    }
    const std::uint64_t dolOffset = static_cast<std::uint64_t>(readBe32(bootBin + kBootDolOffset)) << 2;
    const std::uint64_t fstOffset = static_cast<std::uint64_t>(readBe32(bootBin + kBootFstOffset)) << 2;
    const std::uint64_t fstSize = static_cast<std::uint64_t>(readBe32(bootBin + kBootFstSize)) << 2;

    if (hdr.dataSize == 0 || hdr.dataSize % kWiiClusterSize != 0 || fstOffset == 0 ||
        fstSize < 12 || fstSize > kMaxFstSize) {
        return fail("Partition FST location looks invalid (offset=" + std::to_string(fstOffset) +
                    " size=" + std::to_string(fstSize) + ").");
    }

    ExtractResult r;
    r.fstSize = fstSize;

    // Read and validate the FST before creating any game-visible output.
    std::vector<std::uint8_t> fst(static_cast<std::size_t>(fstSize));
    {
        ExtractProgress p;
        p.stage = "reading FST";
        if (!report(opts, p)) return fail("Aborted.");
    }
    if (!src.read(fstOffset, fst.data(), fst.size())) {
        return fail("Failed to read the FST: " + src.error());
    }
    std::vector<FstNode> nodes;
    std::string fstErr;
    if (!parseWiiFst(fst.data(), fst.size(), nodes, fstErr)) {
        return fail("FST parse failed: " + fstErr);
    }

    const std::string sysDir = dvdRootOut + "/sys";
    const std::string filesDir = dvdRootOut + "/files";
    const std::string stagingDir = dvdRootOut + "/.kartpad-staging";
    const std::string manifestPath = dvdRootOut + "/" + kCompletionManifest;
    if (!makeDirs(sysDir) || !makeDirs(filesDir) || !makeDirs(stagingDir))
        return fail("Cannot create output directories under " + dvdRootOut + ".");
    // A former successful run stops being a valid readiness signal as soon as
    // this run begins changing its tree. A failed rerun therefore never leaves
    // a stale completion marker behind.
    if (unlink(manifestPath.c_str()) != 0 && errno != ENOENT)
        return fail("Cannot invalidate the previous completion manifest.");
    if (!writeFileBytes(sysDir + "/fst.bin", stagingDir, fst.data(), fst.size()))
        return fail("Failed to write sys/fst.bin (SD full or read-only?).");

    // Tally total file work for progress.
    std::uint64_t filesTotal = 0, bytesTotal = 0;
    for (const FstNode& n : nodes) {
        if (!n.isDir) {
            ++filesTotal;
            bytesTotal += n.size;
        }
    }
    const std::uint64_t fstHash = fnv1a(fst.data(), fst.size());
    (void)opts.skipExisting;
    std::vector<ManifestEntry> completedManifest;
    completedManifest.reserve(static_cast<std::size_t>(filesTotal));

    // sys/ extras: boot.bin, bi2.bin, apploader.img, main.dol.
    if (opts.writeSysExtras) {
        if (!writeFileBytes(sysDir + "/boot.bin", stagingDir, bootBin, sizeof(bootBin))) {
            return fail("Failed to write sys/boot.bin.");
        }

        std::vector<std::uint8_t> bi2(static_cast<std::size_t>(kBi2Size));
        if (!src.read(kBootBinSize, bi2.data(), bi2.size())) {
            return fail("Failed to read bi2.bin: " + src.error());
        }
        if (!writeFileBytes(sysDir + "/bi2.bin", stagingDir, bi2.data(), bi2.size())) {
            return fail("Failed to write sys/bi2.bin.");
        }

        // Apploader: 0x20 header (size @0x14, trailer @0x18), then body+trailer.
        std::uint8_t appHdr[0x20];
        if (!src.read(kApploaderOffset, appHdr, sizeof(appHdr))) {
            return fail("Failed to read apploader header: " + src.error());
        }
        const std::uint64_t appBody = readBe32(appHdr + 0x14);
        const std::uint64_t appTrailer = readBe32(appHdr + 0x18);
        const std::uint64_t appTotal = 0x20 + appBody + appTrailer;
        if (appTotal < 0x20 || appTotal > kMaxSystemFileSize) {
            return fail("Apploader size looks invalid.");
        }
        {
            std::uint64_t appBytes = 0;
            std::uint64_t appHash = 0;
            bool aborted = false;
            if (!writeLogicalRange(src, kApploaderOffset, appTotal, sysDir + "/apploader.img",
                                   stagingDir, appBytes, appHash, opts, nullptr, aborted)) {
                return fail(aborted ? "Aborted by user." : "Failed to write sys/apploader.img.");
            }
        }

        // main.dol (size from its header).
        if (dolOffset == 0) return fail("Partition has no main.dol offset.");
        std::uint8_t dolHdr[0x100];
        if (!src.read(dolOffset, dolHdr, sizeof(dolHdr)))
            return fail("Failed to read DOL header: " + src.error());
        const std::uint64_t dolSize = dolFileSize(dolHdr);
        r.dolSize = dolSize;
        if (dolSize < 0x100 || dolSize > kMaxSystemFileSize)
            return fail("main.dol size looks invalid.");
        std::uint64_t dolBytes = 0, dolHash = 0;
        bool aborted = false;
        if (!writeLogicalRange(src, dolOffset, dolSize, sysDir + "/main.dol", stagingDir,
                               dolBytes, dolHash, opts, nullptr, aborted)) {
            return fail(aborted ? "Aborted by user." : "Failed to write sys/main.dol.");
        }
    }

    // files/ tree. The completion manifest is written only after every file
    // has been atomically published. The manifest itself is the only readiness
    // signal; a lost/partial manifest means the next run re-extracts.
    for (const FstNode& n : nodes) {
        const std::string rel = n.path.empty() || n.path[0] != '/' ? n.path : n.path.substr(1);
        const std::string outPath = filesDir + "/" + rel;
        if (n.isDir) {
            if (!makeDirs(outPath)) {
                return fail("Cannot create directory " + outPath + ".");
            }
            continue;
        }
        const std::string parent = parentDir(outPath);
        if (!parent.empty() && !makeDirs(parent)) {
            return fail("Cannot create directory " + parent + ".");
        }

        std::uint64_t actualHash = 0;
        ExtractProgress p;
        p.stage = "extracting files";
        p.filesDone = r.filesExtracted;
        p.filesTotal = filesTotal;
        p.bytesDone = r.bytesExtracted;
        p.bytesTotal = bytesTotal;
        bool aborted = false;
        if (!writeLogicalRange(src, n.offset, n.size, outPath, stagingDir, r.bytesExtracted,
                               actualHash, opts, &p, aborted)) {
            return fail(aborted ? "Aborted by user." :
                        "Failed to write " + outPath + ": " + src.error());
        }
        completedManifest.push_back({n.size, actualHash});
        ++r.filesExtracted;

        ExtractProgress completeProgress;
        completeProgress.stage = "extracting files";
        completeProgress.filesDone = r.filesExtracted;
        completeProgress.filesTotal = filesTotal;
        completeProgress.bytesDone = r.bytesExtracted;
        completeProgress.bytesTotal = bytesTotal;
        if (!report(opts, completeProgress)) {
            return fail("Aborted by user.");
        }
    }

    if (!writeManifest(manifestPath, stagingDir, fstHash, completedManifest))
        return fail("Failed to commit extraction completion manifest.");

    r.ok = true;
    r.message = "Extraction complete.";
    return r;
}

}  // namespace kartpad::disc
