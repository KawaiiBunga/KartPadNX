#pragma once

// Horizon returns names, types AND sizes in a single directory read. The
// devoptab/std::filesystem walk instead asks the SD service for metadata for
// each entry (often twice). Keep the normal registration/overlay order while
// enumerating in batches. No persistent index: edits to mods appear next boot.
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#if defined(__SWITCH__)
#include <switch.h>

namespace kartpad::dvd {
struct DirectoryEntry {
    std::filesystem::path name;
    uint64_t size;
    const std::filesystem::path& path() const { return name; }
    bool is_regular_file(std::error_code& ec) const { ec.clear(); return true; }
    uint64_t file_size(std::error_code& ec) const { ec.clear(); return size; }
};

// False means the path belongs to a device without a native FS handle. Only
// that case uses the caller's portable walker. An error after opening must not
// restart a partially visited tree and duplicate registrations.
template<class Visit, class Error>
bool WalkNativeDirectory(const std::filesystem::path& root, bool recursive,
                         Visit&& visit, Error&& error) {
    FsFileSystem* filesystem = nullptr;
    char nativePath[FS_MAX_PATH];
    if (fsdevTranslatePath(root.string().c_str(), &filesystem, nativePath) < 0 || !filesystem)
        return false;
    struct Work { std::string native; std::filesystem::path host; uint64_t size = 0; bool directory = true; };
    std::vector<Work> pending{{nativePath, root}};
    // FsDirectoryEntry is ~800 bytes. Use heap storage, not the guest stack.
    constexpr size_t Batch = 32;
    auto entries = std::make_unique<FsDirectoryEntry[]>(Batch);
    while (!pending.empty()) {
        Work current = std::move(pending.back());
        pending.pop_back();
        if (!current.directory) {
            visit(DirectoryEntry{std::move(current.host), current.size});
            continue;
        }
        FsDir directory{};
        Result rc = fsFsOpenDirectory(filesystem, current.native.c_str(),
            FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &directory);
        if (R_FAILED(rc)) { error(current.host, rc); continue; }
        // Finish each open directory before descending, keeping handle use
        // bounded even for deep user-created trees.
        std::vector<Work> children;
        for (;;) {
            s64 count = 0;
            rc = fsDirRead(&directory, &count, Batch, entries.get());
            if (R_FAILED(rc)) { error(current.host, rc); break; }
            if (!count) break;
            for (s64 i = 0; i < count; ++i) {
                const auto& entry = entries[i];
                const std::string name(entry.name);
                if (name == "." || name == "..") continue;
                auto host = current.host / name;
                if (entry.type == FsDirEntryType_Dir) {
                    if (recursive) {
                        std::string path = current.native;
                        if (path.empty() || path.back() != '/') path += '/';
                        path += name;
                        children.push_back({std::move(path), std::move(host)});
                    }
                } else if (entry.type == FsDirEntryType_File && entry.file_size >= 0) {
                    children.push_back({{}, std::move(host), uint64_t(entry.file_size), false});
                }
            }
        }
        fsDirClose(&directory);
        // Match recursive_directory_iterator's depth-first entry order. This
        // matters for folder-by-name mappings with repeated filenames.
        for (auto it = children.rbegin(); it != children.rend(); ++it)
            pending.push_back(std::move(*it));
    }
    return true;
}
} // namespace kartpad::dvd
#endif
