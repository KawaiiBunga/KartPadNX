#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <random>
#include <string>

#include <nlohmann/json.hpp>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "data/fsutil.h"
#include "data/game_paths.h"
#include "data/mii_record.h"
#include "data/mii_template.h"
#include "data/switch_mii.h"
#include "data/text_utf16.h"
#include "mkw/data.h"
#include "wiinand/miidb.h"

namespace fs = std::filesystem;

namespace mkw {
namespace {

constexpr const char* kColorNames[kMiiFavoriteColors] = {
    "Red", "Orange", "Yellow", "Light green", "Green", "Blue",
    "Light blue", "Pink", "Purple", "Brown", "White", "Black",
};

constexpr const char* kSdmcPrefix = "sdmc:";
constexpr std::size_t kMiiNameLimit = 10;  // characters, UTF-16

// Maps a launcher constant ("sdmc:/...") to the active SD root. On console SdRoot() is "sdmc:",
// so this is the identity; on the host test it becomes a relative directory.
std::string SdPath(const std::string& sdmcPath) {
    const std::string prefix = kSdmcPrefix;
    if (sdmcPath.rfind(prefix, 0) == 0) return paths::SdRoot() + sdmcPath.substr(prefix.size());
    return sdmcPath;
}

std::string LauncherConfigPath() { return SdPath(kLauncherConfig); }
std::string MiiBackupDir() { return SdPath(kSavesDir) + "/mii"; }

// Writes `content` through a ".part" file so a failed write never truncates the live file.
bool WriteAtomically(const std::string& path, const std::string& content, std::string* error) {
    if (auto ec = fsx::MakeDirs(fs::path(path).parent_path())) {
        if (error) *error = "Could not create " + fs::path(path).parent_path().generic_string();
        return false;
    }
    const std::string part = path + ".part";
    std::FILE* out = std::fopen(part.c_str(), "wb");
    if (!out) {
        if (error) *error = "Could not write " + path;
        return false;
    }
    const bool wrote = std::fwrite(content.data(), 1, content.size(), out) == content.size();
    const bool closed = std::fclose(out) == 0;
    if (!wrote || !closed) {
        std::remove(part.c_str());
        if (error) *error = "Could not write " + path;
        return false;
    }
    std::remove(path.c_str());
    if (std::rename(part.c_str(), path.c_str()) != 0) {
        if (error) *error = "Could not replace " + path;
        return false;
    }
    return true;
}

MiiEntry ToEntry(int slot, const wiinand::Mii& m) { return record::DecodeFields(m.raw.data(), slot); }

std::string Timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

// Number of characters in a UTF-8 string (lead bytes only).
std::size_t CodePoints(const std::string& s) {
    std::size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

bool ValidName(const std::string& name, std::string* error) {
    if (name.empty()) {
        if (error) *error = "Enter a name for the Mii.";
        return false;
    }
    if (CodePoints(name) > kMiiNameLimit) {
        if (error) *error = "Mii names are limited to 10 characters.";
        return false;
    }
    std::uint8_t probe[kMiiNameLimit * 2] = {};
    if (!text::WriteName(probe, kMiiNameLimit, name)) {
        if (error) *error = "That name uses characters the console cannot store.";
        return false;
    }
    return true;
}

// Random 32-bit value for the per-install system ID.
std::uint32_t RandomWord() {
#ifdef __SWITCH__
    std::uint32_t v = 0;
    randomGet(&v, sizeof v);
    return v;
#else
    std::random_device rd;
    return rd();
#endif
}

// The system ID is one random value per install, stored in config.json. It is stable across
// launches, so every Mii the launcher makes shares it, the way Miis from one console share one ID.
bool EnsureSystemId(std::uint32_t* out, std::string* error) {
    const std::string path = LauncherConfigPath();
    nlohmann::json j = nlohmann::json::object();
    {
        std::ifstream in(path);
        if (in) {
            nlohmann::json parsed = nlohmann::json::parse(in, nullptr, false);
            if (parsed.is_object()) j = parsed;
        }
    }
    if (j.contains("mii_system_id") && j["mii_system_id"].is_number_unsigned()) {
        const auto v = j["mii_system_id"].get<std::uint64_t>();
        if (v != 0 && v <= 0xFFFFFFFFu) {
            *out = static_cast<std::uint32_t>(v);
            return true;
        }
    }
    std::uint32_t id = 0;
    while (id == 0) id = RandomWord();
    j["mii_system_id"] = id;
    if (!WriteAtomically(path, j.dump(2), error)) return false;
    *out = id;
    return true;
}

// Mii ID for a new Mii: normal type, timestamp in 4-second steps since 2006, bumped until unused.
std::uint32_t NewMiiId(const wiinand::MiiDatabase& db) {
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr)) - mii_template::kWiiEpochUnix;
    std::uint32_t stamp = static_cast<std::uint32_t>(std::max<std::int64_t>(0, now) / 4) & mii_template::kTimestampMask;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::uint32_t id = mii_template::kNormalMiiIdBit | ((stamp + attempt) & mii_template::kTimestampMask);
        bool used = false;
        for (const auto& m : db.Slots()) {
            if (m.Used() && text::Read32(m.raw.data() + mii_template::kMiiId) == id) {
                used = true;
                break;
            }
        }
        if (!used) return id;
    }
    return mii_template::kNormalMiiIdBit | stamp;
}

