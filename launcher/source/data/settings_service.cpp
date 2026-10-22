// Launcher and game settings files. Both are edited in place: unknown JSON keys and every
// other Config.toml line are kept, and each file is replaced through a temporary file.
#include "mkw/data.h"

#include "data/fsutil.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "mkw/services.h"

namespace mkw {
namespace {

namespace fs = std::filesystem;

constexpr const char* kGameConfig = "sdmc:/switch/kartpad/Config.toml";
constexpr const char* kPerfCfg = "sdmc:/switch/kartpad/perf.cfg";

std::string Trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return {};
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> ReadLines(const char* path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

// Minimal TOML access for flat `key = value` lines under `[section]`, matching how the game's
// WriteSetting edits the same file.
struct TomlFile {
    std::vector<std::string> lines;

    // Index of `key` inside `section`, or -1. `sectionEnd` receives the insertion point.
    int Find(const std::string& section, const std::string& key, size_t* sectionEnd) const {
        bool inSection = false;
        size_t end = lines.size();
        bool found = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string t = Trim(lines[i]);
            if (!t.empty() && t.front() == '[') {
                if (inSection) {
                    end = i;
                    break;
                }
                inSection = t == "[" + section + "]";
                found = found || inSection;
                continue;
            }
            if (!inSection) continue;
            const auto eq = t.find('=');
            if (eq != std::string::npos && Trim(t.substr(0, eq)) == key) return static_cast<int>(i);
        }
        if (sectionEnd) *sectionEnd = found ? end : std::string::npos;
        return -1;
    }

    bool Get(const std::string& section, const std::string& key, std::string& out) const {
        const int i = Find(section, key, nullptr);
        if (i < 0) return false;
        std::string v = lines[static_cast<size_t>(i)].substr(lines[static_cast<size_t>(i)].find('=') + 1);
        const auto hash = v.find('#');
        if (hash != std::string::npos) v = v.substr(0, hash);
        out = Trim(v);
        return true;
    }

    void Set(const std::string& section, const std::string& key, const std::string& value) {
        size_t sectionEnd = std::string::npos;
        const int i = Find(section, key, &sectionEnd);
        const std::string line = key + " = " + value;
        if (i >= 0) {
            lines[static_cast<size_t>(i)] = line;
            return;
        }
        if (sectionEnd == std::string::npos) {
            if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back("");
            lines.push_back("[" + section + "]");
            lines.push_back(line);
            return;
        }
        // Insert after the section's last non-blank line.
        size_t at = sectionEnd;
        while (at > 0 && Trim(lines[at - 1]).empty()) --at;
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), line);
    }
};

void GetBool(const TomlFile& t, const char* section, const char* key, bool& out) {
    std::string v;
    if (t.Get(section, key, v)) out = v == "true";
}

void GetPercent(const TomlFile& t, const char* section, const char* key, int& out) {
    std::string v;
    if (!t.Get(section, key, v)) return;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (end != v.c_str() && d >= 0.0 && d <= 1.0) out = static_cast<int>(std::lround(d * 100.0));
}

std::string Percent(int p) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.2f", p / 100.0);
    return buf;
}

// Writes `path` through a temporary file so a failed write never truncates the live file.
bool WriteAtomically(const std::string& path, const std::string& content) {
    std::error_code ec;
    ec = mkw::fsx::MakeDirs(fs::path(path).parent_path());
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << content;
        out.close();
        if (!out) return false;
    }
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    return !ec;
}

}  // namespace

class SettingsServiceImpl : public SettingsService {
public:
    void Load(LauncherSettings& s) override {
        std::ifstream in(mkw::kLauncherConfig);
        if (in) {
            const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_object()) {
                s.accent = j.value("accent", s.accent);
                s.sounds = j.value("sounds", s.sounds);
                s.updateChannel = j.value("update_channel", s.updateChannel);
                s.lastVariant = j.value("last_variant", s.lastVariant);
                s.backupBeforeLaunch = j.value("backup_before_launch", s.backupBeforeLaunch);
            }
        }

        TomlFile t{ReadLines(kGameConfig)};
        GetPercent(t, "audio", "volume", s.masterVolume);
        GetPercent(t, "audio", "music_volume", s.musicVolume);
        GetPercent(t, "audio", "sound_effects_volume", s.effectsVolume);
        GetPercent(t, "audio", "voices_volume", s.voicesVolume);
        GetBool(t, "audio", "muted", s.muted);
        GetBool(t, "audio", "mix_worker", s.mixWorker);
        GetBool(t, "video", "show_fps", s.showFps);
        GetBool(t, "video", "widescreen", s.widescreen);
    }

    bool Save(const LauncherSettings& s) override {
        // Merge into the existing file: other modules keep their own keys here (mii_system_id).
        nlohmann::json j = nlohmann::json::object();
        {
            std::ifstream in(mkw::kLauncherConfig);
            if (in) {
                nlohmann::json existing = nlohmann::json::parse(in, nullptr, false);
                if (existing.is_object()) j = std::move(existing);
            }
        }
        j["schema"] = 1;
        j["accent"] = s.accent;
        j["sounds"] = s.sounds;
        j["update_channel"] = s.updateChannel;
        j["last_variant"] = s.lastVariant;
        j["backup_before_launch"] = s.backupBeforeLaunch;
        const bool launcherOk = WriteAtomically(mkw::kLauncherConfig, j.dump(2));

        TomlFile t{ReadLines(kGameConfig)};
        t.Set("audio", "volume", Percent(s.masterVolume));
        t.Set("audio", "music_volume", Percent(s.musicVolume));
        t.Set("audio", "sound_effects_volume", Percent(s.effectsVolume));
        t.Set("audio", "voices_volume", Percent(s.voicesVolume));
        t.Set("audio", "muted", s.muted ? "true" : "false");
        t.Set("audio", "mix_worker", s.mixWorker ? "true" : "false");
        t.Set("video", "show_fps", s.showFps ? "true" : "false");
        t.Set("video", "widescreen", s.widescreen ? "true" : "false");
        std::ostringstream out;
        for (const auto& line : t.lines) out << line << '\n';
        const bool gameOk = WriteAtomically(kGameConfig, out.str());
        return launcherOk && gameOk;
    }

    bool HasPerformanceOverrides() override {
        std::error_code ec;
        return fs::exists(kPerfCfg, ec);
    }

    bool ResetPerformanceTuning() override {
        std::error_code ec;
        if (!fs::exists(kPerfCfg, ec)) return true;
        // Keep the user's file as a backup instead of deleting it outright.
        const std::string backup = std::string(kPerfCfg) + ".bak";
        fs::remove(backup, ec);
        fs::rename(kPerfCfg, backup, ec);
        return !ec;
    }
};

SettingsService& Settings() {
    static SettingsServiceImpl instance;
    return instance;
}

}  // namespace mkw
