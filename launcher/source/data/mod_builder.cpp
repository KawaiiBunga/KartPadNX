// Injects My Stuff mod files into the game archives they belong in. See mod_builder.h.
#include "data/mod_builder.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>

#include <sys/stat.h>
#include <zlib.h>

#include "data/fsutil.h"

namespace mkw::modbuild {
namespace {

namespace fs = std::filesystem;

constexpr const char* kGeneratedDir = "sdmc:/switch/kartpad/mods-generated";
constexpr const char* kRetroRewindDir = "sdmc:/switch/kartpad/retro-rewind";
constexpr const char* kBuildVersion = "modbuild 2";

uint32_t Be16(const Bytes& b, size_t o) { return o + 2 <= b.size() ? (b[o] << 8) | b[o + 1] : 0; }
uint32_t Be32(const Bytes& b, size_t o) {
    return o + 4 <= b.size() ? (uint32_t(b[o]) << 24) | (b[o + 1] << 16) | (b[o + 2] << 8) | b[o + 3] : 0;
}
void Put32(Bytes& b, size_t o, uint32_t v) {
    if (o + 4 > b.size()) return;
    b[o] = uint8_t(v >> 24), b[o + 1] = uint8_t(v >> 16), b[o + 2] = uint8_t(v >> 8), b[o + 3] = uint8_t(v);
}
size_t Align32(size_t v) { return (v + 31) & ~size_t(31); }

std::string Lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}
std::string Upper(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}
bool EndsWith(const std::string& s, const char* tail) {
    const size_t n = std::strlen(tail);
    return s.size() >= n && s.compare(s.size() - n, n, tail) == 0;
}
std::string BaseName(const std::string& p) { return p.substr(p.find_last_of('/') + 1); }

Bytes ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
bool WriteFile(const std::string& path, const Bytes& data) {
    fsx::MakeDirs(fs::path(path).parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}
// Size and modification time: enough to notice a replaced file.
std::string Stamp(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return "missing";
    return std::to_string(static_cast<long long>(st.st_size)) + ":" + std::to_string(static_cast<long long>(st.st_mtime));
}

std::string XmlAttr(const std::string& attrs, const char* name) {
    const std::regex re(std::string("(?:^|\\s)") + name + "\\s*=\\s*\"([^\"]*)\"");
    std::smatch m;
    return std::regex_search(attrs, m, re) ? m[1].str() : std::string();
}

}  // namespace

// ---- Yaz0 ----

Bytes Yaz0Decode(const Bytes& in) {
    if (in.size() < 16 || std::memcmp(in.data(), "Yaz0", 4) != 0) return in;
    const uint32_t n = Be32(in, 4);
    Bytes out;
    out.reserve(n);
    size_t s = 16;
    while (out.size() < n && s < in.size()) {
        const uint8_t code = in[s++];
        for (int bit = 0; bit < 8 && out.size() < n; ++bit) {
            if (code & (0x80 >> bit)) {
                if (s >= in.size()) return {};
                out.push_back(in[s++]);
                continue;
            }
            if (s + 1 >= in.size()) return {};
            const uint8_t b1 = in[s], b2 = in[s + 1];
            s += 2;
            const size_t dist = (((b1 & 0xF) << 8) | b2) + 1;
            size_t count = b1 >> 4;
            if (count == 0) {
                if (s >= in.size()) return {};
                count = in[s++] + 0x12;
            } else {
                count += 2;
            }
            if (dist > out.size()) return {};
            for (size_t i = 0; i < count && out.size() < n; ++i) out.push_back(out[out.size() - dist]);
        }
    }
    return out.size() == n ? out : Bytes{};
}

// shortcut: stores every byte as a literal (valid Yaz0, 12.5% larger than the raw archive); add
// real LZ matching if load times or SD space ever matter.
Bytes Yaz0Encode(const Bytes& raw) {
    Bytes out(16, 0);
    std::memcpy(out.data(), "Yaz0", 4);
    Put32(out, 4, static_cast<uint32_t>(raw.size()));
    out.reserve(16 + raw.size() + raw.size() / 8 + 1);
    for (size_t i = 0; i < raw.size(); i += 8) {
        out.push_back(0xFF);
        out.insert(out.end(), raw.begin() + i, raw.begin() + std::min(raw.size(), i + 8));
    }
    return out;
}

// ---- U8 archives ----

bool U8Parse(const Bytes& d, U8Archive& out) {
    if (d.size() < 0x20 || Be32(d, 0) != 0x55AA382D) return false;
    const uint64_t root = Be32(d, 4), hsize = Be32(d, 8);
    if (root + 12 > d.size()) return false;
    const uint32_t total = Be32(d, root + 8);
    const uint64_t strs = root + uint64_t(total) * 12, hend = root + hsize;
    if (total == 0 || strs > hend || hend > d.size()) return false;
    out.nodes.assign(total, {});
    out.strings.assign(d.begin() + strs, d.begin() + hend);
    std::vector<std::pair<uint32_t, std::string>> dirs{{total, ""}};
    for (uint32_t i = 0; i < total; ++i) {
        while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
        auto& n = out.nodes[i];
        const size_t e = root + size_t(i) * 12;
        n.type = d[e];
        n.nameOffset = (uint32_t(d[e + 1]) << 16) | (d[e + 2] << 8) | d[e + 3];
        n.a = Be32(d, e + 4);
        n.b = Be32(d, e + 8);
        if (n.nameOffset >= out.strings.size()) return false;
        const char* s = reinterpret_cast<const char*>(out.strings.data()) + n.nameOffset;
        const std::string name(s, strnlen(s, out.strings.size() - n.nameOffset));
        if (i == 0) continue;
        if (n.type == 1) {
            if (n.b <= i || n.b > total) return false;
            n.path = dirs.back().second + name + "/";
            dirs.push_back({n.b, n.path});
        } else {
            if (uint64_t(n.a) + n.b > d.size()) return false;
            n.path = dirs.back().second + name;
            n.data.assign(d.begin() + n.a, d.begin() + n.a + n.b);
        }
    }
    return true;
}

U8Archive U8Build(const std::vector<std::pair<std::string, Bytes>>& files) {
    struct Entry {
        std::string name;
        int dir = -1;  // index into dirs, or -1 for a file
        size_t file = 0;
    };
    std::vector<std::vector<Entry>> dirs(1);
    for (size_t i = 0; i < files.size(); ++i) {
        std::vector<std::string> parts;
        std::stringstream ss(files[i].first);
        for (std::string part; std::getline(ss, part, '/');)
            if (!part.empty()) parts.push_back(part);
        if (parts.empty()) continue;
        size_t cur = 0;
        for (size_t p = 0; p + 1 < parts.size(); ++p) {
            auto& list = dirs[cur];
            const auto it = std::find_if(list.begin(), list.end(), [&](const Entry& e) { return e.dir >= 0 && e.name == parts[p]; });
            if (it != list.end()) {
                cur = static_cast<size_t>(it->dir);
                continue;
            }
            dirs.emplace_back();
            dirs[cur].push_back({parts[p], static_cast<int>(dirs.size() - 1), 0});
            cur = dirs.size() - 1;
        }
        dirs[cur].push_back({parts.back(), -1, i});
    }
    U8Archive a;
    a.strings = {0};
    a.nodes.push_back({1, 0, 0, 0, "", {}});
    auto name = [&](const std::string& n) {
        const uint32_t off = static_cast<uint32_t>(a.strings.size());
        a.strings.insert(a.strings.end(), n.begin(), n.end());
        a.strings.push_back(0);
        return off;
    };
    std::function<void(size_t, uint32_t, const std::string&)> emit = [&](size_t dir, uint32_t parent, const std::string& prefix) {
        for (const auto& e : dirs[dir]) {
            if (e.dir < 0) {
                a.nodes.push_back({0, name(e.name), 0, 0, prefix + e.name, files[e.file].second});
                continue;
            }
            const size_t idx = a.nodes.size();
            a.nodes.push_back({1, name(e.name), parent, 0, prefix + e.name + "/", {}});
            emit(static_cast<size_t>(e.dir), static_cast<uint32_t>(idx), prefix + e.name + "/");
            a.nodes[idx].b = static_cast<uint32_t>(a.nodes.size());
        }
    };
    emit(0, 0, "");
    a.nodes[0].b = static_cast<uint32_t>(a.nodes.size());
    return a;
}

Bytes U8Write(const U8Archive& a) {
    const uint32_t total = static_cast<uint32_t>(a.nodes.size());
    const uint32_t hsize = total * 12 + static_cast<uint32_t>(a.strings.size());
    const size_t dataStart = Align32(0x20 + hsize);
    Bytes out(dataStart, 0);
    Put32(out, 0, 0x55AA382D);
    Put32(out, 4, 0x20);
    Put32(out, 8, hsize);
    Put32(out, 12, static_cast<uint32_t>(dataStart));
    std::copy(a.strings.begin(), a.strings.end(), out.begin() + 0x20 + total * 12);
    for (uint32_t i = 0; i < total; ++i) {
        const auto& n = a.nodes[i];
        uint32_t va = n.a, vb = n.b;
        if (n.type != 1 && i != 0) {
            out.resize(Align32(out.size()), 0);
            va = static_cast<uint32_t>(out.size());
            vb = static_cast<uint32_t>(n.data.size());
            out.insert(out.end(), n.data.begin(), n.data.end());
        }
        const size_t e = 0x20 + size_t(i) * 12;
        out[e] = n.type;
        out[e + 1] = uint8_t(n.nameOffset >> 16), out[e + 2] = uint8_t(n.nameOffset >> 8), out[e + 3] = uint8_t(n.nameOffset);
        Put32(out, e + 4, va);
        Put32(out, e + 8, vb);
    }
    out.resize(Align32(out.size()), 0);
    return out;
}

// ---- Icons ----

bool PngDecode(const Bytes& p, Bytes& rgba, int& w, int& h) {
    static const uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (p.size() < 8 || std::memcmp(p.data(), kSig, 8) != 0) return false;
    int depth = 0, ctype = -1, interlace = 0;
    w = h = 0;
    Bytes idat, plte, trns;
    for (size_t o = 8; o + 12 <= p.size();) {
        const uint32_t len = Be32(p, o);
        if (o + 12 + uint64_t(len) > p.size()) return false;
        const std::string type(reinterpret_cast<const char*>(&p[o + 4]), 4);
        const uint8_t* c = &p[o + 8];
        if (type == "IHDR" && len >= 13) {
            w = static_cast<int>(Be32(p, o + 8)), h = static_cast<int>(Be32(p, o + 12));
            depth = c[8], ctype = c[9], interlace = c[12];
        } else if (type == "PLTE") {
            plte.assign(c, c + len);
        } else if (type == "tRNS") {
            trns.assign(c, c + len);
        } else if (type == "IDAT") {
            idat.insert(idat.end(), c, c + len);
        } else if (type == "IEND") {
            break;
        }
        o += 12 + len;
    }
    const int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
    if (depth != 8 || ch == 0 || interlace != 0 || w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    const size_t stride = size_t(w) * ch;
    Bytes raw((stride + 1) * h);
    uLongf rawLen = static_cast<uLongf>(raw.size());
    if (uncompress(raw.data(), &rawLen, idat.data(), static_cast<uLong>(idat.size())) != Z_OK || rawLen != raw.size())
        return false;
    Bytes cur(stride), prev(stride, 0);
    rgba.assign(size_t(w) * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        const uint8_t filter = raw[y * (stride + 1)];
        const uint8_t* src = &raw[y * (stride + 1) + 1];
        for (size_t x = 0; x < stride; ++x) {
            const int a = x >= size_t(ch) ? cur[x - ch] : 0, b = prev[x], c = x >= size_t(ch) ? prev[x - ch] : 0;
            int v = src[x];
            switch (filter) {
                case 0: break;
                case 1: v += a; break;
                case 2: v += b; break;
                case 3: v += (a + b) / 2; break;
                case 4: {
                    const int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c);
                    v += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
                    break;
                }
                default: return false;
            }
            cur[x] = uint8_t(v);
        }
        for (int x = 0; x < w; ++x) {
            uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
            const uint8_t* s = &cur[size_t(x) * ch];
            switch (ctype) {
                case 0: o[0] = o[1] = o[2] = s[0]; break;
                case 2: o[0] = s[0], o[1] = s[1], o[2] = s[2]; break;
                case 3: {
                    const size_t i = s[0];
                    if (i * 3 + 2 >= plte.size()) return false;
                    o[0] = plte[i * 3], o[1] = plte[i * 3 + 1], o[2] = plte[i * 3 + 2];
                    if (i < trns.size()) o[3] = trns[i];
                    break;
                }
                case 4: o[0] = o[1] = o[2] = s[0], o[3] = s[1]; break;
                default: std::memcpy(o, s, 4);
            }
        }
        std::swap(cur, prev);
    }
    return true;
}

bool TplReplace(const Bytes& tpl, const Bytes& rgba, int w, int h, Bytes& out) {
    if (Be32(tpl, 0) != 0x0020AF30 || Be32(tpl, 4) < 1 || w <= 0 || h <= 0 || rgba.size() < size_t(w) * h * 4)
        return false;
    const uint32_t table = Be32(tpl, 8), hdr = Be32(tpl, table);
    const uint32_t th = Be16(tpl, hdr), tw = Be16(tpl, hdr + 2), dataOff = Be32(tpl, hdr + 8);
    if (table + 8 > tpl.size() || hdr + 0x24 > tpl.size() || tw == 0 || th == 0 || tw > 1024 || th > 1024 ||
        dataOff < hdr + 0x24 || dataOff < table + 8 || dataOff > tpl.size())
        return false;
    out.assign(tpl.begin(), tpl.begin() + dataOff);
    Put32(out, 4, 1);          // first image only
    Put32(out, table + 4, 0);  // RGB5A3 has no palette
    Put32(out, hdr + 4, 5);    // RGB5A3
    out[hdr + 0x21] = 0, out[hdr + 0x22] = 0;  // min/max LOD: no mipmaps
    const uint32_t bw = (tw + 3) / 4, bh = (th + 3) / 4;
    out.resize(dataOff + size_t(bw) * bh * 32, 0);
    size_t o = dataOff;
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx)
            for (uint32_t y = 0; y < 4; ++y)
                for (uint32_t x = 0; x < 4; ++x, o += 2) {
                    const uint32_t px = bx * 4 + x, py = by * 4 + y;
                    if (px >= tw || py >= th) continue;
                    const uint8_t* s = &rgba[(size_t(py * h / th) * w + px * w / tw) * 4];
                    const uint16_t v = s[3] >= 0xE0 ? uint16_t(0x8000 | (s[0] >> 3) << 10 | (s[1] >> 3) << 5 | s[2] >> 3)
                                                    : uint16_t((s[3] >> 5) << 12 | (s[0] >> 4) << 8 | (s[1] >> 4) << 4 | s[2] >> 4);
                    out[o] = uint8_t(v >> 8), out[o + 1] = uint8_t(v);
                }
    return true;
}

