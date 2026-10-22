# KartPad-NX AIO builder

A Windows program that turns a user's own Mario Kart Wii disc image into the KartPad-NX
launcher (`mkw-launcher.nro`, with the base game and optionally Retro Rewind packed in).
Users install nothing: no Docker, WSL, devkitPro, Visual Studio, Python or .NET SDK.

What you distribute is `KartPadNX-Builder.zip` (~283 MB), a folder with two files:

```
KartPadNX-Builder.exe    ~125 KB, the frontend (the pipeline scripts are embedded)
KartPadNX-Builder.data   ~282 MB, Python, the build tools and the toolkit of prebuilt KartPad-NX parts
```

Neither contains Nintendo code or data: the games are translated and compiled on the user's PC
from their own disc. A first run downloads only devkitPro (365 MB, in the background while the
game is translated) and, for Retro Rewind, its pinned update (under 50 MB) and, when "Also
download the Retro Rewind SD files" is ticked, that version's full pack (1.9 GB).

The data file is separate on purpose. Windows Defender re-scans an executable's whole content
every time it starts: with the bundle appended to the exe, the window took ~10 s to appear on
every launch (~43 s the first time); as a data file it costs ~0.3 s once. `build.ps1 -Single`
still makes the one-file exe for anyone who prefers it.

Requirements: 64-bit Windows 10 or 11 (the downloaded .NET 8, Python 3.14 and LLVM need it),
about 12 GB free for the work folder plus 5 GB for the SD card folder, 8 GB RAM or more (less
works, one file at a time), and an internet connection for the first build (about 0.4 GB of
pinned downloads, plus 1.9 GB for Retro Rewind's full pack). The exe checks these before it
starts. It is not code-signed, so SmartScreen shows "Windows protected your PC" on first run
(More info, Run anyway).

## What the user does

1. Unzip `KartPadNX-Builder.zip` anywhere and run `KartPadNX-Builder.exe`.
2. Choose the disc image (`.iso`, `.wbfs`, `.rvz`, `.wia`, `.ciso`, `.gcz`). It must be PAL
   RMCP01 revision 0; anything else is rejected after extraction.
3. Click **Build**. The **SD card** folder (next to the exe by default) gets exactly what goes
   on the SD card, and the user copies its contents to the card's root. Nothing is imported
   on the Switch:

   ```
   switch/mkw-launcher.nro                     the launcher, with both games packed in
   switch/kartpad/data/                        the extracted disc (sys/, files/) and
                                               .kartpad-extraction-complete
   switch/kartpad/retro-rewind/RetroRewind6/   Retro Rewind, the version that was built
   INSTALL.txt
   ```

The game data is the tree and completion manifest the console's own extractor writes
(`switch/src/runtime/disc_extractor.cpp`: magic line, FNV-1a of `fst.bin`, then size and FNV-1a
of every file in FST order). The launcher verifies `main.dol` and `StaticR.rel` in place on
first start. The copy and the hashes (about 10 CPU-minutes in Python, on low-priority worker
processes) run in the background while the game is translated and compiled.

Retro Rewind's files come from its full pack (`<version>-full.zip` on its CDN, pinned by size
and SHA-256 in the approved list), because the update zips the build translates `Code.pul`
from carry only changed files. Its `Code.pul` must be the one that was built. It replaces the
previous `RetroRewind6` (saves sit outside it, in `retro-rewind/riivolution`). The zip is kept
in the work folder's `downloads`, so later builds only re-check it. An approved entry without a
full pack skips this, and `INSTALL.txt` then says where to get the files.

The work folder holds the downloaded tools, the extracted disc and the build: about 9 GB, 11
GB with Retro Rewind's pack. It
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
| `toolkit/make-prebuilt.py` | run by you (or by `make-toolkit.py --prebuilt-data`) | compiles the game-independent targets into the toolkit, plus the CMake configure seed |
| `toolkit/make-bundle.py` | run by you | the `.data` bundle: Python, the trimmed tools (LLVM: clang, lld, ar, nm, objcopy, headers), the toolkit |

