#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "mkw/services.h"

namespace mkw {

// ---- Saves (rksys.dat backups) -----------------------------------------------------------

struct SaveSlot {
    std::string name;
    std::string timestamp;
    std::string path;
};

class SaveService {
public:
    // The rksys.dat the game uses for this variant.
    virtual std::string GetSavePath(Variant v) = 0;
    // True once the game has created this variant's save.
    virtual bool HasSave(Variant v) = 0;
    // Copies the live save into a new backup slot; keeps the newest 5.
    virtual bool BackupCurrent(Variant v) = 0;
    // Replaces the live save with a backup slot (the live save is backed up first).
    virtual bool Restore(Variant v, const std::string& slotId) = 0;
    virtual std::vector<SaveSlot> ListBackups(Variant v) = 0;
    virtual bool ImportSave(Variant v, const std::string& sourcePath) = 0;
    virtual bool ExportSave(Variant v, const std::string& destPath) = 0;
    virtual ~SaveService() = default;
};

// ---- Miis (NAND /shared2/menu/FaceLib/RFL_DB.dat) ----------------------------------------

inline constexpr int kMiiFavoriteColors = 12;
const char* MiiColorName(int color);

struct MiiEntry {
    int slot = -1;
    std::string name;       // up to 10 characters
    std::string creator;
    bool female = false;
    int favoriteColor = 0;  // 0..11
    int height = 64;        // 0..127
    int weight = 64;        // 0..127
    bool favorite = false;
    std::uint32_t miiId = 0;
    std::uint32_t systemId = 0;
    int birthMonth = 1;     // 1..12
    int birthDay = 1;       // 1..31

