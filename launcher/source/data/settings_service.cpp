// Launcher and game settings files. Both are edited in place: unknown JSON keys and every
// other Config.toml line are kept, and each file is replaced through a temporary file.
#include "mkw/data.h"

#include "data/fsutil.h"
#include "data/mod_builder.h"
#include "data/zip_extract.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
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

// A TOML basic string.
std::string Quote(const std::string& s) {
    std::string q = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') q += '\\';
        q += c;
    }
    return q + '"';
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

// ---- Riivolution mods: their options, like the Riivolution menu --------------------------------
// A small tag scanner, enough for Riivolution XMLs (no DTDs or CDATA). Attribute values are kept
// as written, entities included, so ids match what the game's own parser reads.

std::string Attr(const std::string& attrs, const char* name) {
    const std::regex re(std::string("(?:^|\\s)") + name + "\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)')");
    std::smatch m;
    if (!std::regex_search(attrs, m, re)) return {};
    return m[1].matched ? m[1].str() : m[2].str();
}

// Riivolution path rules: '/' starts at the SD root (the mod folder), anything else is relative.
std::string ResolvePath(const std::string& sdRoot, const std::string& base, const std::string& rel) {
    if (rel.empty()) return base;
    return rel[0] == '/' ? sdRoot + rel : base + "/" + rel;
}

// Game code: a replaced main.dol/StaticR.rel, or Pulsar's .pul modules (Retro Rewind, Item Fever).
bool HasCodeFile(const std::string& path) {
    auto isCode = [](const std::string& p) {
        std::string e = p.substr(p.find_last_of('.') == std::string::npos ? p.size() : p.find_last_of('.'));
        for (auto& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return e == ".dol" || e == ".rel" || e == ".pul";
    };
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) return isCode(path);
    bool found = false;
    fsx::ForEachFile(path, [&](const std::string& file, const std::string&) { found = found || isCode(file); });
    return found;
}

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string ConfigPath(const std::string& modDir) { return modDir + "/riivolution/config/RMCP.xml"; }

// A mod with one of the disc's top-level folders at its top is laid out like the disc; the game
// maps those files by path without any XML.
bool DiscShaped(const std::string& modDir) {
    static const char* const kDiscTop[] = {"boot", "contents", "demo", "hbm", "race", "rel", "scene", "sound", "thp"};
    std::error_code ec;
    for (fs::directory_iterator it(modDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string name = it->path().filename().string();
        for (auto& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        for (const char* top : kDiscTop) {
            if (name == top) return true;
        }
    }
    return false;
}

std::string Lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// File names the game can already load: the extracted disc and the Retro Rewind pack (whose
// Character/Assets folders add names of their own).
void CollectNames(const std::string& dir, std::vector<std::string>& names) {
    fsx::ForEachFile(dir, [&](const std::string& path, const std::string&) {
        names.push_back(Lower(path.substr(path.find_last_of('/') + 1)));
    });
}

// Where Retro Rewind's Character folder puts a new character file (its RetroRewind6.xml), for a
// My Stuff file that replaces nothing: extra skin slots such as "fk-8" are new names. Unnumbered
// models and voices ("wl.brres") belong inside archives instead (mod_builder).
std::string CharacterSlotPath(const std::string& lowerName) {
    static const std::regex kart("^(l[a-f]|ldf)_(kart|bike)[^/]*\\.szs$");
    static const std::regex model("^[a-z]+-\\d+\\.brres$");
    static const std::regex voice("^grp_vo_[a-z]+-\\d+_.*");
    if (std::regex_match(lowerName, kart)) return "/Race/Kart/";
    if (lowerName.find("-allkart") != std::string::npos && lowerName.size() > 4 &&
        lowerName.compare(lowerName.size() - 4, 4, ".szs") == 0)
        return "/Scene/Model/Kart/";
    if (std::regex_match(lowerName, model)) return "/Scene/Model/Driver/";
    if (std::regex_match(lowerName, voice)) return "/sound/";
    return "";
}

std::string XmlEscape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += ch;
        }
    }
    return out;
}

// ---- My Stuff mods: loose files dropped in as they come -----------------------------------------
// Nothing is written into these folders. One launcher-owned XML at the kartpad folder describes
// them all (the runtime resolves externals inside the root that holds the XML, so it sits above
// mods/ and mods-generated/).