// ---- The sound bank (BRSAR) ----

int PatchSoundArchive(const std::string& base, const std::string& outPath, const std::vector<SoundFile>& files,
                      std::string& log) {
    std::FILE* in = std::fopen(base.c_str(), "rb");
    if (!in) {
        log += "sound bank: can't open " + base + "\n";
        return 0;
    }
    auto readAt = [&](uint64_t off, uint64_t n, Bytes& b) {
        b.assign(n, 0);
        return n == 0 || (std::fseek(in, static_cast<long>(off), SEEK_SET) == 0 && std::fread(b.data(), 1, n, in) == n);
    };
    Bytes h, symb, info;
    if (!readAt(0, 0x28, h) || std::memcmp(h.data(), "RSAR", 4) != 0 || !readAt(Be32(h, 0x10), Be32(h, 0x14), symb) ||
        !readAt(Be32(h, 0x18), Be32(h, 0x1C), info)) {
        std::fclose(in);
        log += "sound bank: not a sound archive\n";
        return 0;
    }
    const uint32_t infoOff = Be32(h, 0x18), fileOff = Be32(h, 0x20);
    const uint32_t strtab = 8 + Be32(symb, 8);
    auto symName = [&](uint32_t id) -> std::string {
        if (id >= Be32(symb, strtab)) return {};
        const size_t o = 8 + size_t(Be32(symb, strtab + 4 + 4 * size_t(id)));
        if (o >= symb.size()) return {};
        const char* s = reinterpret_cast<const char*>(symb.data()) + o;
        return std::string(s, strnlen(s, symb.size() - o));
    };
    constexpr uint32_t B = 8;  // INFO references are relative to the section body
    auto table = [&](uint32_t o) {
        std::vector<uint32_t> out;
        const uint32_t n = Be32(info, o);
        for (uint32_t i = 0; i < n && o + 4 + 8 * size_t(i) + 8 <= info.size(); ++i) out.push_back(B + Be32(info, o + 8 + 8 * i));
        return out;
    };
    const auto fileTable = table(B + Be32(info, B + 0x18 + 4));
    const auto groupTable = table(B + Be32(info, B + 0x20 + 4));
    auto items = [&](uint32_t g) { return table(B + Be32(info, g + 0x20 + 4)); };

    std::map<uint32_t, std::pair<Bytes, Bytes>> repl;  // file index -> header file, wave archive
    for (const auto& f : files) {
        const uint32_t hlen = Be32(f.data, 8);
        if (f.data.size() < 16 || hlen < 16 || hlen > f.data.size()) {
            log += "sound: " + f.group + " isn't an exported sound file\n";
            continue;
        }
        long chosen = -1;
        for (uint32_t g : groupTable) {
            if (Upper(symName(Be32(info, g))) != Upper(f.group)) continue;
            for (uint32_t it : items(g)) {
                const uint32_t fid = Be32(info, it);
                Bytes magic;
                if (!readAt(uint64_t(Be32(info, g + 0x10)) + Be32(info, it + 4), 4, magic) ||
                    std::memcmp(magic.data(), f.data.data(), 4) != 0)
                    continue;
                if (chosen < 0 || long(fid) == f.index) chosen = fid;
            }
        }
        if (chosen < 0 || size_t(chosen) >= fileTable.size()) {
            log += "sound: no " + std::string(reinterpret_cast<const char*>(f.data.data()), 4) + " file in group " + f.group + "\n";
            continue;
        }
        repl[uint32_t(chosen)] = {Bytes(f.data.begin(), f.data.begin() + hlen), Bytes(f.data.begin() + hlen, f.data.end())};
        log += "sound: " + f.group + " file " + std::to_string(chosen) + " replaced\n";
    }
    if (repl.empty()) {
        std::fclose(in);
        return 0;
    }

    std::fseek(in, 0, SEEK_END);
    uint64_t end = static_cast<uint64_t>(std::ftell(in));
    std::fclose(in);
    in = nullptr;
    const std::string part = outPath + ".part";
    fsx::MakeDirs(fs::path(outPath).parent_path());
    if (fsx::CopyFile(base, part)) {
        log += "sound bank: copy failed\n";
        return 0;
    }
    in = std::fopen(base.c_str(), "rb");
    std::FILE* out = std::fopen(part.c_str(), "r+b");
    bool ok = in && out;
    auto append = [&](const Bytes& b) {
        const uint64_t at = Align32(end);
        const Bytes pad(at - end, 0);
        ok = ok && std::fseek(out, static_cast<long>(end), SEEK_SET) == 0 && std::fwrite(pad.data(), 1, pad.size(), out) == pad.size() &&
             std::fwrite(b.data(), 1, b.size(), out) == b.size();
        end = at + b.size();
        return at;
    };
    for (uint32_t g : groupTable) {
        const auto list = items(g);
        if (std::none_of(list.begin(), list.end(), [&](uint32_t it) { return repl.count(Be32(info, it)); })) continue;
        Bytes hb, wb;
        for (uint32_t it : list) {
            const uint32_t fid = Be32(info, it);
            Bytes hd, wd;
            if (const auto r = repl.find(fid); r != repl.end()) {
                hd = r->second.first, wd = r->second.second;
            } else if (!readAt(uint64_t(Be32(info, g + 0x10)) + Be32(info, it + 4), Be32(info, it + 8), hd) ||
                       !readAt(uint64_t(Be32(info, g + 0x18)) + Be32(info, it + 12), Be32(info, it + 16), wd)) {
                ok = false;
            }
            hb.resize(Align32(hb.size()), 0);
            wb.resize(Align32(wb.size()), 0);
            Put32(info, it + 4, uint32_t(hb.size()));
            Put32(info, it + 8, uint32_t(hd.size()));
            Put32(info, it + 12, uint32_t(wb.size()));
            Put32(info, it + 16, uint32_t(wd.size()));
            hb.insert(hb.end(), hd.begin(), hd.end());
            wb.insert(wb.end(), wd.begin(), wd.end());
        }
        Put32(info, g + 0x10, uint32_t(append(hb)));
        Put32(info, g + 0x14, uint32_t(hb.size()));
        Put32(info, g + 0x18, uint32_t(append(wb)));
        Put32(info, g + 0x1C, uint32_t(wb.size()));
    }
    for (const auto& [fid, data] : repl) {
        Put32(info, fileTable[fid], uint32_t(data.first.size()));
        Put32(info, fileTable[fid] + 4, uint32_t(data.second.size()));
    }
    append({});  // pads the file to 32 bytes
    // The FILE section is last: it now runs to the new end.
    Bytes sizes(4);
    auto writeAt = [&](uint64_t off, const Bytes& b) {
        ok = ok && std::fseek(out, static_cast<long>(off), SEEK_SET) == 0 && std::fwrite(b.data(), 1, b.size(), out) == b.size();
    };
    writeAt(infoOff, info);
    Put32(sizes, 0, uint32_t(end));
    writeAt(8, sizes);
    Put32(sizes, 0, uint32_t(end - fileOff));
    writeAt(0x24, sizes);
    writeAt(uint64_t(fileOff) + 4, sizes);
    if (in) std::fclose(in);
    if (out) ok = std::fclose(out) == 0 && ok;
    std::error_code ec;
    if (ok) {
        fs::remove(outPath, ec);
        fs::rename(part, outPath, ec);
        ok = !ec;
    }
    if (!ok) {
        fs::remove(part, ec);
        log += "sound bank: writing " + outPath + " failed\n";
        return 0;
    }
    return static_cast<int>(repl.size());
}

