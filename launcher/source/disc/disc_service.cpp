#include "mkw/services.h"
#include "data/fsutil.h"
#include "data/game_paths.h"
#include "../../../switch/src/runtime/disc_extractor.h"
#include "../../../switch/src/runtime/wii_crypto.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#ifdef __SWITCH__
extern "C" {
#include <switch.h>
}
#endif

// Forward declaration of the platform AES decryptor from switch/src/runtime/wii_crypto_switch.cpp
namespace kartpad::disc {
void platformAesCbcDecrypt(const uint8_t key[16], const uint8_t iv[16], uint8_t* data, size_t size);
}

namespace {
// SHA-256 of the PAL RMCP01 rev0 files (see docs/LAUNCHER-PLAN.md section 4.1).
constexpr const char* kMainDolSha256 = "80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05";
constexpr const char* kStaticRelSha256 = "16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d";
}  // namespace

namespace mkw {
#ifdef _WIN32
const std::string ROOT_DIR = ".";
#else
const std::string ROOT_DIR = "sdmc:";
#endif
namespace fs = std::filesystem;

class DiscServiceImpl : public DiscService {
public:
    DiscServiceImpl() {
        m_status.state = JobStatus::State::Idle;
        RecoverInterruptedSwap(GameDataDir());
        RecoverInterruptedSwap(RetroRewindDir());
    }
    
    ~DiscServiceImpl() override {
        Cancel();
        if (m_worker.joinable()) m_worker.join();
    }

    // Data extracted before the launcher existed has no marker: main.dol + StaticR.rel are only
    // ~7 MB, so verify them in place once instead of asking for a fresh import.
    bool HasValidGameData() override {
        if (VerifiedMarkerMatches()) return true;
        return ValidateGameTree(GameDataDir()).empty() && WriteVerifiedMarker();
    }

    std::vector<DiscCandidate> ScanForSources() override {
        std::vector<DiscCandidate> candidates;
        std::vector<std::string> searchPaths = {
            ROOT_DIR + "/iso", ROOT_DIR + "/wbfs", ROOT_DIR + "/", ROOT_DIR + "/switch/kartpad/import"
        };
        
        for (const auto& path : searchPaths) {
            if (!fs::exists(path)) continue;
            for (const auto& entry : fs::directory_iterator(path)) {
                if (entry.is_regular_file()) {
                    auto ext = entry.path().extension().string();
                    if (ext == ".wbfs" || ext == ".iso") {
                        candidates.push_back({entry.path().string(), false, entry.file_size()});
                    }
                } else if (entry.is_directory()) {
                    // Check if Dolphin extracted tree
                    if (fs::exists(entry.path() / "sys/main.dol") && fs::exists(entry.path() / "files")) {
                        candidates.push_back({entry.path().string(), true, 0});
                    } else if (fs::exists(entry.path() / "DATA/sys/main.dol") && fs::exists(entry.path() / "DATA/files")) {
                        candidates.push_back({entry.path().string(), true, 0});
                    }
                }
            }
        }
        return candidates;
    }

    bool StartImport(const DiscCandidate& source) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_status.state == JobStatus::State::Running) return false;
        
        if (m_worker.joinable()) m_worker.join();
        m_cancelFlag = false;
        m_status.state = JobStatus::State::Running;
        m_status.fraction = 0.0f;
        m_status.stage = "Starting import...";
        m_status.message = "";
        
        m_worker = std::thread(&DiscServiceImpl::BackgroundImport, this, source);
        return true;
    }

    bool StartVerify() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_status.state == JobStatus::State::Running) return false;
        
        if (m_worker.joinable()) m_worker.join();
        m_cancelFlag = false;
        m_status.state = JobStatus::State::Running;
        m_status.fraction = -1.0f;
        m_status.stage = "Verifying data...";
        m_status.message = "";
        
        m_worker = std::thread(&DiscServiceImpl::BackgroundVerify, this);
        return true;
    }

    JobStatus Poll() override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_status;
    }

    void Cancel() override {
        m_cancelFlag = true;
    }

    bool HasValidRetroRewind(std::string* versionOut) override {
        return ValidateRetroRewindPack(RetroRewindDir(), versionOut).empty();
    }

    bool StartRetroRewindImport(const std::string& folderPath) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_status.state == JobStatus::State::Running) return false;
        
        if (m_worker.joinable()) m_worker.join();
        m_cancelFlag = false;
        m_status.state = JobStatus::State::Running;
        m_status.fraction = -1.0f;
        m_status.stage = "Importing Retro Rewind...";
        m_status.message = "";
        
        m_worker = std::thread(&DiscServiceImpl::BackgroundRRImport, this, folderPath);
        return true;
    }