class MiiServiceImpl : public MiiService {
public:
    bool Load(std::string* error) override {
        db_.reset();
        dirty_ = false;
        std::vector<std::uint8_t> bytes;
        {
            // Read in its own scope: the file must be closed before Save replaces it (Windows
            // refuses to rename or remove an open file).
            std::ifstream in(Path(), std::ios::binary);
            if (!in) {
                if (error) *error = "No Mii database yet. Make a Mii, or start the game once.";
                return false;
            }
            bytes.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
        bool crcOk = false;
        bool migrated = false;
        auto parsed = wiinand::MiiDatabase::Parse(bytes, &crcOk, &migrated);
        if (!parsed) {
            if (error) *error = "The Mii database is not a valid RFL_DB.dat.";
            return false;
        }
        db_ = std::move(*parsed);
        // A file the old launcher wrote (0x1F1E0 bytes) is extended by Parse; keep it saved.
        dirty_ = migrated;
        const int repaired = RepairBrokenMiis();
        if (repaired > 0) dirty_ = true;
        // Both fixes are on the file's own data, and Save keeps a backup, so write them back now.
        if (dirty_) Save(nullptr);
        return true;
    }

    bool Loaded() const override { return db_.has_value(); }
    std::string Path() const override { return paths::MiiDatabasePath(); }

    std::vector<MiiEntry> List() const override {
        std::vector<MiiEntry> out;
        if (!db_) return out;
        const auto& slots = db_->Slots();
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].Used()) out.push_back(ToEntry(static_cast<int>(i), slots[i]));
        }
        return out;
    }

    bool Create(const std::string& name, bool female, int favoriteColor, std::string* error) override {
        if (!ValidName(name, error)) return false;
        if (favoriteColor < 0 || favoriteColor >= kMiiFavoriteColors) {
            if (error) *error = "Pick a favourite colour from the list.";
            return false;
        }

        if (!db_) {
            if (FileExists(Path())) {
                // An existing file that cannot be read is refused, never replaced.
                if (!Load(error)) return false;
            } else {
                db_ = wiinand::MiiDatabase::Empty();
            }
        }
        const auto free = db_->FirstFree();
        if (!free) {
            if (error) *error = "The Mii database is full (100 Miis).";
            return false;
        }

        std::uint32_t systemId = 0;
        if (!EnsureSystemId(&systemId, error)) return false;

        const MiiEntry entry = record::FactoryEntry(name, female, favoriteColor);
        wiinand::Mii mii{};
        record::EncodeFields(entry, mii.raw.data());
        record::SetIdentity(mii.raw.data(), NewMiiId(*db_), systemId, entry.creator);

        db_->Put(*free, mii);
        dirty_ = true;
        return Save(error);
    }

    bool Update(const MiiEntry& e) override {
        if (!db_ || e.slot < 0 || e.slot >= static_cast<int>(wiinand::kMiiSlots)) return false;
        if (!ValidName(e.name, nullptr)) return false;
        wiinand::Mii m = db_->Slots()[static_cast<std::size_t>(e.slot)];
        if (!m.Used()) return false;
        // Writes every field MiiEntry carries. Mii ID, system ID and creator are left as they are.
        record::EncodeFields(e, m.raw.data());
        db_->Put(static_cast<std::size_t>(e.slot), m);
        dirty_ = true;
        return true;
    }

    bool Duplicate(int slot, std::string* error) override {
        if (!db_ || slot < 0 || slot >= static_cast<int>(wiinand::kMiiSlots)) return false;
        const auto free = db_->FirstFree();
        if (!free) {
            if (error) *error = "The Mii database is full (100 Miis).";
            return false;
        }
        wiinand::Mii copy = db_->Slots()[static_cast<std::size_t>(slot)];
        if (!copy.Used()) return false;
        // A copy needs its own Mii ID; keep the ID's type bits (top byte), vary the rest.
        std::mt19937 rng(static_cast<std::uint32_t>(std::time(nullptr)) ^ static_cast<std::uint32_t>(slot));
        std::uint32_t id;
        do {
            id = (text::Read32(copy.raw.data() + mii_template::kMiiId) & 0xFF000000u) | (rng() & 0x00FFFFFFu);
        } while (IdInUse(id));
        text::Write32(copy.raw.data() + mii_template::kMiiId, id);
        db_->Put(*free, copy);
        dirty_ = true;
        return true;
    }

    bool SyncFromSwitch(int* added, int* updated, std::string* error) override {
        *added = *updated = 0;
        std::vector<switch_mii::SwitchMii> miis;
        if (!switch_mii::ReadSystemMiis(&miis, error)) return false;
        if (miis.empty()) return true;
        if (!db_) {
            if (FileExists(Path())) {
                if (!Load(error)) return false;  // an unreadable database is never replaced
            } else {
                db_ = wiinand::MiiDatabase::Empty();
            }
        }
        std::uint32_t systemId = 0;
        if (!EnsureSystemId(&systemId, error)) return false;
        bool full = false;
        for (const auto& s : miis) {
            const std::uint32_t id = switch_mii::WiiMiiId(s);
            MiiEntry entry = switch_mii::ToWii(s);
            if (!ValidName(entry.name, nullptr)) entry.name = "Mii";  // characters the Wii cannot store
            std::optional<std::size_t> slot = SlotOf(id);
            const bool known = slot.has_value();
            if (!known) slot = db_->FirstFree();
            if (!slot) {
                full = true;
                continue;
            }
            wiinand::Mii mii = known ? db_->Slots()[*slot] : wiinand::Mii{};
            const auto before = mii.raw;
            record::EncodeFields(entry, mii.raw.data());
            record::SetIdentity(mii.raw.data(), id, systemId, entry.creator);
            if (known && mii.raw == before) continue;
            db_->Put(*slot, mii);
            ++(known ? *updated : *added);
            dirty_ = true;
        }
        if (dirty_ && !Save(error)) return false;
        if (full && error) *error = "The Mii database is full (100 Miis): some Switch Miis were not copied.";
        return true;
    }

    bool Remove(int slot) override {
        if (!db_ || !db_->Remove(static_cast<std::size_t>(slot))) return false;
        dirty_ = true;
        return true;
    }

    bool Dirty() const override { return dirty_; }

    bool Save(std::string* error) override {
        if (!db_) return false;
        const std::string live = Path();
        if (FileExists(live)) {
            if (auto ec = fsx::MakeDirs(MiiBackupDir())) {
                if (error) *error = "Could not create the Mii backup folder.";
                return false;
            }
            const std::string backup = MiiBackupDir() + "/RFL_DB-" + Timestamp() + ".dat";
            if (fsx::CopyFile(live, backup)) {
                if (error) *error = "Could not back up the Mii database.";
                return false;
            }
        }
        const auto bytes = db_->Build();
        const std::string part = live + ".part";
        if (auto ec = fsx::MakeDirs(fs::path(live).parent_path())) {
            if (error) *error = "Could not create the Mii database folder.";
            return false;
        }
        {
            std::FILE* out = std::fopen(part.c_str(), "wb");
            if (!out) {
                if (error) *error = "Could not write the Mii database.";
                return false;
            }
            const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
            const bool closed = std::fclose(out) == 0;
            if (!wrote || !closed) {
                std::remove(part.c_str());
                if (error) *error = "Could not write the Mii database.";
                return false;
            }
        }
        std::remove(live.c_str());
        if (std::rename(part.c_str(), live.c_str()) != 0) {
            if (error) *error = "Could not replace the Mii database (a backup is in the Mii backup folder).";
            return false;
        }
        dirty_ = false;
        return true;
    }