Tools (all pinned in `tools.json`): Python 3.14.4 embeddable, .NET 8.0.32 runtime, CMake
3.31.12, Ninja 1.13.2, nodtool 2.0.0-alpha.9 (reads ISO/WBFS/RVZ/WIA/CISO/GCZ) and LLVM 19.1.7
(the clang version of the proven Docker build) are in the `.data` bundle; devkitPro's own
`devkitpro/devkita64` image layer comes from Docker Hub, over parallel connections and in the
background. Retro Rewind adds its pinned update zip (for `Code.pul`; `retro_rewind` in
`tools.json`), optionally its full pack (`full_url`/`full_size`/`full_sha256`, for the SD
files) and the live
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

### Prebuilt game-independent code

The player's PC compiles only the translated game. Everything else in the game NROs (the
WiiCompiled runtime, the Switch shim, Aurora, the deko3d renderer, uam, cryptopp, pugixml, and
fmt/imgui/sqlite3/zstd/xxhash/tracy) is compiled once by `toolkit/make-prebuilt.py` and shipped
as objects in the toolkit (`aio-prebuilt/libs/<profile>/`, 82 MB without debug sections; the
targets that take the PGO profile are per profile, the rest are shared).

- `make-prebuilt.py` configures `switch/` with the pipeline's own CMake command
  (`kpnx_aio.cmake_configure_cmd`) and the same pinned tools, against a stub translation, so no
  game code exists in that build. The compiler's dependency records must show that no prebuilt
  object reads the stub translation or the build's `generated/` headers. The two runtime files
  that include `generated/RuntimeConfig.h` (`abi_bridge.cpp`, `os_alarm.cpp`) are left out and
  compiled by the player. It also fails if an object defines a `func_8xxxxxxx` other than the
  runtime's hand-written `PPC_NATIVE_OVERRIDE` replacements.
- `switch/cmake/Prebuilt.cmake`, enabled only by `-DKARTPAD_PREBUILT_LIBS_DIR` (the Docker build
  never sets it), swaps each target's sources for its objects. The targets keep their names,
  usage requirements and link order. Each target carries a fingerprint of its sources, its
  per-source settings, its compile settings (PGO profile included), those of everything it
  links, the directory flags and the compiler version. A target whose fingerprint differs is
  compiled from source with a warning, so a mismatched toolkit is slower but never wrong.
- `aio-prebuilt/cmake/` seeds the configure step: the results of the ~20 compiler checks (zstd's
  and fmt's flag probes and FindThreads, ~5 s each as Windows try-compiles) and CMake's compiler
  identification files. They apply only when the tool hashes match the ones they were recorded
  with.

Checked on the PC below: every function in both ELFs has the same size as in a build without
prebuilt objects (`llvm-nm -S`: 60,493 base, 69,907 Retro Rewind), `_start` is at offset 0,
and both NROs have the same byte size.

Time on a 12-thread / 32 GB PC (both games, from nothing, downloads and the 1.9 GB Retro Rewind
pack included): **23 minutes**, against 44 with everything compiled locally. Per profile,
configure takes 31 s / 27 s (was 250 s / 156 s) and the compile 256 s / 294 s (was 508 s /
492 s). Translating the two games (about 11 minutes) is now the largest stage. A rebuild with
everything cached takes a couple of minutes.

## When Retro Rewind updates

Retro Rewind's code (`Code.pul`) is translated and compiled into the game, so an update that
changes it needs a rebuild, and the SD card's `RetroRewind6` has to be the same version. Players
never need a new builder for this: on
every build it reads the approved version list, a public copy of `manifest/rr-versions.json`
at `retro_rewind.approved_url` in `tools.json`, and builds its `latest`. When the list cannot be
read it builds the version in `tools.json`. List entries may only point at
`cdn.update.rwfc.net`, and every file is hash-checked.

To approve a new version:

```powershell
python aio\toolkit\approve-rr.py              # newest in the feed; or give a version
python aio\pipeline\kpnx_aio.py --rr --rr-list aio\manifest\rr-versions.json --data C:\KPNX --disc RMCP01.wbfs --out C:\KPNX-SD --bundle aio\out\dist\KartPadNX-Builder.data --shim aio\out\obj\nxshim.exe
```

`approve-rr.py` also downloads the version's full pack (~2 GB, into `aio\out\cache\rr`),
checks it has `xml/RetroRewind6.xml` and the same `Code.pul`, and records `full_url`,
`full_size` and `full_sha256`. `--no-full` leaves them out; players then get a message and
fetch the SD files themselves. When the approved version changes, also copy the entry's fields
into `tools.json` `retro_rewind` (the fallback) before the next release.

Check the result plays, then commit and push `aio/manifest/rr-versions.json`: the approved URL
is this file on the repo's `main` branch. Players get it the next time they click Build: only
Retro Rewind is re-translated and rebuilt (about 12 minutes), the SD card folder's
`RetroRewind6` is replaced from the new full pack, and they copy the folder to the card again. The builder also logs when the feed has a newer version than the approved one.

## Making a release

One command, nothing installed, everything kept in `aio\out` (gitignored):

```powershell
powershell -ExecutionPolicy Bypass -File aio\release.ps1
```

It takes this repo's sources and the binary parts (Dawn/SDL, the staged runtime and built
translator, the launcher ELF, compiled shaders, PGO profiles, shader seeds) from the previous
release in `aio\out\dist` (`-From <KartPadNX-Builder.data>` for another one), compiles the
game-independent code, and writes `aio\out\KartPadNX-Builder.zip`: attach that to a GitHub
release. About 20 minutes; `aio\out\work` keeps the compiled code so later runs are faster.

