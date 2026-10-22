// MKW Launcher service contracts shared by the UI and the backend modules. Each module
// implements its services in its own directory:
//   source/app, source/ui, source/screens  -> UI: calls services, never touches files itself
//   source/disc                            -> DiscService
//   source/payload, source/net             -> PayloadService, UpdateService
//   source/data                            -> SaveService, MiiService, LicenseService, SettingsService (mkw/data.h)
// Services run long work on their own thread and are polled by the UI each frame; no callbacks
// into UI code.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mkw {

inline constexpr const char* kLauncherTitle = "MKW Launcher";
inline constexpr const char* kLauncherAuthor = "kawaii";
inline constexpr const char* kLauncherVersion = "1.0.0";

inline constexpr const char* kLauncherNroPath = "sdmc:/switch/mkw-launcher.nro";
inline constexpr const char* kLauncherDir = "sdmc:/switch/mkw-launcher";
inline constexpr const char* kGamesDir = "sdmc:/switch/mkw-launcher/.games";
inline constexpr const char* kUpdateDir = "sdmc:/switch/mkw-launcher/update";
inline constexpr const char* kSavesDir = "sdmc:/switch/mkw-launcher/saves";
inline constexpr const char* kLauncherConfig = "sdmc:/switch/mkw-launcher/config.json";
inline constexpr const char* kGameDataDir = "sdmc:/switch/kartpad/data";

// Where this launcher NRO lives on SD: argv[0] when hbloader passed one, else kLauncherNroPath.
// The updater replaces this file and the game returns to it.
void SetLauncherSelfPath(const char* argv0);
const std::string& LauncherSelfPath();

enum class Variant : std::uint8_t { Vanilla = 0, RetroRewind = 1 };

const char* VariantId(Variant v);           // "vanilla" | "rr"
const char* VariantDisplayName(Variant v);  // "Vanilla" | "Retro Rewind"

// Progress of a background job. `fraction` < 0 means indeterminate.
struct JobStatus {
    enum class State : std::uint8_t { Idle, Running, Succeeded, Failed, Cancelled };
    State state = State::Idle;
    float fraction = -1.0f;
    std::string stage;    // short label for the UI, e.g. "Decrypting partition"
    std::string message;  // final result or error, safe to display
};

// ---- Disc import (source/disc) ---------------------------------------------------------
struct DiscCandidate {
    std::string path;      // sdmc:/... .wbfs/.iso file or extracted folder
    bool isExtractedFolder = false;
    std::uint64_t sizeBytes = 0;
};

class DiscService {
public:
    // True when kGameDataDir holds a validated RMCP01 rev0 tree (main.dol + StaticR.rel hashes).
    virtual bool HasValidGameData() = 0;
    virtual std::vector<DiscCandidate> ScanForSources() = 0;  // sd:/iso, sd:/wbfs, sd:/ root, etc.
    virtual bool StartImport(const DiscCandidate& source) = 0;
    virtual bool StartVerify() = 0;  // re-hash existing data
    virtual JobStatus Poll() = 0;
    virtual void Cancel() = 0;

    // Retro Rewind: user-supplied RetroRewind6 folder.
    virtual bool HasValidRetroRewind(std::string* versionOut) = 0;
    virtual bool StartRetroRewindImport(const std::string& folderPath) = 0;
    virtual ~DiscService() = default;
};

// ---- Bundled game payloads (source/payload) ----------------------------------------------
struct PayloadInfo {
    Variant variant;
    bool bundled = false;      // present in romfs
    bool unpacked = false;     // kGamesDir copy exists and matches the bundled SHA-256
    std::string buildStamp;    // "KARTPAD-BUILD 2026-..."
    std::uint64_t rawSize = 0;
};

class PayloadService {
public:
    virtual std::vector<PayloadInfo> List() = 0;
    virtual bool StartUnpack(Variant v) = 0;  // romfs zstd -> .games/<id>.nro.part -> verify -> rename
    virtual JobStatus Poll() = 0;
    // Writes session.json, calls envSetNextLoad(game, argv) with
    // --variant=<id> --profile=<profileId> --return=<kLauncherNroPath>. Caller then exits the app loop.
    virtual bool LaunchGame(Variant v, const std::string& profileId, std::string* errorOut) = 0;
    // Deletes the unpacked games in kGamesDir; they are unpacked again before the next launch.
    virtual bool RemoveUnpacked() = 0;
    virtual ~PayloadService() = default;
};

// ---- Updater (source/net) ----------------------------------------------------------------
struct UpdateInfo {
    bool available = false;
    std::string version;     // release tag
    std::string changelog;   // markdown-ish plain text
    std::uint64_t downloadSize = 0;
};

class UpdateService {
public:
    virtual bool StartCheck(bool force) = 0;  // respects a 6 h throttle unless force
    virtual bool StartDownloadAndInstall() = 0;
    virtual JobStatus Poll() = 0;
    virtual UpdateInfo Latest() = 0;
    virtual bool RelaunchIntoNewVersion() = 0;
    virtual void Cancel() = 0;
    virtual ~UpdateService() = default;
};

// ---- Accessors (implemented by each module; a stub returns a fake for UI work) -----------------
DiscService& Disc();
PayloadService& Payloads();
UpdateService& Updates();

}  // namespace mkw