constexpr const char* kKartpadDir = "sdmc:/switch/kartpad";
constexpr const char* kModsXml = "sdmc:/switch/kartpad/riivolution/KartPadNX Mods.xml";
constexpr const char* kOldMyStuffXml = "KartPadNX My Stuff.xml";  // earlier builds wrote this into each mod

// A Riivolution pack brings its own XML (an earlier build's My Stuff XML doesn't count).
bool IsPack(const std::string& modDir) {
    std::error_code ec;
    for (fs::directory_iterator it(modDir + "/riivolution", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_regular_file(ec) && Lower(it->path().extension().string()) == ".xml" && name != kOldMyStuffXml) return true;
    }
    return false;
}

// Removes the riivolution/ folder an earlier build put into a My Stuff mod (only our files).
void RemoveOldMyStuff(const std::string& modDir) {
    std::error_code ec;
    const std::string dir = modDir + "/riivolution";
    if (!fs::exists(dir + "/" + kOldMyStuffXml, ec) || IsPack(modDir)) return;
    fs::remove(dir + "/" + kOldMyStuffXml, ec);
    fs::remove(dir + "/config/RMCP.xml", ec);
    fs::remove(dir + "/config", ec);  // these only remove empty folders
    fs::remove(dir, ec);
}

// The mod as a Riivolution patch: its whole folder replaces game files by name (a <folder>
// without a disc path), and files that replace nothing but look like character files (Retro
// Rewind skin slots) are added where Retro Rewind's Character folder puts them.
std::string MyStuffPatch(const std::string& id, const std::string& name, const std::vector<std::string>& known) {
    const std::string modDir = std::string(kModsDir) + "/" + name;
    const std::string ext = "/mods/" + XmlEscape(name);
    std::string patch = "  <patch id=\"" + id + "\">\n    <folder external=\"" + ext + "\" recursive=\"true\" />\n";
    fsx::ForEachFile(modDir, [&](const std::string& path, const std::string& rel) {
        const std::string file = path.substr(path.find_last_of('/') + 1);
        if (std::binary_search(known.begin(), known.end(), Lower(file))) return;
        const std::string disc = CharacterSlotPath(Lower(file));
        if (disc.empty()) return;
        patch += "    <file disc=\"" + XmlEscape(disc + file) + "\" external=\"" + ext + "/" + XmlEscape(rel) +
                 "\" create=\"true\" />\n";
    });
    return patch + "  </patch>\n";
}

