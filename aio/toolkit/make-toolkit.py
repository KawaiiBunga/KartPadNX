#!/usr/bin/env python3
"""Assemble the AIO builder toolkit from this checkout (run on the developer machine).

  python aio/toolkit/make-toolkit.py --out aio/out/toolkit [--build-root DIR] [--zip] [--no-pgo] [--no-seeds]

The toolkit holds everything the user-side build needs that is not on a public download
server and not derived from the user's disc. It is laid out as a small copy of this
repository so the builder scripts resolve their usual paths:

  builder/                      the repo's Python passes (sources)
  switch/                       the Switch port sources and CMake
  launcher/assets, romfs        launcher icon and RomFS assets
  ref/kartpad-main/...          KartPad profile metadata, translator configs (GPLv3 sources)
  ref/upstream/Wiicompiled/projects/mkwii/MAP.txt
  ref/dawn/..., ref/SDL/include headers matching the prebuilt Dawn/SDL libraries
  build/wiicompiled-fpscr/      the staged, patched WiiCompiled runtime + Aurora sources and
                                the built translator (GPLv3, source included)
  build/dawn-switch-deko/       prebuilt Dawn Null + Tint static libraries and generated headers
  build/sdl-switch-vulkan/      prebuilt SDL3 static library
  build/launcher/               the launcher ELF, rebuilt into an NRO with the user's games
  aio-prebuilt/dksh/            renderer shaders precompiled by uam (no Windows uam exists)
  aio-prebuilt/deps/            Aurora's FetchContent sources (fmt, imgui, sqlite3, tracy, xxhash, zstd)
  private/pgo/                  sample-PGO profile and link order named by private/pgo/current*
  private/deko-training/        shader / pipeline seeds packed into the game RomFS
  private/self-build/.../base_common_shard_map.json
                                frozen shard membership (function addresses only)

It never copies translated game code, disc files, game NROs or Retro Rewind files; a final
scan refuses the toolkit if any such file slipped in.
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import time
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Aurora FetchContent dependencies (directory names under build/switch-fullapp/_deps).
FETCH_DEPS = ('fmt', 'imgui', 'sqlite3', 'tracy', 'xxhash', 'zstd')
HEADER_SUFFIXES = {'.h', '.hpp', '.hh', '.inc', '.inl', '.def', '.tmpl'}
SKIP_DIRS = {'__pycache__', '.git', '.github'}
# Disc- or translation-derived files that must never be in a toolkit.
FORBIDDEN_NAMES = ('main.dol', 'StaticR.rel', 'Code.pul', 'data_sections_init.cpp',
                   'guest_symbol_table.cpp', 'RuntimeConfig.h', 'kartpad.nro',
                   'kartpad-retro-rewind.nro', 'mkw-launcher.nro', 'rksys.dat',
                   'native_model_region.cpp', 'native_scene_regions.cpp')


class Toolkit:
    def __init__(self, out):
        self.out = out
        self.files = 0
        self.bytes = 0

    def copy_file(self, src, rel):
        dst = self.out / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        self.files += 1
        self.bytes += dst.stat().st_size

    def copy_tree(self, src, rel, suffixes=None, skip=SKIP_DIRS, exclude=(), skip_top=()):
        src = Path(src)
        if not src.is_dir():
            sys.exit(f'missing: {src}')
        for d, dirs, files in os.walk(src):
            dirs[:] = [x for x in dirs if x not in skip and not (Path(d) == src and x in skip_top)]
            for f in files:
                p = Path(d) / f
                if suffixes is not None and p.suffix.lower() not in suffixes:
                    continue
                if p.suffix in ('.orig', '.rej') or any(p.match(e) for e in exclude):
                    continue
                if p.is_symlink() or not p.is_file():
                    continue  # Linux checkouts carry symlinks Windows cannot read
                self.copy_file(p, Path(rel) / p.relative_to(src))


# The repo's CMake writes devkitPro paths into flag strings unquoted, which splits them when
# the path has a space (the builder's work folder often does: C:\Users\First Last\...). The
# toolkit's copy quotes them; the repo itself is left as it is.
SPACE_SAFE_EDITS = {
    'switch/cmake/SwitchClang.cmake': [
        ('--gcc-toolchain=${DEVKITPRO}/devkitA64', '\\"--gcc-toolchain=${DEVKITPRO}/devkitA64\\"'),
        ('string(APPEND _KARTPAD_CXX_MULTILIB_INC " -I${_d}")', 'string(APPEND _KARTPAD_CXX_MULTILIB_INC " \\"-I${_d}\\"")'),
    ],
    'switch/CMakeLists.txt': [
        ('"-L$ENV{DEVKITPRO}/libnx/lib -L$ENV{DEVKITPRO}/portlibs/switch/lib -fPIE -specs=$ENV{DEVKITPRO}/libnx/switch.specs"',
         '"\\"-L$ENV{DEVKITPRO}/libnx/lib\\" \\"-L$ENV{DEVKITPRO}/portlibs/switch/lib\\" -fPIE \\"-specs=$ENV{DEVKITPRO}/libnx/switch.specs\\""'),
    ],
}


def make_space_safe(out):
    for rel, edits in SPACE_SAFE_EDITS.items():
        p = out / rel
        text = p.read_text(encoding='utf-8')
        for old, new in edits:
            if text.count(old) != 1:
                sys.exit(f'{rel}: expected exactly one "{old}" to quote; update SPACE_SAFE_EDITS')
            text = text.replace(old, new)
        p.write_text(text, encoding='utf-8', newline='')


def pgo_names(bld):
    names = set()
    for marker in ('current', 'current-retro-rewind'):
        p = bld / 'private/pgo' / marker
        if p.is_file():
            n = p.read_text().strip()
            if n:
                names.add((marker, n))
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--out', type=Path, default=REPO / 'aio/out/toolkit')
    ap.add_argument('--zip', action='store_true', help='also write <out>.zip')
    ap.add_argument('--no-pgo', action='store_true', help='leave out the sample-PGO profiles')
    ap.add_argument('--no-seeds', action='store_true', help='leave out the shader/pipeline seeds')
    ap.add_argument('--build-root', type=Path, default=REPO,
                    help='checkout that holds the Docker build outputs (build/, private/) and the fetched '
                         'upstreams (ref/dawn, ref/SDL, ref/upstream); sources always come from this repo')
    args = ap.parse_args()
    BLD = args.build_root.resolve()
    out = args.out.resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    tk = Toolkit(out)
    t0 = time.time()

    # Sources.
    for p in (REPO / 'builder').glob('*.py'):
        tk.copy_file(p, Path('builder') / p.name)
    # switch/kartpad and switch/mkw-launcher are local SD-card mirrors (gitignored); the
    # native_model_region files in switch/src are generated from a translation (gitignored).
    tk.copy_tree(REPO / 'switch', 'switch', skip_top={'tests', 'docker', 'mkw-launcher', 'kartpad'},
                 exclude=('native_model_region.cpp', 'native_model_region.json'))
    make_space_safe(out)
    tk.copy_file(REPO / 'launcher/assets/icon.jpg', 'launcher/assets/icon.jpg')
    tk.copy_tree(REPO / 'launcher/romfs', 'launcher/romfs')
    km = REPO / 'ref/kartpad-main'
    tk.copy_tree(km / 'builder/kartpad_builder', 'ref/kartpad-main/builder/kartpad_builder')
    tk.copy_tree(km / 'builder/profiles', 'ref/kartpad-main/builder/profiles')
    tk.copy_tree(km / 'tools', 'ref/kartpad-main/tools')
    # The translation hooks builder/translator-post-hooks.py applies.
    for script in ('inject-g10-rkg-fixture-hook.py', 'inject-online-rkg-selection-hooks.py',
                   'inject-g10-camera-lifecycle-guard.py', 'inject-retro-rel-report-guard.py'):
        tk.copy_file(km / 'scripts' / script, Path('ref/kartpad-main/scripts') / script)
    for lic in ('LICENSE', 'RIGHTS_AND_LICENSES.md'):
        if (km / lic).is_file():
            tk.copy_file(km / lic, Path('ref/kartpad-main') / lic)
    tk.copy_file(BLD / 'ref/upstream/Wiicompiled/projects/mkwii/MAP.txt',
                 'ref/upstream/Wiicompiled/projects/mkwii/MAP.txt')
    tk.copy_tree(BLD / 'ref/dawn/include', 'ref/dawn/include')
    tk.copy_tree(BLD / 'ref/dawn/src', 'ref/dawn/src', suffixes=HEADER_SUFFIXES)
    tk.copy_tree(BLD / 'ref/dawn/third_party/abseil-cpp', 'ref/dawn/third_party/abseil-cpp',
                 suffixes=HEADER_SUFFIXES)
    tk.copy_tree(BLD / 'ref/SDL/include', 'ref/SDL/include')
    for lic in ('ref/dawn/LICENSE', 'ref/SDL/LICENSE.txt', 'ref/sdl3-switch/LICENSE.txt'):
        if (BLD / lic).is_file():
            tk.copy_file(BLD / lic, lic)

    # Staged runtime, Aurora and the built translator.
    stg = BLD / 'build/wiicompiled-fpscr'
    # generated/ holds pc-harvest.py's rebased copies of a translation; test_output is upstream's.
    tk.copy_tree(stg, 'build/wiicompiled-fpscr', skip=SKIP_DIRS | {'obj'}, skip_top={'generated', 'test_output'},
                 exclude=('*.pdb',))
    dll = out / 'build/wiicompiled-fpscr/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
    if not dll.is_file():
        sys.exit(f'translator not built: {dll} (dotnet build it first)')

    # Prebuilt graphics libraries.
    dawn = BLD / 'build/dawn-switch-deko'
    for p in dawn.rglob('*.a'):
        tk.copy_file(p, Path('build/dawn-switch-deko') / p.relative_to(dawn))
    tk.copy_tree(dawn / 'gen', 'build/dawn-switch-deko/gen')
    tk.copy_file(BLD / 'build/sdl-switch-vulkan/libSDL3.a', 'build/sdl-switch-vulkan/libSDL3.a')

    # Launcher ELF (its NRO is rebuilt around the user's games) and NACP.
    tk.copy_file(BLD / 'build/launcher/mkw_launcher.elf', 'build/launcher/mkw_launcher.elf')
    tk.copy_file(BLD / 'build/launcher/mkw-launcher.nacp', 'build/launcher/mkw-launcher.nacp')

    # Precompiled renderer shaders, checked against this repo's sources.
    dksh = BLD / 'build/switch-fullapp/dksh'
    sums = []
    for glsl in sorted((REPO / 'switch/deko/shaders').glob('*.glsl')):
        bin_ = dksh / (glsl.stem + '.dksh')
        if not bin_.is_file():
            sys.exit(f'no precompiled shader for {glsl.name} in {dksh}')
        tk.copy_file(bin_, Path('aio-prebuilt/dksh') / bin_.name)
        sums.append(hashlib.sha256(glsl.read_bytes().replace(b'\r\n', b'\n')).hexdigest() + '  ' + glsl.name)
    (out / 'aio-prebuilt/dksh/sources.sha256').write_text('\n'.join(sums) + '\n')

    deps = BLD / 'build/switch-fullapp/_deps'
    for name in FETCH_DEPS:
        tk.copy_tree(deps / f'{name}-src', f'aio-prebuilt/deps/{name}-src',
                     skip={'.git', '.github', '__pycache__', 'tests', 'test', 'doc', 'docs', 'examples'})

    # Optional performance data.
    if not args.no_pgo:
        for marker, name in sorted(pgo_names(BLD)):
            tk.copy_file(BLD / 'private/pgo' / marker, Path('private/pgo') / marker)
            for ext in ('.extbin', '.order.ld'):
                tk.copy_file(BLD / 'private/pgo' / (name + ext), Path('private/pgo') / (name + ext))
    if not args.no_seeds:
        for name in ('initial_dksh_cache.bin', 'initial_pipeline_cache.db', 'initial_pipeline_cache.id'):
            tk.copy_file(BLD / 'private/deko-training' / name, Path('private/deko-training') / name)

    # Frozen shard boundaries for each profile.
    for rel in ('private/self-build/translation/build_shards/base_common_shard_map.json',
                'private/self-build/retro-rewind/translation/build_shards/base_common_shard_map.json'):
        if (BLD / rel).is_file():
            tk.copy_file(BLD / rel, rel)

    # Refuse anything disc- or translation-derived.
    bad = []
    for d, _, files in os.walk(out):
        for f in files:
            if f in FORBIDDEN_NAMES or (f.startswith('func_8') and f.endswith('.cpp')) \
                    or (f.startswith('shard_') and f.endswith('.cpp')):
                bad.append(os.path.join(d, f))
    if bad:
        sys.exit('toolkit contains disc-derived files:\n  ' + '\n  '.join(bad[:20]))

    manifest = {'schema': 1, 'created_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                'files': tk.files, 'bytes': tk.bytes, 'pgo': not args.no_pgo, 'seeds': not args.no_seeds}
    (out / 'aio-toolkit.json').write_text(json.dumps(manifest, indent=2))
    print(f'toolkit: {tk.files} files, {tk.bytes / 1e6:.0f} MB in {out} ({time.time() - t0:.0f}s)')

    if args.zip:
        zpath = out.with_suffix('.zip')
        with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            for d, _, files in os.walk(out):
                for f in sorted(files):
                    p = Path(d) / f
                    z.write(p, p.relative_to(out).as_posix())
        digest = hashlib.sha256(zpath.read_bytes()).hexdigest()
        print(f'zip: {zpath} {zpath.stat().st_size / 1e6:.0f} MB sha256 {digest}')


if __name__ == '__main__':
    main()
