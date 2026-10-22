#include "mkw/data.h"
#include "data/fsutil.h"
#include "data/game_paths.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#ifdef _WIN32
const std::string ROOT_DIR = ".";
#else
const std::string ROOT_DIR = "sdmc:";
#endif
namespace fs = std::filesystem;

namespace mkw {
namespace {

constexpr std::uintmax_t kRksysSize = 0x2BC000;
constexpr size_t kKeepBackups = 5;

std::string VariantDir(Variant v) { return v == Variant::Vanilla ? "vanilla" : "rr"; }

fs::path BackupRoot(Variant v) { return fs::path(ROOT_DIR + "/switch/mkw-launcher/saves") / VariantDir(v); }

bool IsValidRksys(const fs::path& p) {
    std::error_code ec;
    if (fs::file_size(p, ec) != kRksysSize || ec) return false;
    std::ifstream file(p, std::ios::binary);
    char magic[4];
    return file.read(magic, 4) && std::string_view(magic, 4) == "RKSD";
}

std::string Timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::vector<fs::path> BackupDirs(Variant v) {
    std::vector<fs::path> dirs;
    std::error_code ec;
    for (auto it = fs::directory_iterator(BackupRoot(v), ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec)) dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end());  // timestamps sort chronologically
    return dirs;
}

}  // namespace

class SaveServiceImpl : public SaveService {
public:
    std::string GetSavePath(Variant v) override { return paths::SavePath(v); }
    bool HasSave(Variant v) override {
        std::error_code ec;
        return std::filesystem::exists(GetSavePath(v), ec);
    }

    bool BackupCurrent(Variant v) override {
        return BackupTo(v, nullptr);
    }

    bool Restore(Variant v, const std::string& slotId) override {
        if (slotId.empty() || slotId.find('/') != std::string::npos || slotId.find("..") != std::string::npos) return false;
        return ReplaceLiveSave(v, BackupRoot(v) / slotId / "rksys.dat");
    }

    std::vector<SaveSlot> ListBackups(Variant v) override {
        std::vector<SaveSlot> slots;
        for (const auto& dir : BackupDirs(v)) {
            SaveSlot slot;
            slot.name = dir.filename().string();
            slot.timestamp = slot.name;
            slot.path = dir.string();
            slots.push_back(slot);
        }
        return slots;
    }

    bool ImportSave(Variant v, const std::string& sourcePath) override {
        return ReplaceLiveSave(v, sourcePath);
    }

    bool ExportSave(Variant v, const std::string& destPath) override {
        std::error_code ec;
        const std::string current = GetSavePath(v);
        if (!fs::exists(current, ec)) return false;
        ec = mkw::fsx::CopyFile(current, destPath);
        return !ec;
    }

private:
    bool BackupTo(Variant v, std::string* createdSlot) {
        std::error_code ec;
        const std::string current = GetSavePath(v);
        if (!fs::exists(current, ec)) return false;

        const fs::path base = BackupRoot(v) / Timestamp();
        fs::path dir = base;
        for (int n = 1; fs::exists(dir, ec); ++n) dir = base.string() + "-" + std::to_string(n);
        ec = mkw::fsx::MakeDirs(dir);
        if (ec) return false;

        ec = mkw::fsx::CopyFile(current, dir / "rksys.dat");
        if (ec) {
            fs::remove_all(dir, ec);
            return false;
        }
        {
            std::ofstream meta(dir / "meta.json", std::ios::trunc);
            meta << "{ \"timestamp\": \"" << dir.filename().string() << "\", \"variant\": \"" << VariantDir(v) << "\" }";
            meta.close();
            if (!meta) {
                fs::remove_all(dir, ec);
                return false;
            }
        }
        if (createdSlot) *createdSlot = dir.filename().string();

        // Keep the newest kKeepBackups.
        auto dirs = BackupDirs(v);
        for (size_t i = 0; i + kKeepBackups < dirs.size(); ++i) fs::remove_all(dirs[i], ec);
        return true;
    }

    // Validates `source`, stages it next to the live save, backs up the live save, then swaps.
    // The source is staged first so backup rotation can never delete the slot being restored.
    bool ReplaceLiveSave(Variant v, const fs::path& source) {
        if (!IsValidRksys(source)) return false;

        std::error_code ec;
        const std::string dest = GetSavePath(v);
        const std::string part = dest + ".part";
        const std::string bak = dest + ".bak";
        ec = mkw::fsx::MakeDirs(fs::path(dest).parent_path());
        ec = mkw::fsx::CopyFile(source, part);
        if (ec || !IsValidRksys(part)) {
            fs::remove(part, ec);
            return false;
        }

        const bool hadLive = fs::exists(dest, ec);
        if (hadLive && !BackupTo(v, nullptr)) {
            fs::remove(part, ec);
            return false;
        }
        if (hadLive) {
            fs::remove(bak, ec);
            fs::rename(dest, bak, ec);
            if (ec) {
                fs::remove(part, ec);
                return false;
            }
        }
        fs::rename(part, dest, ec);
        if (ec) {
            if (hadLive) {
                std::error_code rollback;
                fs::rename(bak, dest, rollback);
            }
            fs::remove(part, ec);
            return false;
        }
        fs::remove(bak, ec);
        return true;
    }
};

SaveService& Saves() {
    static SaveServiceImpl instance;
    return instance;
}

}  // namespace mkw