std::vector<ModOption> ReadModOptions(const std::string& modDir) {
    std::vector<ModOption> options;
    const std::string xmlDir = modDir + "/riivolution";
    std::vector<fs::path> xmls;
    std::error_code ec;
    for (fs::directory_iterator it(xmlDir, ec), end; !ec && it != end; it.increment(ec)) {
        std::string ext = it->path().extension().string();
        for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (it->is_regular_file(ec) && ext == ".xml") xmls.push_back(it->path());
    }
    std::sort(xmls.begin(), xmls.end());
    const std::regex comment("<!--[\\s\\S]*?-->");
    const std::regex tag("<(/?)([A-Za-z]+)([^>]*)>");
    for (const auto& xml : xmls) {
        const std::string text = std::regex_replace(ReadText(xml), comment, "");
        std::string root, section, patchId, patchRoot;
        bool inOptions = false, inChoice = false;
        std::vector<std::vector<std::string>> choicePatches;  // per option: patch ids of all its choices
        std::map<std::string, std::vector<std::string>> patchPaths;
        std::set<std::string> loadsCode;  // patches with a <memory valuefile>: a code blob written into RAM
        size_t first = options.size();
        for (std::sregex_iterator t(text.begin(), text.end(), tag), end; t != end; ++t) {
            const bool close = (*t)[1].length() > 0;
            const std::string name = (*t)[2];
            const std::string attrs = (*t)[3];
            const bool selfClose = !attrs.empty() && attrs.back() == '/';
            if (name == "wiidisc" && !close) {
                root = Attr(attrs, "root");
            } else if (name == "options") {
                inOptions = !close && !selfClose;
            } else if (name == "section" && !close) {
                section = Attr(attrs, "name");
            } else if (name == "option" && !close && inOptions) {
                ModOption o;
                o.section = section;
                o.name = Attr(attrs, "name");
                const std::string id = Attr(attrs, "id");
                o.id = id.empty() ? section + o.name : id;
                options.push_back(o);
                choicePatches.emplace_back();
            } else if (name == "choice" && inOptions) {
                inChoice = !close && !selfClose;
                if (!close && options.size() > first) options.back().choices.push_back(Attr(attrs, "name"));
            } else if (name == "patch") {
                if (inOptions) {
                    if (!close && inChoice && !choicePatches.empty()) choicePatches.back().push_back(Attr(attrs, "id"));
                } else if (!close) {
                    patchId = selfClose ? "" : Attr(attrs, "id");
                    const std::string r = Attr(attrs, "root");
                    patchRoot = ResolvePath(modDir, xmlDir, r.empty() ? root : r);
                } else {
                    patchId.clear();
                }
            } else if ((name == "folder" || name == "file") && !close && !patchId.empty()) {
                patchPaths[patchId].push_back(ResolvePath(modDir, patchRoot, Attr(attrs, "external")));
            } else if (name == "memory" && !close && !patchId.empty() && !Attr(attrs, "valuefile").empty()) {
                loadsCode.insert(patchId);
            }
        }
        for (size_t i = first; i < options.size(); ++i) {
            for (const auto& id : choicePatches[i - first]) {
                options[i].replacesCode = options[i].replacesCode || loadsCode.count(id) > 0;
                for (const auto& path : patchPaths[id]) options[i].replacesCode = options[i].replacesCode || HasCodeFile(path);
            }
        }
    }
    options.erase(std::remove_if(options.begin(), options.end(), [](const ModOption& o) { return o.choices.empty(); }),
                  options.end());

    // Saved choices (Riivolution/Dolphin config), else the drag-and-drop default.
    std::map<std::string, int> saved;
    bool haveConfig = false;
    if (fs::is_regular_file(ConfigPath(modDir), ec)) {
        haveConfig = true;
        const std::string text = ReadText(ConfigPath(modDir));
        for (std::sregex_iterator t(text.begin(), text.end(), tag), end; t != end; ++t) {
            if ((*t)[2] == "option" && !(*t)[1].length()) {
                const std::string attrs = (*t)[3];
                saved[Attr(attrs, "id")] = std::atoi(Attr(attrs, "default").c_str());
            }
        }
    }
    for (auto& o : options) {
        const auto it = saved.find(o.id);
        o.selected = haveConfig ? (it == saved.end() ? 0 : it->second) : (o.replacesCode ? 0 : 1);
        if (o.selected < 0 || o.selected > static_cast<int>(o.choices.size())) o.selected = 0;
    }
    return options;
}

bool WriteModOptions(const std::string& modDir, const std::vector<ModOption>& options) {
    std::string xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<riivolution version=\"2\">\n";
    for (const auto& o : options) {
        xml += "  <option id=\"" + o.id + "\" default=\"" + std::to_string(o.selected) + "\" />\n";
    }
    xml += "</riivolution>\n";
    return WriteAtomically(ConfigPath(modDir), xml);
}

// Mods made for the US game ship its language archives (Race_U.szs: U English, Q French, M Spanish);
// this European game reads Race_E/_F/_S. Map each to the European name the disc has, unless the
// mod brings that one too. `discShaped`: enabled disc-layout mods, highest precedence first.
std::string RegionAliasPatch(const std::vector<std::string>& discShaped) {
    static const std::regex us("^(.*_)([umq])(\\.szs)$", std::regex::icase);
    const std::string discFiles = kGameDataDir + std::string("/files");
    std::map<std::string, std::string> entries;  // European disc path -> external
    for (auto it = discShaped.rbegin(); it != discShaped.rend(); ++it) {
        const std::string modDir = std::string(kModsDir) + "/" + *it;
        fsx::ForEachFile(modDir, [&](const std::string&, const std::string& rel) {
            std::smatch m;
            if (!std::regex_match(rel, m, us)) return;
            const char eu = std::tolower(static_cast<unsigned char>(m[2].str()[0])) == 'u' ? 'E'
                            : std::tolower(static_cast<unsigned char>(m[2].str()[0])) == 'q' ? 'F' : 'S';
            const std::string pal = m[1].str() + eu + m[3].str();
            std::error_code ec;
            if (fs::exists(discFiles + "/" + pal, ec) && !fs::exists(modDir + "/" + pal, ec))
                entries["/" + pal] = "/mods/" + *it + "/" + rel;
        });
    }
    if (entries.empty()) return {};
    std::string patch = "  <patch id=\"regions\">\n";
    for (const auto& [disc, ext] : entries)
        patch += "    <file disc=\"" + XmlEscape(disc) + "\" external=\"" + XmlEscape(ext) + "\" />\n";
    return patch + "  </patch>\n";
}

