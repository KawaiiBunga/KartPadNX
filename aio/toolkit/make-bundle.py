#!/usr/bin/env python3
"""Assemble the zip that build.ps1 appends to KartPadNX-Builder.exe.

  python aio/toolkit/make-bundle.py --toolkit aio/out/toolkit --out aio/out/bundle.zip

Everything the build needs that may be redistributed goes into the exe, so a first run only
downloads devkitPro (which asks not to be rehosted) and, for Retro Rewind, the pinned RR update
and the live Retro WFC payload:

  python/            Python 3.14 embeddable (runs the pipeline)
  tools/dotnet/      .NET 8 runtime (runs the translator)
  tools/cmake/       CMake (bin/cmake.exe and its modules)
  tools/ninja/       Ninja
  tools/nodtool/     nodtool (disc images)
  tools/llvm/        clang, lld, llvm-ar, llvm-nm, llvm-objcopy and clang's headers; the
                     same-binary names (clang++, ld.lld, llvm-ranlib, llvm-strip) are linked
                     at install time instead of stored again
  toolkit/           the tree from make-toolkit.py
  bundle.json        what is inside, the per-tool keys the pipeline records, aliases
  THIRD-PARTY.txt    versions, licenses and sources of the bundled tools

The inputs are the pinned downloads in aio/manifest/tools.json (cached in aio/out/cache).
"""
import argparse
import hashlib
import json
import os
import shutil
import sys
import time
import uuid
import zipfile
from pathlib import Path

AIO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AIO / 'pipeline'))
import kpnx_aio  # noqa: E402  (download and extract helpers)

LLVM_KEEP = ['bin/clang.exe', 'bin/lld.exe', 'bin/llvm-ar.exe', 'bin/llvm-nm.exe', 'bin/llvm-objcopy.exe',
             'lib/clang/19/include/']
# The release tarball carries no top-level license; take it from the same tag.
LLVM_LICENSE = 'https://raw.githubusercontent.com/llvm/llvm-project/llvmorg-19.1.7/llvm/LICENSE.TXT'
LLVM_ALIASES = {'bin/clang++.exe': 'bin/clang.exe', 'bin/ld.lld.exe': 'bin/lld.exe',
                'bin/llvm-ranlib.exe': 'bin/llvm-ar.exe', 'bin/llvm-strip.exe': 'bin/llvm-objcopy.exe'}
CMAKE_KEEP = ['bin/cmake.exe', 'share/cmake-3.31/', 'doc/cmake-3.31/Copyright.txt']

THIRD_PARTY = """Third-party programs bundled in KartPadNX-Builder.exe
=====================================================

Python {py}              PSF License 2.0            https://www.python.org/
.NET Runtime 8.0.32      MIT                         https://dotnet.microsoft.com/ (tools/dotnet/LICENSE.txt)
CMake 3.31.12            BSD 3-Clause                https://cmake.org/ (tools/cmake/doc/cmake-3.31/Copyright.txt)
Ninja 1.13.2             Apache License 2.0          https://ninja-build.org/
nodtool 2.0.0-alpha.9    MIT OR Apache-2.0           https://github.com/encounter/nod
LLVM 19.1.7              Apache 2.0 with LLVM exception  https://llvm.org/ (tools/llvm/LICENSE.TXT)

Downloaded at build time, not bundled: devkitPro's devkitpro/devkita64 Docker image layer
(libnx, newlib, libstdc++; devkitPro asks that its binaries are not rehosted).
The toolkit/ tree's own sources and licenses are listed in its files (WiiCompiled and KartPad
GPL-3.0, Dawn BSD-3-Clause, SDL zlib, ...).
"""


