#!/usr/bin/env python3
"""Use Horizon's batched directory metadata for DVD/mod indexing (idempotent)."""
from pathlib import Path
import argparse
import shutil

def transform(s):
    if 'kartpad::dvd::WalkNativeDirectory' in s:
        return s
    s = s.replace('#include "os_host_thread.h"', '#include "os_host_thread.h"\n#include "dvd_directory_index.h"\n#include <chrono>', 1)
    marker = 'static void WalkDirectory(const fs::path& root, bool recursive, bool announceErrors, Visit&& visit) {\n'
    assert marker in s
    s = s.replace(marker, marker + '''#if defined(__SWITCH__)
    if (kartpad::dvd::WalkNativeDirectory(root, recursive, visit,
        [&](const fs::path& path, Result rc) {
            if (announceErrors && rc != MAKERESULT(2, 1)) RT_LOGF(RT_TAG_DVD,
                "WARNING: directory scan %s: result 0x%08x\\n", path.string().c_str(), rc);
        })) return;
#endif
''', 1)
    assert s.count('const fs::directory_entry& entry) {') == 2
    s = s.replace('const fs::directory_entry& entry) {', 'const auto& entry) {')
    assert s.count('fs::file_size(entry.path(), entryEc)') == 2
    s = s.replace('fs::file_size(entry.path(), entryEc)', 'entry.file_size(entryEc)')
    # Existence is checked by opening the directory. Avoid an extra SD stat
    # for every folder mapping on Switch; retain portable semantics elsewhere.
    marker = '''    if (!fs::exists(root, ec)) {
        return;
    }
'''
    assert s.count(marker) == 1
    s = s.replace(marker, '#if !defined(__SWITCH__)\n'+marker+'#endif\n', 1)
    marker = '    // 4. Scan Files\n'
    assert marker in s
    s = s.replace(marker, marker+'''    const auto indexStart = std::chrono::steady_clock::now();
    const auto indexPhase = [&](const char* phase) {
        RT_LOGF(RT_TAG_DVD, "index timing: %s %.1f ms since scan start\\n", phase,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - indexStart).count());
    };
''', 1)
    s = s.replace('    const auto& overlays = RuntimeRiivolution::Overlays();', '    indexPhase("base directories");\n    const auto& overlays = RuntimeRiivolution::Overlays();', 1)
    s = s.replace('    // Load FST mapping so real files keep their physical disc extents.', '    indexPhase("overlays");\n    // Load FST mapping so real files keep their physical disc extents.', 1)
    s = s.replace('    BuildAndPublishRuntimeFst();', '    BuildAndPublishRuntimeFst();\n    indexPhase("FST published");', 1)
    return s

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runtime', type=Path)
    parser.add_argument('--archive', type=Path)
    args = parser.parse_args()
    path = args.runtime/'src/hle/storage/dvd.cpp'
    raw = path.read_text()
    updated = transform(raw)
    if updated == raw:
        print('DVD native directory index already integrated')
        return
    if args.archive:
        args.archive.parent.mkdir(parents=True, exist_ok=True)
        assert not args.archive.exists()
        shutil.copy2(path, args.archive)
    path.write_text(updated, newline='\n')
    assert transform(updated) == updated
    print('DVD native batched metadata scan integrated; registration logic retained')

if __name__ == '__main__': main()
