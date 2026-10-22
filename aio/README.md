# KartPad-NX AIO builder

A Windows program that turns a user's own Mario Kart Wii disc image into the KartPad-NX
launcher (`mkw-launcher.nro`, with the base game and optionally Retro Rewind packed in).
Users install nothing: no Docker, WSL, devkitPro, Visual Studio, Python or .NET SDK.

What you distribute is `KartPadNX-Builder.zip` (~265 MB), a folder with two files:

```
KartPadNX-Builder.exe    ~110 KB, the frontend (the pipeline scripts are embedded)
KartPadNX-Builder.data   ~263 MB, Python, the build tools and the toolkit of prebuilt KartPad-NX parts
```

Neither contains Nintendo code or data: the games are translated and compiled on the user's PC
from their own disc. A first run downloads only devkitPro (365 MB, in the background while the
game is translated) and, for Retro Rewind, its 41 MB pinned update.

The data file is separate on purpose. Windows Defender re-scans an executable's whole content
every time it starts: with the bundle appended to the exe, the window took ~10 s to appear on
every launch (~43 s the first time); as a data file it costs ~0.3 s once. `build.ps1 -Single`
still makes the one-file exe for anyone who prefers it.

Requirements: 64-bit Windows 10 or 11 (the downloaded .NET 8, Python 3.14 and LLVM need it),
about 10 GB free, 8 GB RAM or more (less works, one file at a time), and an internet
connection for the first build (about 0.4 GB of pinned downloads). The exe checks these before
it starts. It is not code-signed, so SmartScreen shows "Windows protected your PC" on first run
(More info, Run anyway).

## What the user does

1. Unzip `KartPadNX-Builder.zip` anywhere and run `KartPadNX-Builder.exe`.
2. Choose the disc image (`.iso`, `.wbfs`, `.rvz`, `.wia`, `.ciso`, `.gcz`). It must be PAL
   RMCP01 revision 0; anything else is rejected after extraction.
3. Click **Build**. The output folder gets `switch/mkw-launcher.nro` and `INSTALL.txt`.