def tree_digest(root):
    h = hashlib.sha256()
    for d, dirs, files in os.walk(root):
        dirs.sort()
        for f in sorted(files):
            p = Path(d) / f
            h.update(p.relative_to(root).as_posix().encode() + b'\0')
            h.update(kpnx_aio.sha256_file(p).encode())
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--toolkit', type=Path, required=True, help='toolkit tree from make-toolkit.py')
    ap.add_argument('--out', type=Path, default=AIO / 'out/bundle.zip')
    ap.add_argument('--cache', type=Path, default=AIO / 'out/cache')
    args = ap.parse_args()
    if not (args.toolkit / 'aio-toolkit.json').is_file():
        sys.exit(f'{args.toolkit} is not a toolkit tree (run make-toolkit.py first)')
    m = json.loads((AIO / 'manifest/tools.json').read_text(encoding='utf-8'))
    stage = AIO / 'out/bundle-stage'
    if stage.exists():
        shutil.rmtree(stage)
    t0 = time.time()

    def fetch(name):
        spec = m[name]
        fname = spec.get('name') or Path(spec['url'].split('?')[0]).name
        return kpnx_aio.download(spec['url'], args.cache / fname, spec['size'], spec['sha256'], label=name)

    kpnx_aio.extract_zip(fetch('python'), stage / 'python')
    kpnx_aio.extract_zip(fetch('dotnet'), stage / 'tools/dotnet')
    kpnx_aio.extract_zip(fetch('cmake'), stage / 'tools/cmake', strip=1, include=CMAKE_KEEP)
    shutil.rmtree(stage / 'tools/cmake/share/cmake-3.31/Help', ignore_errors=True)
    kpnx_aio.extract_zip(fetch('ninja'), stage / 'tools/ninja')
    (stage / 'tools/nodtool').mkdir(parents=True)
    shutil.copyfile(fetch('nodtool'), stage / 'tools/nodtool/nodtool.exe')
    kpnx_aio.extract_tar(fetch('llvm'), stage / 'tools/llvm', 'r:*', strip=1, include=LLVM_KEEP)
    for keep in LLVM_KEEP:
        if not (stage / 'tools/llvm' / keep).exists():
            sys.exit(f'LLVM archive is missing {keep}')
    shutil.copyfile(kpnx_aio.download(LLVM_LICENSE, args.cache / 'LLVM-LICENSE.TXT'), stage / 'tools/llvm/LICENSE.TXT')

    info = {
        'schema': 1, 'id': str(uuid.uuid4()), 'created_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        # Same keys as a download install, so switching between the two does not reinstall.
        'tools': {n: m[n]['sha256'] for n in ('dotnet', 'cmake', 'ninja', 'nodtool', 'llvm')},
        'python_sha256': m['python']['sha256'],
        'aliases': {'llvm': LLVM_ALIASES},
        'toolkit_sha256': tree_digest(args.toolkit),
    }
    (stage / 'bundle.json').write_text(json.dumps(info, indent=2), encoding='utf-8')
    (stage / 'THIRD-PARTY.txt').write_text(THIRD_PARTY.format(py=Path(m['python']['url']).parts[-2]),
                                           encoding='utf-8')

    args.out.parent.mkdir(parents=True, exist_ok=True)
    part = args.out.with_suffix('.part')
    count = 0
    with zipfile.ZipFile(part, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
        z.write(stage / 'bundle.json', 'bundle.json')
        for top, prefix in ((stage, ''), (args.toolkit, 'toolkit/')):
            for d, dirs, files in os.walk(top):
                dirs.sort()
                if top == stage and Path(d) == stage:
                    dirs[:] = [x for x in dirs]  # python/, tools/
                for f in sorted(files):
                    p = Path(d) / f
                    rel = prefix + p.relative_to(top).as_posix()
                    if rel == 'bundle.json':
                        continue
                    z.write(p, rel)
                    count += 1
    os.replace(part, args.out)
    print(f'bundle: {count} files, {args.out.stat().st_size / 1e6:.0f} MB -> {args.out} '
          f'({time.time() - t0:.0f}s), toolkit {info["toolkit_sha256"][:12]}')


if __name__ == '__main__':
    main()