Only when the runtime, Aurora, Dawn, the launcher or the renderer shaders change do the binary
parts need a Docker source build ([docs/BUILDING-SWITCH.md](../docs/BUILDING-SWITCH.md)):
then run `release.ps1 -BuildRoot <that checkout>`. `make-toolkit.py` stops if a renderer shader
changed since its compiled copy, rather than shipping a stale one.

`aio\out` after a run: `dist\` and `KartPadNX-Builder.zip` (the release), `previous-release.data`
(what it was built from), `toolkit\`, `bundle.zip`, `work\` (compiled game-independent code),
`cache\` (pinned downloads), `python\`. All of it can be deleted except `dist\`, which the next
run starts from. When only the exe changes (pipeline or manifest), players replace the exe and
keep their `.data` file.

The individual steps (`release.ps1` runs them) are `toolkit\make-toolkit.py`,
`toolkit\make-bundle.py` and `build.ps1`. `make-bundle.py` fetches the pinned tool downloads once
into `aio\out\cache`. Without `-Bundle`
you get an exe that downloads every tool itself and uses a `KartPadNX-Toolkit*.zip` next to it
(`make-toolkit.py --zip`) or the toolkit URL in `manifest/tools.json`. A toolkit zip next to the
exe always wins over the bundled toolkit, to try a new one without rebuilding the bundle.
`make-toolkit.py` refuses to write a toolkit containing disc files, translated
functions or shards, game NROs or saves. `--no-pgo` and `--no-seeds` leave out the sample-PGO
profiles and the shader/pipeline seeds.

`--prebuilt-data` runs `make-prebuilt.py` on the new toolkit, with `aio\out\work` as its AIO
work folder (tools from `--bundle`, or downloaded). It takes about 15 minutes, less when the
folder already holds builds of the same sources. Run it again whenever the runtime, Aurora,
`switch/` sources, PGO profiles or pinned tools change. A stale toolkit only falls back to
compiling, but players lose the speed-up. It also runs Aurora's `builder/integrate-deko.py`
edits in the toolkit, which the player's build would apply anyway.

## Command line

The pipeline also runs without the frontend:

```powershell
python aio\pipeline\kpnx_aio.py --data C:\KPNX --disc D:\RMCP01.wbfs --out C:\KPNX-SD --rr --bundle aio\out\dist\KartPadNX-Builder.data --shim aio\out\obj\nxshim.exe
```

`--out` is the SD card folder; `--rr` also builds Retro Rewind and adds its files.