// ---- The build ----

namespace {

// Character codes (kart file suffixes) and the names their icons use inside the menu archives.
const std::map<std::string, std::string>& IconNames() {
    static const std::map<std::string, std::string> names = {
        {"mr", "mario"}, {"lg", "luigi"}, {"pc", "peach"}, {"ds", "daisy"}, {"ys", "yoshi"}, {"kt", "kinopio"},
        {"ko", "kinopico"}, {"nk", "noko"}, {"bmr", "baby_mario"}, {"blg", "baby_luigi"}, {"bpc", "baby_peach"},
        {"bds", "baby_daisy"}, {"ka", "karon"}, {"kp", "koopa"}, {"jr", "koopa_jr"}, {"wr", "wario"},
        {"wl", "waluigi"}, {"dk", "donky"}, {"dd", "didy"}, {"ca", "catherine"}, {"fk", "fuky"},
        {"kk", "teresa"}, {"bk", "hone_koopa"}, {"rs", "roseta"}};
    return names;
}

bool Excluded(const std::string& lowerDisc) {
    // Tracks and kart models: big, and their member names are generic.
    for (const char* p : {"/race/course/", "/race/kart/", "/scene/model/kart/"})
        if (lowerDisc.rfind(p, 0) == 0) return true;
    return false;
}

struct Source {
    std::string disc, file;
};

struct ModFile {
    std::string path, rel, lowerName;
};

std::vector<ModFile> ListModFiles(const std::string& dir) {
    std::vector<ModFile> out;
    fsx::ForEachFile(dir, [&](const std::string& path, const std::string& rel) {
        if (Lower(rel).rfind("riivolution/", 0) != 0) out.push_back({path, rel, Lower(BaseName(path))});
    });
    return out;
}

// Archive member paths per source file, cached by size and time.
class MemberIndex {
public:
    explicit MemberIndex(std::string cache) : cache_(std::move(cache)) {
        std::ifstream in(cache_);
        for (std::string line; std::getline(in, line);) {
            std::istringstream ss(line);
            std::string file, stamp, members;
            if (!std::getline(ss, file, '\t') || !std::getline(ss, stamp, '\t')) continue;
            std::getline(ss, members);
            auto& e = entries_[file];
            e.stamp = stamp;
            std::istringstream ms(members);
            for (std::string m; std::getline(ms, m, '|');)
                if (!m.empty()) e.members.push_back(m);
        }
    }
    const std::vector<std::string>& Members(const std::string& file) {
        auto& e = entries_[file];
        const std::string stamp = Stamp(file);
        if (e.stamp != stamp) {
            e = {stamp, {}};
            U8Archive a;
            if (U8Parse(Yaz0Decode(ReadFile(file)), a))
                for (const auto& n : a.nodes)
                    if (n.type == 0 && !n.path.empty()) e.members.push_back(n.path);
            dirty_ = true;
        }
        return e.members;
    }
    void Save() {
        if (!dirty_) return;
        std::string text;
        for (const auto& [file, e] : entries_) {
            text += file + "\t" + e.stamp + "\t";
            for (const auto& m : e.members) text += m + "|";
            text += "\n";
        }
        WriteFile(cache_, Bytes(text.begin(), text.end()));
    }

private:
    struct Entry {
        std::string stamp;
        std::vector<std::string> members;
    };
    std::string cache_;
    std::map<std::string, Entry> entries_;
    bool dirty_ = false;
};

struct Patch {
    std::string file;  // mod file (for a member of a patch archive: that archive)
    bool png = false;
    size_t mod = 0;    // precedence rank: higher wins
    bool hinted = false;
    bool inlineData = false;  // `data` came out of a patch archive
    bool remove = false;      // a ".delete" entry: drop the member
    Bytes data;
};

// Wheel Wizard / Pulsar patch archives: "<anything>.<Archive>.szs" holds only the members it
// changes in <Archive>.szs ("revo_kart" = the sound bank). Returns the lower-case target tag.
std::string PatchArchiveTag(const std::string& lowerName) {
    if (!EndsWith(lowerName, ".szs")) return {};
    const std::string stem = lowerName.substr(0, lowerName.size() - 4);
    const size_t dot = stem.rfind('.');
    return dot == std::string::npos || dot == 0 || dot + 1 == stem.size() ? std::string() : stem.substr(dot + 1);
}

std::string MemberKey(std::string path) {
    while (path.rfind("./", 0) == 0) path.erase(0, 2);
    while (!path.empty() && path[0] == '/') path.erase(0, 1);
    return "./" + path;
}

// Folder names that say which archive a file is for: "Driver", "Award.szs", "Race_E.d".
std::string HintName(std::string part) {
    part = Lower(part);
    for (const char* tail : {".szs", ".d", "_szs", ".u8", ".arc"})
        if (EndsWith(part, tail)) part.resize(part.size() - std::strlen(tail));
    return part;
}
std::string ArchiveStem(const std::string& lowerDisc) {
    std::string stem = BaseName(lowerDisc);
    return stem.substr(0, stem.rfind('.'));
}

}  // namespace