    // Face (layout and ranges: docs/launcher/research/mii-format.md, from WheelWizard MiiSerializer).
    int faceShape = 0;      // 0..7
    int skinColor = 0;      // 0..5
    int facialFeature = 0;  // 0..11
    int hairType = 33;      // 0..71
    int hairColor = 1;      // 0..7
    bool hairFlipped = false;
    int browType = 6;       // 0..23
    int browRotation = 6;   // 0..11
    int browColor = 1;      // 0..7
    int browSize = 4;       // 0..8
    int browVertical = 10;  // 3..18
    int browSpacing = 2;    // 0..12
    int eyeType = 2;        // 0..47
    int eyeRotation = 4;    // 0..7
    int eyeVertical = 12;   // 0..18
    int eyeColor = 0;       // 0..5
    int eyeSize = 4;        // 0..7
    int eyeSpacing = 2;     // 0..12
    int noseType = 1;       // 0..11
    int noseSize = 4;       // 0..8
    int noseVertical = 9;   // 0..18
    int lipType = 23;       // 0..23
    int lipColor = 0;       // 0..2
    int lipSize = 4;        // 0..8
    int lipVertical = 13;   // 0..18
    int glassesType = 0;    // 0..8
    int glassesColor = 0;   // 0..5
    int glassesSize = 4;    // 0..7
    int glassesVertical = 10;  // 0..20
    int mustacheType = 0;   // 0..3
    int beardType = 0;      // 0..3
    int facialHairColor = 0;   // 0..7
    int mustacheSize = 4;   // 0..8
    int mustacheVertical = 10; // 0..16
    bool moleOn = false;
    int moleSize = 4;       // 0..8
    int moleVertical = 20;  // 0..30
    int moleHorizontal = 2; // 0..16
};

class MiiService {
public:
    // Reads the database into a working copy. False with a message if it is missing or invalid.
    virtual bool Load(std::string* error) = 0;
    virtual bool Loaded() const = 0;
    virtual std::string Path() const = 0;
    virtual std::vector<MiiEntry> List() const = 0;
    virtual bool Update(const MiiEntry& mii) = 0;   // name, gender, colour, size, favourite
    // Creates a new default-face Mii in the first free slot. Makes RFL_DB.dat if it does not exist
    // yet. A damaged database is refused, never overwritten. Saves before returning.
    virtual bool Create(const std::string& name, bool female, int favoriteColor, std::string* error) = 0;
    virtual bool Duplicate(int slot, std::string* error) = 0;
    // Copies the Switch's own Miis in (data/switch_mii.h): one synced before is updated in its slot,
    // a new one takes the first free slot; Miis made elsewhere are left alone. Makes RFL_DB.dat if
    // needed and saves when anything changed. `added` and `updated` count them.
    virtual bool SyncFromSwitch(int* added, int* updated, std::string* error) = 0;
    virtual bool Remove(int slot) = 0;
    virtual bool Dirty() const = 0;
    // Backs up the file, then writes the working copy with its checksum recomputed.
    virtual bool Save(std::string* error) = 0;
    virtual ~MiiService() = default;
};

// ---- Licenses (the four rksys.dat RKPD slots) -----------------------------------------------

struct LicenseEntry {
    int index = 0;           // 0..3
    bool used = false;
    std::string name;        // the license's Mii name
    int vr = 0;              // race rating
    int br = 0;              // battle rating
    std::uint32_t wins = 0;  // offline VS wins
    std::uint32_t losses = 0;
    std::uint32_t avatarId = 0;
    std::uint32_t clientId = 0;
};

class LicenseService {
public:
    virtual bool Load(Variant v, std::string* error) = 0;
    virtual bool Loaded() const = 0;
    virtual Variant LoadedVariant() const = 0;
    // False when the file's checksum format was not recognised; edits are then refused.
    virtual bool Writable() const = 0;
    virtual std::vector<LicenseEntry> List() const = 0;
    virtual bool SetName(int index, const std::string& name) = 0;
    virtual bool SetRatings(int index, int vr, int br) = 0;
    virtual bool UnlockEverything(int index) = 0;
    virtual bool LinkMii(int index, const MiiEntry& mii) = 0;
    virtual bool Dirty() const = 0;
    // Backs up the live save, then writes the edited one with its CRC-32 recomputed.
    virtual bool Save(std::string* error) = 0;
    virtual ~LicenseService() = default;
};

// ---- Settings ----------------------------------------------------------------------------

// Launcher settings (sdmc:/switch/mkw-launcher/config.json) and the Config.toml keys of the
// game (runtime_config.h) that the launcher edits.
struct LauncherSettings {
    // Launcher (config.json)
    int accent = 0;          // ui::Accent index: 0 Mushroom, 1 Shell, 2 Star, 3 Boo
    bool sounds = true;      // UI and countdown sound effects
    int updateChannel = 0;   // 0 stable, 1 beta
    int lastVariant = 0;     // 0 vanilla, 1 Retro Rewind; used by the quick-launch gesture
    bool backupBeforeLaunch = true;

    // Game audio (Config.toml [audio])
    int masterVolume = 100;  // volume
    int musicVolume = 100;   // music_volume
    int effectsVolume = 100; // sound_effects_volume
    int voicesVolume = 100;  // voices_volume
    bool muted = false;      // muted
    bool mixWorker = true;   // mix_worker

    // Game video (Config.toml [video])
    bool showFps = true;     // show_fps
    bool widescreen = true;  // widescreen
};

class SettingsService {
public:
    // Fields missing from the files keep their current values.
    virtual void Load(LauncherSettings& s) = 0;
    // Writes config.json and the game's Config.toml keys, each through a temporary file. Other
    // keys and lines of both files are kept. False if either write failed.
    virtual bool Save(const LauncherSettings& s) = 0;
    // True while sdmc:/switch/kartpad/perf.cfg overrides the game's built-in tuning.
    virtual bool HasPerformanceOverrides() = 0;
    // Moves perf.cfg to perf.cfg.bak so the game uses its built-in tuning.
    virtual bool ResetPerformanceTuning() = 0;
    virtual ~SettingsService() = default;
};

SaveService& Saves();
MiiService& Miis();
LicenseService& Licenses();
SettingsService& Settings();

}  // namespace mkw