// The XML for every enabled My Stuff mod (`names`, highest precedence first) plus the archives
// mod_builder patched and US language archives standing in for European ones. Later patches shadow
// earlier ones, so they go lowest precedence first and the patched archives last. Returns false
// when there is nothing to describe or it can't be written.
bool WriteModsXml(Variant v, const std::vector<std::string>& names, const std::vector<std::string>& discShaped) {
    std::string choice, patches = RegionAliasPatch(discShaped);
    if (!patches.empty()) choice += "<patch id=\"regions\" />";
    if (names.empty() && choice.empty()) return false;
    std::vector<std::string> known;
    if (!names.empty()) {
        CollectNames(kGameDataDir + std::string("/files"), known);
        CollectNames("sdmc:/switch/kartpad/retro-rewind/RetroRewind6", known);
        std::sort(known.begin(), known.end());
    }
    for (size_t i = names.size(); i-- > 0;) {
        const std::string id = "mod" + std::to_string(i);
        choice += "<patch id=\"" + id + "\" />";
        patches += MyStuffPatch(id, names[i], known);
    }
    std::vector<std::string> dirs, baseDirs;
    for (const auto& n : names) dirs.push_back(std::string(kModsDir) + "/" + n);
    for (const auto& n : discShaped) baseDirs.push_back(std::string(kModsDir) + "/" + n);
    const auto generated = names.empty() ? modbuild::Result{} : modbuild::Build(v, dirs, baseDirs);
    if (!generated.root.empty()) {
        // Some are Retro Rewind's own paths, which aren't on the disc: create them.
        const std::string ext = generated.root.substr(std::strlen(kKartpadDir));
        choice += "<patch id=\"archives\" />";
        patches += "  <patch id=\"archives\">\n";
        for (const auto& d : generated.files)
            patches += "    <file disc=\"" + XmlEscape(d) + "\" external=\"" + XmlEscape(ext + d) + "\" create=\"true\" />\n";
        patches += "  </patch>\n";
    }
    ModOption on;
    on.id = "KartPadNXMods";
    on.selected = 1;
    return WriteAtomically(kModsXml,
                           "<wiidisc version=\"1\">\n"
                           "  <id game=\"RMC\" />\n"
                           "  <options>\n"
                           "    <section name=\"KartPadNX\">\n"
                           "      <option name=\"Mods\" default=\"1\">\n"
                           "        <choice name=\"On\">" + choice + "</choice>\n"
                           "      </option>\n"
                           "    </section>\n"
                           "  </options>\n" +
                               patches +
                           "</wiidisc>\n") &&
           WriteModOptions(kKartpadDir, {on});
}

}  // namespace