The work folder holds the downloaded tools and the build: about 6.6 GB with Retro Rewind. It
defaults to `KartPadNX-Builder-data` next to the exe, or to `C:\KartPadNX-data` when the exe's
folder has a path the build tools cannot use (non-ASCII characters, a network path, or too long
for Windows' 260-character limit once the build tree nests it). Builds resume: run Build again
after an error or a cancel and finished stages are skipped.

## How it works

| Piece | Where | Role |
|---|---|---|
| `frontend/Builder.cs` | compiled by `build.ps1` with Windows' own `csc.exe` (.NET Framework 4.8) | UI; fetches the pinned embeddable Python, runs the pipeline, shows its progress |
| `pipeline/kpnx_aio.py` | embedded in the exe | the build: tools, toolkit, disc, translate, transform, compile, launcher |
| `pipeline/nxtools.py` | embedded | Windows replacements for devkitPro's host programs (see below) |
| `shim/nxshim.cs` | embedded as `nxshim.exe` | copies named `aarch64-none-elf-g++.exe`, `elf2nro.exe`, `nacptool.exe`, `bin2s.exe`, `uam.exe`, `aarch64-none-elf-pkg-config.exe` forward to `nxtools.py` |
| `manifest/tools.json` | embedded | every download, pinned by size and SHA-256 |
| `toolkit/make-toolkit.py` | run by you | packs the toolkit from this checkout |
| `toolkit/make-bundle.py` | run by you | the `.data` bundle: Python, the trimmed tools (LLVM: clang, lld, ar, nm, objcopy, headers), the toolkit |

Tools (all pinned in `tools.json`): Python 3.14.4 embeddable, .NET 8.0.32 runtime, CMake
3.31.12, Ninja 1.13.2, nodtool 2.0.0-alpha.9 (reads ISO/WBFS/RVZ/WIA/CISO/GCZ) and LLVM 19.1.7
(the clang version of the proven Docker build) are in the `.data` bundle; devkitPro's own
`devkitpro/devkita64` image layer comes from Docker Hub, over parallel connections and in the
background. Retro Rewind adds its pinned update zip (for `Code.pul`; `retro_rewind` in
`tools.json`) and the live
Retro WFC payload, which must carry the production RSA signature. Zips are unpacked with 8
threads straight from the bundle.

### devkitPro

devkitPro asks that its binaries are not rehosted, and blocks scraping of its package server.
The builder therefore takes only the target-side files (libnx, newlib, libstdc++, libgcc, crt
objects, portlibs, linker script, CMake platform files) from the official Docker Hub image, and
never runs or ships a devkitPro host program. `nxtools.py` replaces them:

- `link`: the `aarch64-none-elf-g++` link step, applying `switch.specs` and linking with LLVM
  lld. The GNU-only `--nx-module-name` becomes a linked `.nx-module-name` object;
  `--section-ordering-file` becomes lld's `--symbol-ordering-file`. lld gets a copy of libnx's
  `switch.ld` with a `PT_TLS` header (lld needs it for local-exec TLS offsets; GNU ld derives
  the same offsets without it), `.relr.dyn` narrowed to `*(.relr.dyn)` (lld's internal
  `.relr.auth.dyn` would otherwise collide), and `.main.tls` typed NOBITS (as GNU ld makes it).
- `elf2nro`, the RomFS builder, `nacptool`, `bin2s`: ports of switch-tools/general-tools. Checked
  byte-for-byte: they reproduce the deployed candidate-v9 launcher NRO, base game NRO, NACPs and
  bin2s output from the same inputs.
- `uam`: copies the toolkit's precompiled renderer shaders (checked against the GLSL hashes).
- `pkg-config`: a stub; devkitPro's `Switch.cmake` only requires it to exist.

The local copy of `dkp-initialize-path.cmake` loses its "Windows host is not supported" check.

### Equivalence with the Docker build

- Disc extraction, translation, post-hooks and source passes are the same programs; the Windows
  translation is byte-identical to `private/self-build/translation` apart from embedded paths
  (the toolkit carries the frozen `base_common_shard_map.json`, so shard membership matches).
- Compile flags come from the unchanged `switch/` CMake. Same clang version, same sysroot.
  Against the Docker-built candidate-v9 ELFs, every function common to both has the same size
  (49,493 base, 53,683 Retro Rewind); the Retro Rewind translation and mod output are also
  identical apart from paths.
- The link differs: LLVM lld instead of GNU ld. Same segment boundaries and BSS size, the same
  MOD0/LNY0/LNY1 block, only RELATIVE (RELR) dynamic relocations, and the PGO hot set lands in
  the same first 8 MB of `.text`, but the exact function order is not GNU ld's. The
  `_start`-at-offset-0 image check still runs. **An NRO from this builder needs its own hardware
  run before it is treated as equivalent to a Docker-built one.**

Time on a 12-thread / 32 GB PC: about 35 minutes from nothing (downloads included) for both
games; a rebuild with everything cached takes a couple of minutes.

## When Retro Rewind updates

Retro Rewind's code (`Code.pul`) is translated and compiled into the game, so an update that
changes it needs a rebuild; an update that only changes assets (tracks, music, UI) just needs
the new `RetroRewind6` folder on the SD card. Players never need a new builder for this: on
every build it reads the approved version list, a public copy of `manifest/rr-versions.json`
at `retro_rewind.approved_url` in `tools.json`, and builds its `latest`. When the list cannot be
read it builds the version in `tools.json`. List entries may only point at
`cdn.update.rwfc.net`, and every file is hash-checked.

To approve a new version:

```powershell
python aio\toolkit\approve-rr.py              # newest in the feed; or give a version
python aio\pipeline\kpnx_aio.py --rr --rr-list aio\manifest\rr-versions.json --data C:\KPNX --disc RMCP01.wbfs --out C:\KPNX-out --bundle aio\out\dist\KartPadNX-Builder.data --shim aio\out\obj\nxshim.exe
```

Check the result plays, then commit and push `aio/manifest/rr-versions.json`: the approved URL
is this file on the repo's `main` branch. Players get it the next time they click Build: only
Retro Rewind is re-translated and rebuilt (about 25 minutes),
and `INSTALL.txt` names the version to put on the SD card. The builder also logs when the feed
has a newer version than the approved one.

## Making a release

On the developer machine, after a normal Docker build of the game and launcher
([docs/BUILDING-SWITCH.md](../docs/BUILDING-SWITCH.md)). The toolkit takes this repo's sources,
and the prebuilt Dawn/SDL libraries, staged runtime, built translator, launcher ELF, compiled
shaders, PGO profiles and shader seeds from the checkout the Docker build ran in
(`--build-root`, default this repo):

```powershell
python aio\toolkit\make-toolkit.py --out aio\out\toolkit --build-root "D:\path\to\build checkout"
python aio\toolkit\make-bundle.py --toolkit aio\out\toolkit
powershell -ExecutionPolicy Bypass -File aio\build.ps1 -Bundle aio\out\bundle.zip
```

`aio\out\KartPadNX-Builder.zip` is the release (also unpacked in `aio\out\dist\`): attach it to
a GitHub release. When only the exe changes (pipeline or manifest), players replace the exe and
keep their `.data` file.
`make-bundle.py` fetches the pinned tool downloads once into `aio\out\cache`. Without `-Bundle`
you get an exe that downloads every tool itself and uses a `KartPadNX-Toolkit*.zip` next to it
(`make-toolkit.py --zip`) or the toolkit URL in `manifest/tools.json`. A toolkit zip next to the
exe always wins over the bundled toolkit, to try a new one without rebuilding the bundle.
`make-toolkit.py` refuses to write a toolkit containing disc files, translated
functions or shards, game NROs or saves. `--no-pgo` and `--no-seeds` leave out the sample-PGO
profiles and the shader/pipeline seeds.

## Command line

The pipeline also runs without the frontend:

```powershell
python aio\pipeline\kpnx_aio.py --data C:\KPNX --disc D:\RMCP01.wbfs --out C:\KPNX-out --rr --bundle aio\out\dist\KartPadNX-Builder.data --shim aio\out\obj\nxshim.exe
```