private:
    // A Mii with no face block, or a birthday of month 0 or day 0, is broken: the game shows it
    // without a face, and the Mii Channel rejects the licence linked to it. Refill the face and
    // birthday from the factory defaults for its gender. Name, colour, size and identity are kept.
    int RepairBrokenMiis() {
        int repaired = 0;
        for (std::size_t slot = 0; slot < wiinand::kMiiSlots; ++slot) {
            wiinand::Mii m = db_->Slots()[slot];
            if (!m.Used()) continue;
            MiiEntry e = record::DecodeFields(m.raw.data(), static_cast<int>(slot));
            if (e.birthMonth != 0 && e.birthDay != 0 && !record::FaceBlockEmpty(m.raw.data())) continue;
            record::ApplyFactoryFace(e, e.female);
            record::EncodeFields(e, m.raw.data());
            db_->Put(slot, m);
            ++repaired;
        }
        return repaired;
    }

    static bool FileExists(const std::string& path) {
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        std::fclose(f);
        return true;
    }

    std::optional<std::size_t> SlotOf(std::uint32_t miiId) const {
        const auto& slots = db_->Slots();
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].Used() && text::Read32(slots[i].raw.data() + mii_template::kMiiId) == miiId) return i;
        }
        return std::nullopt;
    }

    bool IdInUse(std::uint32_t id) const {
        for (const auto& m : db_->Slots()) {
            if (m.Used() && text::Read32(m.raw.data() + mii_template::kMiiId) == id) return true;
        }
        return false;
    }

    std::optional<wiinand::MiiDatabase> db_;
    bool dirty_ = false;
};

}  // namespace

const char* MiiColorName(int color) {
    return color >= 0 && color < kMiiFavoriteColors ? kColorNames[color] : "?";
}

MiiService& Miis() {
    static MiiServiceImpl instance;
    return instance;
}

}  // namespace mkw