private:
    std::mutex m_mutex;
    JobStatus m_status;
    std::thread m_worker;
    std::atomic<bool> m_cancelFlag;

    static bool ProgressCallback(const kartpad::disc::ExtractProgress& p, void* user) {
        DiscServiceImpl* self = static_cast<DiscServiceImpl*>(user);
        if (self->m_cancelFlag) return false;
        
        std::lock_guard<std::mutex> lock(self->m_mutex);
        self->m_status.stage = p.stage;
        if (p.filesTotal > 0) {
            self->m_status.fraction = static_cast<float>(p.filesDone) / p.filesTotal;
        } else if (p.bytesTotal > 0) {
            self->m_status.fraction = static_cast<float>(p.bytesDone) / p.bytesTotal;
        }
        return true;
    }

    static std::string GameDataDir() { return paths::DvdRoot(); }
    static std::string RetroRewindDir() { return paths::RetroRewindRoot() + "/RetroRewind6"; }
    static std::string VerifiedMarker() { return ROOT_DIR + "/switch/mkw-launcher/data-verified.json"; }

    void SetStage(const char* stage, float fraction) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.stage = stage;
        m_status.fraction = fraction;
    }

    void Finish(JobStatus::State state, const std::string& message) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.state = state;
        m_status.message = message;
        if (state == JobStatus::State::Succeeded) m_status.fraction = 1.0f;
    }

    static bool HashFile(const std::string& path, std::string& outHex) {
#ifdef __SWITCH__
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file) return false;
        Sha256Context ctx;
        sha256ContextCreate(&ctx);
        std::vector<char> buffer(1 << 20);
        size_t n;
        while ((n = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) {
            sha256ContextUpdate(&ctx, buffer.data(), n);
        }
        const bool readOk = std::ferror(file) == 0;
        std::fclose(file);
        if (!readOk) return false;
        uint8_t hash[SHA256_HASH_SIZE];
        sha256ContextGetHash(&ctx, hash);
        char hex[SHA256_HASH_SIZE * 2 + 1];
        for (int i = 0; i < SHA256_HASH_SIZE; ++i) {
            std::snprintf(hex + i * 2, 3, "%02x", hash[i]);
        }
        outHex = hex;
        return true;
#else
        // Host tests: the fixture stores the expected digest in "<file>.sha256".
        if (!fs::exists(path)) return false;
        std::ifstream digest(path + ".sha256");
        return static_cast<bool>(digest >> outHex);
#endif
    }

    // Empty string when `root` holds a PAL RMCP01 rev0 tree; otherwise a displayable error.
    static std::string ValidateGameTree(const std::string& root) {
        const std::string dol = root + "/sys/main.dol";
        const std::string rel = root + "/files/rel/StaticR.rel";
        if (!fs::exists(dol) || !fs::exists(rel)) return "Missing sys/main.dol or files/rel/StaticR.rel.";
        std::string dolHash, relHash;
        if (!HashFile(dol, dolHash) || !HashFile(rel, relHash)) return "Could not read the game files.";
        if (dolHash != kMainDolSha256 || relHash != kStaticRelSha256) {
            return "This is not a Mario Kart Wii PAL (RMCP01) revision 0 dump.";
        }
        return {};
    }

    struct FileStamp {
        std::uint64_t size = 0;
        long long mtime = 0;
    };

    static bool Stamp(const std::string& path, FileStamp& out) {
        std::error_code ec;
        out.size = fs::file_size(path, ec);
        if (ec) return false;
        out.mtime = static_cast<long long>(fs::last_write_time(path, ec).time_since_epoch().count());
        return !ec;
    }

    static bool WriteVerifiedMarker() {
        FileStamp dol, rel;
        if (!Stamp(GameDataDir() + "/sys/main.dol", dol) || !Stamp(GameDataDir() + "/files/rel/StaticR.rel", rel)) {
            return false;
        }
        nlohmann::json j;
        j["schema"] = 1;
        j["dol_size"] = dol.size;
        j["dol_mtime"] = dol.mtime;
        j["rel_size"] = rel.size;
        j["rel_mtime"] = rel.mtime;
        std::error_code ec;
        ec = mkw::fsx::MakeDirs(fs::path(VerifiedMarker()).parent_path());
        std::ofstream out(VerifiedMarker(), std::ios::trunc);
        out << j.dump(2);
        out.close();
        return static_cast<bool>(out);
    }

    // The marker is only trusted while both files still have the size and mtime it recorded.
    static bool VerifiedMarkerMatches() {
        std::ifstream in(VerifiedMarker());
        if (!in) return false;
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_discarded() || j.value("schema", 0) != 1) return false;
        FileStamp dol, rel;
        if (!Stamp(GameDataDir() + "/sys/main.dol", dol) || !Stamp(GameDataDir() + "/files/rel/StaticR.rel", rel)) {
            return false;
        }
        return j.value("dol_size", std::uint64_t{0}) == dol.size && j.value("dol_mtime", 0LL) == dol.mtime &&
               j.value("rel_size", std::uint64_t{0}) == rel.size && j.value("rel_mtime", 0LL) == rel.mtime;
    }

    // Replaces `live` with `part`. The old tree is kept as `live.old` until the new one is in place.
    static bool SwapIntoPlace(const std::string& part, const std::string& live, std::string& err) {
        std::error_code ec;
        const std::string old = live + ".old";
        fs::remove_all(old, ec);
        const bool hadLive = fs::exists(live);
        if (hadLive) {
            fs::rename(live, old, ec);
            if (ec) {
                err = "Could not move the existing folder aside: " + ec.message();
                return false;
            }
        }
        fs::rename(part, live, ec);
        if (ec) {
            err = "Could not move the new folder into place: " + ec.message();
            if (hadLive) {
                std::error_code rollback;
                fs::rename(old, live, rollback);
            }
            return false;
        }
        fs::remove_all(old, ec);
        return true;
    }

    bool CopyFolderWithProgress(const std::string& srcPath, const std::string& dstPath,
                                std::uint64_t& copiedBytes, std::uint64_t totalBytes) {
        std::error_code ec;
        ec = mkw::fsx::MakeDirs(dstPath);
        if (ec) return false;
        for (auto it = fs::recursive_directory_iterator(srcPath, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (m_cancelFlag) return false;
            const auto destPath = fs::path(dstPath) / fs::relative(it->path(), srcPath, ec);
            if (ec) return false;
            if (it->is_directory(ec)) {
                ec = mkw::fsx::MakeDirs(destPath);
            } else if (it->is_regular_file(ec)) {
                ec = mkw::fsx::CopyFile(it->path(), destPath);
                if (ec) return false;
                copiedBytes += it->file_size(ec);
                SetStage("Copying files", totalBytes ? static_cast<float>(copiedBytes) / totalBytes : -1.0f);
            }
            if (ec) return false;
        }
        return !ec;
    }

    static std::uint64_t TreeBytes(const std::string& path) {
        std::uint64_t total = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(path, ec); !ec && it != fs::recursive_directory_iterator();
             it.increment(ec)) {
            if (it->is_regular_file(ec)) total += it->file_size(ec);
        }
        return total;
    }

    // Folders moved (not copied) out of the user's source. Undo() puts them back, so a failed or
    // cancelled import never deletes user files when the staging folder is cleaned up.
    struct SourceMoves {
        std::vector<std::pair<fs::path, fs::path>> done;  // {from, to}

        bool Move(const fs::path& from, const fs::path& to) {
            std::error_code ec;
            fs::rename(from, to, ec);
            if (ec) return false;
            done.emplace_back(from, to);
            return true;
        }

        void Undo() {
            for (auto it = done.rbegin(); it != done.rend(); ++it) {
                std::error_code ec;
                fs::rename(it->second, it->first, ec);
            }
            done.clear();
        }
    };

    // Drops a staging folder after first returning anything that was moved out of the source.
    static void Discard(const std::string& part, SourceMoves& moves) {
        moves.Undo();
        std::error_code ec;
        fs::remove_all(part, ec);
    }

    // A crash between the two renames in SwapIntoPlace leaves `live.old` and no `live`.
    static void RecoverInterruptedSwap(const std::string& live) {
        std::error_code ec;
        const std::string old = live + ".old";
        if (!fs::exists(live, ec) && fs::exists(old, ec)) {
            fs::rename(old, live, ec);
        }
    }

    // Validates `part`, swaps it over the live data, and records the verified marker.
    void FinishGameImport(const std::string& part, const std::string& doneMessage, SourceMoves& moves) {
        SetStage("Verifying game files", -1.0f);
        std::error_code ec;
        const std::string invalid = ValidateGameTree(part);
        if (!invalid.empty()) {
            Discard(part, moves);
            Finish(JobStatus::State::Failed, invalid);
            return;
        }
        fs::remove(VerifiedMarker(), ec);
        std::string err;
        if (!SwapIntoPlace(part, GameDataDir(), err)) {
            Discard(part, moves);
            Finish(JobStatus::State::Failed, err);
            return;
        }
        if (!WriteVerifiedMarker()) {
            Finish(JobStatus::State::Failed, "Imported, but could not record the verification result.");
            return;
        }
        Finish(JobStatus::State::Succeeded, doneMessage);
    }

    void BackgroundImport(DiscCandidate source) {
        std::error_code ec;
        const std::string part = GameDataDir() + ".part";
        fs::remove_all(part, ec);
        SourceMoves moves;

        if (source.isExtractedFolder) {
            const fs::path src(source.path);
            const fs::path base = fs::exists(src / "sys") ? src : src / "DATA";
            const std::string sysSrc = (base / "sys").string();
            const std::string filesSrc = (base / "files").string();
            const bool inImportDir = source.path.rfind(ROOT_DIR + "/switch/kartpad/import", 0) == 0;

            bool ok;
            if (inImportDir) {
                // Same SD volume: move instead of copying 2.7 GB.
                ec = mkw::fsx::MakeDirs(part);
                ok = !ec && moves.Move(sysSrc, fs::path(part) / "sys") &&
                     moves.Move(filesSrc, fs::path(part) / "files");
            } else {
                const std::uint64_t total = TreeBytes(sysSrc) + TreeBytes(filesSrc);
                std::uint64_t copied = 0;
                ok = CopyFolderWithProgress(sysSrc, part + "/sys", copied, total) &&
                     CopyFolderWithProgress(filesSrc, part + "/files", copied, total);
            }
            if (m_cancelFlag) {
                Discard(part, moves);
                Finish(JobStatus::State::Cancelled, "Import cancelled.");
                return;
            }
            if (!ok) {
                Discard(part, moves);
                Finish(JobStatus::State::Failed, "Could not copy the extracted game folder.");
                return;
            }
            FinishGameImport(part, "Game files imported.", moves);
            return;
        }

        kartpad::disc::ExtractOptions opts;
        opts.progress = ProgressCallback;
        opts.progressUser = this;
        ec = mkw::fsx::MakeDirs(part);
        const auto res = kartpad::disc::extractDataPartition(source.path, part, opts,
                                                            kartpad::disc::platformAesCbcDecrypt());
        if (m_cancelFlag) {
            fs::remove_all(part, ec);
            Finish(JobStatus::State::Cancelled, "Import cancelled.");
            return;
        }
        if (!res.ok) {
            fs::remove_all(part, ec);
            Finish(JobStatus::State::Failed, res.message);
            return;
        }
        FinishGameImport(part, res.message, moves);
    }

    void BackgroundVerify() {
        std::error_code ec;
        fs::remove(VerifiedMarker(), ec);
        const std::string invalid = ValidateGameTree(GameDataDir());
        if (!invalid.empty()) {
            Finish(JobStatus::State::Failed, invalid);
            return;
        }
        if (!WriteVerifiedMarker()) {
            Finish(JobStatus::State::Failed, "Could not record the verification result.");
            return;
        }
        Finish(JobStatus::State::Succeeded, "Game files verified.");
    }

    // Empty string when `pack` is a usable RetroRewind6 folder; fills `version` from version.txt.
    static std::string ValidateRetroRewindPack(const std::string& pack, std::string* version) {
        if (!fs::exists(pack + "/xml/RetroRewind6.xml")) return "Missing xml/RetroRewind6.xml.";
        if (!fs::exists(pack + "/Binaries/Code.pul")) return "Missing Binaries/Code.pul.";
        if (version) {
            std::ifstream in(pack + "/version.txt");
            std::string v;
            std::getline(in, v);
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
            *version = v.empty() ? "unknown" : v;
        }
        return {};
    }

    void BackgroundRRImport(std::string folderPath) {
        // Accept the RetroRewind6 folder itself or a folder that contains it.
        fs::path src(folderPath);
        if (src.filename() != "RetroRewind6" && fs::exists(src / "RetroRewind6")) src /= "RetroRewind6";
        std::string version;
        const std::string invalid = ValidateRetroRewindPack(src.string(), &version);
        if (!invalid.empty()) {
            Finish(JobStatus::State::Failed, "Not a Retro Rewind pack: " + invalid);
            return;
        }

        std::error_code ec;
        const std::string live = RetroRewindDir();
        const std::string part = live + ".part";
        fs::remove_all(part, ec);
        ec = mkw::fsx::MakeDirs(fs::path(live).parent_path());

        // Same SD volume: try a move first, then fall back to copying.
        SourceMoves moves;
        bool ok = moves.Move(src, part);
        if (!ok) {
            std::uint64_t copied = 0;
            ok = CopyFolderWithProgress(src.string(), part, copied, TreeBytes(src.string()));
        }
        if (m_cancelFlag) {
            Discard(part, moves);
            Finish(JobStatus::State::Cancelled, "Import cancelled.");
            return;
        }
        if (!ok || !ValidateRetroRewindPack(part, nullptr).empty()) {
            Discard(part, moves);
            Finish(JobStatus::State::Failed, "Could not copy the Retro Rewind pack.");
            return;
        }
        std::string err;
        if (!SwapIntoPlace(part, live, err)) {
            Discard(part, moves);
            Finish(JobStatus::State::Failed, err);
            return;
        }
        Finish(JobStatus::State::Succeeded, "Retro Rewind " + version + " imported.");
    }
};

DiscService& Disc() {
    static DiscServiceImpl instance;
    return instance;
}

} // namespace mkw