class SettingsServiceImpl : public SettingsService {
public:
    void Load(LauncherSettings& s) override {
        std::ifstream in(mkw::kLauncherConfig);
        // A config from before known_mods: its mods were set up by hand, take them as seen.
        bool upgrading = false;
        if (in) {
            const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
            if (j.is_object()) {
                upgrading = !j.contains("known_mods");
                s.knownMods.clear();
                for (const auto& name : j.value("known_mods", nlohmann::json::array()))
                    if (name.is_string()) s.knownMods.push_back(name.get<std::string>());
                s.accent = j.value("accent", s.accent);
                s.sounds = j.value("sounds", s.sounds);
                s.updateChannel = j.value("update_channel", s.updateChannel);
                s.lastVariant = j.value("last_variant", s.lastVariant);
                s.backupBeforeLaunch = j.value("backup_before_launch", s.backupBeforeLaunch);
                if (j.contains("mods") && j["mods"].is_object()) {
                    for (int v = 0; v < 2; ++v) {
                        const auto& list = j["mods"].value(VariantId(static_cast<Variant>(v)), nlohmann::json::array());
                        s.mods[v].clear();
                        if (!list.is_array()) continue;
                        for (const auto& name : list) {
                            if (name.is_string()) s.mods[v].push_back(name.get<std::string>());
                        }
                    }
                }
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

        if (upgrading) s.knownMods = ListMods();
        if (PickUpNewMods(s) || upgrading) Save(s);
    }

    bool PickUpNewMods(LauncherSettings& s) override {
        // A mod dropped in as a .zip is unpacked next to it (the .zip is kept); one that can't be
        // gets a note instead of a retry on every start.
        std::error_code ec;
        for (fs::directory_iterator it(kModsDir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (Lower(it->path().extension().string()) != ".zip" || name.size() <= 4) continue;
            const std::string dest = std::string(kModsDir) + "/" + name.substr(0, name.size() - 4);
            const std::string note = std::string(kModsDir) + "/" + name + " - could not unpack.txt";
            std::error_code e;
            if (fs::exists(dest, e) || fs::exists(note, e)) continue;
            std::string error;
            if (!zipx::Extract(std::string(kModsDir) + "/" + name, dest, error))
                WriteAtomically(note, "KartPadNX could not unpack " + name + ": " + error +
                                          "\nUnzip it on a computer and copy the folder here instead.\n");
        }
        const auto present = ListMods();
        bool changed = false;
        for (const auto& name : present) {
            if (std::find(s.knownMods.begin(), s.knownMods.end(), name) != s.knownMods.end()) continue;
            for (auto& list : s.mods)
                if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
            changed = true;
        }
        // Forget deleted folders, so one added back counts as new.
        changed = changed || s.knownMods != present;
        s.knownMods = present;
        return changed;
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
        j["mods"] = {{VariantId(Variant::Vanilla), s.mods[0]}, {VariantId(Variant::RetroRewind), s.mods[1]}};
        j["known_mods"] = s.knownMods;
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

    std::vector<std::string> ListMods() override {
        std::vector<std::string> names;
        std::error_code ec;
        for (fs::directory_iterator it(kModsDir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec)) names.push_back(it->path().filename().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    bool ApplyMods(Variant v, const LauncherSettings& s) override {
        const auto present = ListMods();
        std::vector<std::string> dirs, myStuff, discShaped;
        for (const auto& name : s.mods[static_cast<int>(v)]) {
            if (std::find(present.begin(), present.end(), name) == present.end()) continue;
            const std::string dir = std::string(kModsDir) + "/" + name;
            RemoveOldMyStuff(dir);
            const bool pack = IsPack(dir);
            if (!pack && !DiscShaped(dir)) {
                myStuff.push_back(name);
                continue;
            }
            if (!pack) discShaped.push_back(name);
            // A Riivolution pack nobody has set up yet would load nothing (its options default to
            // off): save the default choices so it works as dropped in.
            std::error_code ec;
            if (!fs::exists(ConfigPath(dir), ec)) {
                const auto options = ReadModOptions(dir);
                if (!options.empty()) WriteModOptions(dir, options);
            }
            dirs.push_back(dir);
        }
        // Loose-file mods and the archives patched for them, above the packs.
        if (WriteModsXml(v, myStuff, discShaped)) dirs.insert(dirs.begin(), kKartpadDir);
        std::string roots = "[";
        for (const auto& dir : dirs) roots += (roots.size() > 1 ? ", " : "") + Quote(dir);
        roots += "]";
        TomlFile t{ReadLines(kGameConfig)};
        t.Set("paths", "overlay_roots", roots);
        std::ostringstream out;
        for (const auto& line : t.lines) out << line << '\n';
        return WriteAtomically(kGameConfig, out.str());
    }

    std::vector<ModOption> ModOptions(const std::string& mod) override {
        RemoveOldMyStuff(std::string(kModsDir) + "/" + mod);
        return ReadModOptions(std::string(kModsDir) + "/" + mod);
    }

    bool SaveModOptions(const std::string& mod, const std::vector<ModOption>& options) override {
        return options.empty() || WriteModOptions(std::string(kModsDir) + "/" + mod, options);
    }
};

SettingsService& Settings() {
    static SettingsServiceImpl instance;
    return instance;
}

}  // namespace mkw