Result Build(Variant v, const std::vector<std::string>& modDirs, const std::vector<std::string>& baseDirs) {
    Result result;
    const std::string root = std::string(kGeneratedDir) + "/" + VariantId(v);
    const std::string discDir = std::string(kGameDataDir) + "/files";
    const std::string rrPack = std::string(kRetroRewindDir) + "/RetroRewind6";

    // Lowest precedence first.
    std::vector<std::vector<ModFile>> mods;
    for (auto it = modDirs.rbegin(); it != modDirs.rend(); ++it) mods.push_back(ListModFiles(*it));
    if (mods.empty()) return result;

    // Archives the game loads, by disc path, and every file name the game already has.
    std::map<std::string, Source> sources;  // lower disc path ->
    std::set<std::string> known;
    std::error_code ec;
    fsx::ForEachFile(discDir, [&](const std::string& file, const std::string& rel) {
        const std::string disc = "/" + rel;
        known.insert(Lower(BaseName(file)));
        if (EndsWith(Lower(disc), ".szs") && !Excluded(Lower(disc))) sources[Lower(disc)] = {disc, file};
    });
    fsx::ForEachFile(rrPack, [&](const std::string& file, const std::string&) { known.insert(Lower(BaseName(file))); });
    if (v == Variant::RetroRewind) {
        // Retro Rewind's own archives replace the disc's (its XML's <file> and <folder> entries).
        std::string xml;
        for (fs::directory_iterator it(rrPack + "/xml", ec), end; !ec && it != end; it.increment(ec))
            if (EndsWith(Lower(it->path().string()), ".xml")) {
                const Bytes b = ReadFile(it->path().generic_string());
                xml.assign(b.begin(), b.end());
                break;
            }
        static const std::regex tag("<(file|folder)\\s([^>]*)>");
        for (std::sregex_iterator t(xml.begin(), xml.end(), tag), end; t != end; ++t) {
            const std::string attrs = (*t)[2], disc = XmlAttr(attrs, "disc"), external = XmlAttr(attrs, "external");
            if (disc.empty() || external.empty() || disc[0] != '/' || external[0] != '/') continue;
            if ((*t)[1] == "file") {
                if (EndsWith(Lower(disc), ".szs") && !Excluded(Lower(disc)))
                    sources[Lower(disc)] = {disc, kRetroRewindDir + external};
                continue;
            }
            const std::string base = disc == "/" ? "" : disc;
            for (fs::directory_iterator it(kRetroRewindDir + external, ec), end; !ec && it != end; it.increment(ec)) {
                const std::string name = it->path().filename().string(), d = base + "/" + name;
                if (it->is_regular_file(ec) && EndsWith(Lower(name), ".szs") && !Excluded(Lower(d)))
                    sources[Lower(d)] = {d, it->path().generic_string()};
            }
        }
    }

    // Sort each mod's files: sound-bank files, icons, and archive members.
    static const std::regex soundName("^(?:(\\d+)[ ._-]+)?(grp_[a-z0-9_-]+)\\.(brwsd|brbnk|brseq)$");
    static const std::regex kartCode("^(?:l[a-f]|ldf)_(?:kart|bike)[^-]*-([a-z]+)(?:_\\d)?\\.szs$");
    static const std::regex allkartCode("^([a-z]+)-allkart[^.]*\\.szs$");
    static const std::regex modelCode("^([a-z]+)\\.brres$");
    std::map<std::string, SoundFile> sounds;          // key: group + magic
    std::map<std::string, std::string> soundFiles;    // same key -> mod file (for the stamp)
    std::map<std::string, std::map<std::string, Patch>> patches;  // lower disc -> member -> patch
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> byName;  // lower member name -> (disc, member)
    bool indexed = false;
    MemberIndex index(std::string(kGeneratedDir) + "/archive-index.txt");
    auto indexAll = [&] {
        if (indexed) return;
        indexed = true;
        for (const auto& [lower, src] : sources)
            for (const auto& m : index.Members(src.file)) byName[Lower(BaseName(m))].push_back({lower, m});
        index.Save();
    };
    auto isTag = [](const std::string& disc, const std::string& tag) { return tag.empty() || BaseName(disc) == tag + ".szs"; };
    auto hasArchive = [&](const std::string& tag) {
        return std::any_of(sources.begin(), sources.end(), [&](const auto& s) { return isTag(s.first, tag); });
    };
    // `onlyTag`: the patch names its archive ("<member>.<Archive>").
    auto addPatch = [&](const std::string& lowerMember, const ModFile& f, size_t rank, bool png,
                        const std::string& onlyTag = std::string()) {
        indexAll();
        std::vector<std::pair<std::string, std::string>> candidates;  // (disc, member path)
        if (const auto found = byName.find(lowerMember); found != byName.end())
            for (const auto& c : found->second)
                if (isTag(c.first, onlyTag)) candidates.push_back(c);
        if (candidates.empty() && !onlyTag.empty()) {  // not in the named archive yet: add it at the top
            for (const auto& [disc, src] : sources)
                if (isTag(disc, onlyTag)) patches[disc]["./" + BaseName(f.path).substr(0, lowerMember.size())] = {f.path, false, rank};
            return true;
        }
        if (candidates.empty()) return false;
        std::set<std::string> hints;
        for (const auto& part : fs::path(f.rel).parent_path()) hints.insert(HintName(part.string()));
        static const std::regex lang("_[a-z]{1,2}$");
        auto hinted = [&](const std::string& disc) {
            const std::string stem = ArchiveStem(disc);
            return hints.count(stem) || hints.count(std::regex_replace(stem, lang, ""));
        };
        const bool anyHint = std::any_of(candidates.begin(), candidates.end(), [&](const auto& c) { return hinted(c.first); });
        for (const auto& [disc, member] : candidates) {
            if (anyHint && !hinted(disc)) continue;
            auto& slot = patches[disc][member];
            if (!slot.file.empty() && slot.mod == rank && slot.hinted && !anyHint) continue;
            if (!slot.file.empty() && slot.mod == rank && !slot.png && png) continue;  // a mod's own .tpl beats its .png
            slot = {f.path, png, rank, anyHint};
        }
        return true;
    };
    for (size_t rank = 0; rank < mods.size(); ++rank) {
        std::string code;
        for (const auto& f : mods[rank]) {
            std::smatch m;
            if (std::regex_match(f.lowerName, m, kartCode) || std::regex_match(f.lowerName, m, allkartCode) ||
                std::regex_match(f.lowerName, m, modelCode)) {
                if (IconNames().count(m[1].str())) {
                    code = m[1].str();
                    break;
                }
            }
        }
        for (const auto& f : mods[rank]) {
            if (known.count(f.lowerName) || EndsWith(f.lowerName, ".ini")) continue;  // by name already; Wheel Wizard's mod info
            std::smatch m;
            if (const std::string tag = PatchArchiveTag(f.lowerName); !tag.empty() && (tag == "revo_kart" || hasArchive(tag))) {
                U8Archive pa;
                if (!U8Parse(Yaz0Decode(ReadFile(f.path)), pa)) continue;
                for (const auto& n : pa.nodes) {
                    if (n.type != 0 || n.path.empty()) continue;
                    const std::string base = Lower(BaseName(n.path));
                    if (tag == "revo_kart") {
                        if (!std::regex_match(base, m, soundName)) continue;
                        SoundFile s{Upper(m[2].str()), m[1].matched ? std::atol(m[1].str().c_str()) : -1, n.data};
                        const std::string key = s.group + "." + m[3].str();
                        sounds[key] = std::move(s);
                        soundFiles[key] = f.path;
                        continue;
                    }
                    std::string key = MemberKey(n.path);
                    const bool remove = EndsWith(Lower(key), ".delete");
                    if (remove) key.resize(key.size() - 7);
                    for (const auto& [disc, src] : sources)
                        if (isTag(disc, tag)) {
                            Patch p{f.path, false, rank};
                            p.inlineData = true, p.remove = remove, p.data = n.data;
                            patches[disc][key] = std::move(p);
                        }
                }
                continue;
            }
            if (const size_t dot = f.lowerName.rfind('.'); dot != std::string::npos && dot > 0 &&
                f.lowerName.find('.') < dot && hasArchive(f.lowerName.substr(dot + 1))) {
                addPatch(f.lowerName.substr(0, dot), f, rank, false, f.lowerName.substr(dot + 1));  // "<member>.<Archive>"
                continue;
            }
            if (std::regex_match(f.lowerName, m, soundName)) {
                SoundFile s{Upper(m[2].str()), m[1].matched ? std::atol(m[1].str().c_str()) : -1, {}};
                const std::string key = s.group + "." + m[3].str();
                sounds[key] = std::move(s);
                soundFiles[key] = f.path;
            } else if (EndsWith(f.lowerName, ".png")) {
                if (code.empty()) continue;
                const std::string name = IconNames().at(code);
                const bool big = f.lowerName.find("64") != std::string::npos || f.lowerName.find("css") != std::string::npos;
                const bool small = f.lowerName.find("32") != std::string::npos || f.lowerName.find("map") != std::string::npos;
                if (big != small) addPatch(big ? "tt_" + name + "_64x64.tpl" : "st_" + name + "_32x32.tpl", f, rank, true);
            } else {
                addPatch(f.lowerName, f, rank, false);
            }
        }
    }

    // The archive each patched file starts from: a mod's whole replacement, else the game's.
    std::map<std::string, std::string> bases;
    for (const auto& [disc, members] : patches) {
        std::string base = sources[disc].file;
        for (auto it = baseDirs.rbegin(); it != baseDirs.rend(); ++it)  // disc-layout mods, by path
            if (fs::is_regular_file(*it + sources[disc].disc, ec)) base = *it + sources[disc].disc;
        for (const auto& mod : mods)
            for (const auto& f : mod)
                if (f.lowerName == BaseName(disc)) base = f.path;
        bases[disc] = base;
    }
    std::string soundBase = discDir + "/sound/revo_kart.brsar";
    for (auto it = baseDirs.rbegin(); it != baseDirs.rend(); ++it)
        if (fs::is_regular_file(*it + "/sound/revo_kart.brsar", ec)) soundBase = *it + "/sound/revo_kart.brsar";
    for (const auto& mod : mods)
        for (const auto& f : mod)
            if (f.lowerName == "revo_kart.brsar") soundBase = f.path;

    // Nothing changed since the last build: reuse it.
    std::string sig = std::string(kBuildVersion) + "\n";
    for (const auto& [disc, members] : patches) {
        sig += sources[disc].disc + " <- " + bases[disc] + " " + Stamp(bases[disc]) + "\n";
        for (const auto& [member, p] : members) sig += "  " + member + " <- " + p.file + " " + Stamp(p.file) + "\n";
    }
    if (!sounds.empty()) {
        sig += "sound <- " + soundBase + " " + Stamp(soundBase) + "\n";
        for (const auto& [key, path] : soundFiles) sig += "  " + key + " <- " + path + " " + Stamp(path) + "\n";
    }
    if (patches.empty() && sounds.empty()) return result;
    const std::string sigPath = root + "/inputs.txt";
    {
        const Bytes old = ReadFile(sigPath);
        std::ifstream filesIn(root + "/files.txt");
        std::vector<std::string> previous;
        for (std::string line; std::getline(filesIn, line);)
            if (!line.empty()) previous.push_back(line);
        if (std::string(old.begin(), old.end()) == sig && !previous.empty()) {
            result.root = root;
            result.files = previous;
            return result;
        }
    }

    fs::remove_all(root, ec);  // ours alone: only ever holds what this function wrote
    std::string log;
    for (const auto& [disc, members] : patches) {
        U8Archive a;
        if (!U8Parse(Yaz0Decode(ReadFile(bases[disc])), a)) {
            log += sources[disc].disc + ": can't read " + bases[disc] + "\n";
            continue;
        }
        std::map<std::string, size_t> byPath;  // lower member path -> node
        for (size_t i = 0; i < a.nodes.size(); ++i)
            if (a.nodes[i].type == 0 && i != 0) byPath[Lower(a.nodes[i].path)] = i;
        std::set<size_t> removed;
        std::vector<std::pair<std::string, Bytes>> added;
        int changed = 0;
        for (const auto& [member, p] : members) {
            const auto at = byPath.find(Lower(member));
            const std::string where = sources[disc].disc + " " + member;
            if (p.remove) {
                if (at != byPath.end()) removed.insert(at->second), ++changed, log += where + " removed by " + p.file + "\n";
                continue;
            }
            Bytes data = p.inlineData ? p.data : ReadFile(p.file);
            if (p.png) {
                Bytes rgba, tpl;
                int w = 0, h = 0;
                if (at == byPath.end() || !PngDecode(data, rgba, w, h) || !TplReplace(a.nodes[at->second].data, rgba, w, h, tpl)) {
                    log += where + ": can't convert " + p.file + "\n";
                    continue;
                }
                data = std::move(tpl);
            }
            if (at == byPath.end()) {
                added.push_back({member, std::move(data)});
                log += where + " added from " + p.file + "\n";
            } else {
                a.nodes[at->second].data = std::move(data);
                log += where + " <- " + p.file + "\n";
            }
            ++changed;
        }
        if (!removed.empty() || !added.empty()) {  // the tree changes: lay the archive out again
            std::vector<std::pair<std::string, Bytes>> files;
            for (size_t i = 1; i < a.nodes.size(); ++i)
                if (a.nodes[i].type == 0 && !removed.count(i)) files.push_back({a.nodes[i].path, std::move(a.nodes[i].data)});
            for (auto& f : added) files.push_back(std::move(f));
            a = U8Build(files);
        }
        if (changed && WriteFile(root + sources[disc].disc, Yaz0Encode(U8Write(a)))) result.files.push_back(sources[disc].disc);
    }
    if (!sounds.empty()) {
        std::vector<SoundFile> list;
        for (auto& [key, s] : sounds) {
            if (s.data.empty()) s.data = ReadFile(soundFiles[key]);  // patch-archive members come filled in
            list.push_back(std::move(s));
        }
        if (PatchSoundArchive(soundBase, root + "/sound/revo_kart.brsar", list, log) > 0)
            result.files.push_back("/sound/revo_kart.brsar");
    }
    WriteFile(root + "/build-log.txt", Bytes(log.begin(), log.end()));
    if (result.files.empty()) return result;
    std::string filesText;
    for (const auto& f : result.files) filesText += f + "\n";
    WriteFile(root + "/files.txt", Bytes(filesText.begin(), filesText.end()));
    WriteFile(sigPath, Bytes(sig.begin(), sig.end()));
    result.root = root;
    return result;
}

}  // namespace mkw::modbuild
