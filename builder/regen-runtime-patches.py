#!/usr/bin/env python3
"""Regenerate switch/patches/runtime/ from the staged runtime and Aurora sources.

    python builder/regen-runtime-patches.py

The staged sources under build/wiicompiled-fpscr are gitignored, so these patches are the only
durable record of the KartPad changes made in them: every changed or new source file, diffed
against the 2026-09-23 staged baseline (private/baseline-2026-09-23/sources.tar, extracted once
to private/baseline-2026-09-23/tree). The baseline already includes the older
wiicompiled-switch-*.patch files.

Each file goes into exactly one subsystem patch (GROUPS); `series` lists the patches in apply
order. builder/build-fullapp.sh applies them on a fresh stage, and
builder/tests/check-perf-patch-replay.py proves the series reproduces the staged tree.
Run this after every staged-source change that should survive a re-stage.
"""
from fnmatch import fnmatch
import os
from pathlib import Path
import subprocess
import sys
import tarfile

REPO = Path(__file__).resolve().parents[1]
BASE_TAR = REPO / 'private/baseline-2026-09-23/sources.tar'
BASE_DIR = REPO / 'private/baseline-2026-09-23/tree'
BASE = BASE_DIR / 'build/wiicompiled-fpscr'
CUR = REPO / 'build/wiicompiled-fpscr'
OUT = REPO / 'switch/patches/runtime'
ROOTS = [('runtime', ('src', 'include', 'cmake')), ('aurora-main', ('lib', 'include', 'cmake'))]
EXTS = ('.cpp', '.h', '.hpp', '.inc', '.c', '.cmake', '.txt')

# (patch name, path patterns). The first matching group owns a file.
GROUPS = [
    ('runtime-platform', ['runtime/src/main.cpp', 'runtime/include/runtime_config.h', 'runtime/src/settings_overlay.cpp',
                          'runtime/src/perf_diag.cpp', 'runtime/include/perf_diag.h', 'runtime/src/dynamic_aspect.cpp',
                          'runtime/src/hle/vi.cpp', 'runtime/cmake/*', 'runtime/include/hle_stubs.h']),
    ('runtime-cpu', ['runtime/include/isa/*', 'runtime/include/ppc_runtime.h', 'runtime/include/native_cpu_calls.inc',
                     'runtime/include/guest_flat_memory.h', 'runtime/src/memory.cpp', 'runtime/*/recomp_mod_loader.*',
                     'runtime/include/abi_bridge.h']),
    ('runtime-os', ['runtime/src/hle/os/*', 'runtime/include/os_host_thread.h', 'runtime/src/fiber_manager.cpp',
                    'runtime/include/guest_interrupt_context.h']),
    ('runtime-storage', ['runtime/src/hle/storage/*', 'runtime/include/hle/dvd_contract.h',
                         'runtime/include/nand_path.h', 'runtime/src/system_bridge.cpp']),
    ('runtime-audio', ['runtime/src/hle/audio/*', 'runtime/*/audio_backend.*', 'runtime/src/music_attenuation.cpp']),
    ('runtime-gx', ['runtime/src/hle/gx/*', 'runtime/include/gx_hle_thread.h', 'runtime/include/aurora_events.h']),
    ('runtime-input', ['runtime/src/hle/input/*']),
    ('runtime-net', ['runtime/src/hle/net/*']),
    ('aurora-gx', ['aurora-main/lib/gx/*', 'aurora-main/lib/dolphin/gx/*', 'aurora-main/cmake/aurora_gx.cmake']),
    ('aurora-graphics', ['aurora-main/lib/gfx/*', 'aurora-main/lib/webgpu/*', 'aurora-main/lib/switch/*',
                         'aurora-main/lib/sqlite_utils.hpp', 'aurora-main/lib/aurora.cpp', 'aurora-main/lib/window.cpp',
                         'aurora-main/lib/imgui.cpp']),
    ('aurora-input', ['aurora-main/lib/input.*', 'aurora-main/lib/dolphin/pad/*', 'aurora-main/lib/dolphin/si/*']),
]


def changed_files():
    changed = []
    for top, subs in ROOTS:
        for sub in subs:
            for path in sorted((CUR / top / sub).rglob('*')):
                if not path.is_file() or not path.name.endswith(EXTS):
                    continue
                relative = path.relative_to(CUR).as_posix()
                base = BASE / relative
                if not base.exists() or base.read_bytes() != path.read_bytes():
                    changed.append(relative)
    return changed


def file_diff(relative):
    base = BASE / relative
    result = subprocess.run(['diff', '-uN', '--label', 'a/' + relative if base.exists() else '/dev/null',
                             '--label', 'b/' + relative, str(base) if base.exists() else os.devnull,
                             str(CUR / relative)], capture_output=True)
    if result.returncode not in (0, 1):
        sys.exit(result.stderr.decode())
    # An explicit header keeps git apply --recount from reading the next file's header as part
    # of the previous hunk; LF only, because the Windows source mount mixes line endings.
    header = f'diff --git a/{relative} b/{relative}\n' + ('' if base.exists() else 'new file mode 100644\n')
    return header.encode() + result.stdout.replace(b'\r\n', b'\n')


def main():
    if not BASE.is_dir():
        with tarfile.open(BASE_TAR) as tar:
            tar.extractall(BASE_DIR, filter='data')
    owners = {}
    for relative in changed_files():
        group = next((name for name, patterns in GROUPS if any(fnmatch(relative, p) for p in patterns)), None)
        if group is None:
            sys.exit(f'{relative} belongs to no patch group: add it to GROUPS in {Path(__file__).name}')
        owners.setdefault(group, []).append(relative)

    OUT.mkdir(parents=True, exist_ok=True)
    for stale in OUT.glob('*.patch'):
        stale.unlink()
    series = []
    for index, (name, _) in enumerate(GROUPS, 1):
        if name not in owners:
            continue
        filename = f'{index:02d}-{name}.patch'
        (OUT / filename).write_bytes(b''.join(file_diff(relative) for relative in owners[name]))
        series.append(filename)
        print(f'{filename}: {len(owners[name])} files')
    (OUT / 'series').write_text(''.join(name + '\n' for name in series), newline='\n')
    print(f'{sum(map(len, owners.values()))} files in {len(series)} patches -> {OUT}')


if __name__ == '__main__':
    main()
