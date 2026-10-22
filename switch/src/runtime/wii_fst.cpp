#include "wii_fst.h"

#include <unordered_set>

namespace kartpad::disc {
namespace {

constexpr std::size_t kEntrySize = 12;

std::uint32_t be24(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
}
std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

// Read a NUL-terminated name from the string table with bounds checking.
bool nameAt(const std::uint8_t* strTab, std::size_t strLen, std::uint32_t off,
            std::string& out) {
    if (off >= strLen) return false;
    std::size_t end = off;
    while (end < strLen && strTab[end] != 0) ++end;
    if (end >= strLen) return false;  // unterminated
    out.assign(reinterpret_cast<const char*>(strTab + off), end - off);
    return true;
}

// The FST name is later used as one component below dvd_root on an SD card.
// Keep valid Wii byte/UTF-8 names intact, but reject components which either
// escape that root or which FAT cannot represent predictably.
bool safePathComponent(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name.size() > 255) return false;
    if (name.back() == '.' || name.back() == ' ') return false;

    for (const unsigned char c : name) {
        if (c < 0x20 || c == 0x7f) return false;
        switch (c) {
            case '/': case '\\': case ':': case '*': case '?':
            case '"': case '<': case '>': case '|':
                return false;
            default:
                break;
        }
    }
    return true;
}

// FAT directory lookup is ASCII case-insensitive.  Folding only ASCII avoids
// treating distinct non-ASCII Wii byte names as collisions without a complete
// Unicode case-folding implementation.
std::string fatCollisionKey(const std::string& path) {
    std::string key = path;
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return key;
}

}  // namespace

bool parseWiiFst(const std::uint8_t* fst, std::size_t len,
                 std::vector<FstNode>& out, std::string& err) {
    out.clear();
    err.clear();

    if (fst == nullptr || len < kEntrySize) {
        err = "FST too small";
        return false;
    }
    if (fst[0] != 1) {
        err = "FST root entry is not a directory";
        return false;
    }
    if (be24(fst + 1) != 0 || be32(fst + 4) != 0) {
        err = "FST root entry is malformed";
        return false;
    }

    const std::uint32_t count = be32(fst + 8);  // total entries incl. root
    if (count == 0) {
        err = "FST declares zero entries";
        return false;
    }
    if (static_cast<std::uint64_t>(count) * kEntrySize > len) {
        err = "FST entry array exceeds blob size";
        return false;
    }

    const std::uint8_t* strTab = fst + static_cast<std::size_t>(count) * kEntrySize;
    const std::size_t strLen = len - static_cast<std::size_t>(count) * kEntrySize;

    // Directory stack: each level records the exclusive end index of that dir and
    // the path prefix of entries inside it. Root spans [1, count) with prefix "".
    struct DirFrame {
        std::uint32_t end;
        std::uint32_t index;
        std::string prefix;
    };
    std::vector<DirFrame> stack;
    stack.push_back({count, 0, std::string()});
    std::unordered_set<std::string> paths;

    for (std::uint32_t i = 1; i < count; ++i) {
        while (!stack.empty() && i >= stack.back().end) {
            stack.pop_back();
        }
        if (stack.empty()) {
            err = "FST directory nesting is inconsistent";
            return false;
        }

        const std::uint8_t* e = fst + static_cast<std::size_t>(i) * kEntrySize;
        const std::uint8_t type = e[0];
        std::string name;
        if (!nameAt(strTab, strLen, be24(e + 1), name)) {
            err = "FST entry has an out-of-range name offset";
            return false;
        }
        if (!safePathComponent(name)) {
            err = "FST entry has an unsafe output path component";
            return false;
        }
        const std::string path = stack.back().prefix + "/" + name;
        if (!paths.insert(fatCollisionKey(path)).second) {
            err = "FST entries collide on a FAT output path";
            return false;
        }

        if (type == 1) {  // directory
            if (be32(e + 4) != stack.back().index) {
                err = "FST directory has an invalid parent index";
                return false;
            }
            const std::uint32_t next = be32(e + 8);
            if (next <= i || next > stack.back().end) {
                err = "FST directory has an invalid end index";
                return false;
            }
            out.push_back(FstNode{path, true, 0, 0});
            stack.push_back({next, i, path});
        } else if (type == 0) {  // file
            FstNode node;
            node.path = path;
            node.isDir = false;
            node.offset = static_cast<std::uint64_t>(be32(e + 4)) << 2;  // Wii: <<2
            node.size = be32(e + 8);
            out.push_back(node);
        } else {
            err = "FST entry has an unknown type";
            return false;
        }
    }
    return true;
}

}  // namespace kartpad::disc
