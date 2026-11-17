# KartPadNX builder for Linux

A command-line port of the Windows AIO builder. It turns your own Mario Kart Wii disc image
(PAL, RMCP01, revision 0) into the `SD card` folder for the KartPadNX launcher, using the same
pipeline as the Windows `KartPadNX-Builder.exe`.

**Status: one full build has completed on Fedora** (Python 3.14, x86-64, glibc): base game plus
Retro Rewind 6.13.5, linked with devkitPro's own GNU ld, with the SD card folder produced. Other
distributions are untested; please report results. See "Verified, and not yet verified" for what that run did not
cover.

## What you need

- **A git checkout of KartPadNX** with this builder inside it (`aio/linux/...`). The game's CMake
  links through `builder/link-switch.sh`, which the toolkit does not contain, so the builder copies
  `builder/*.sh` from the checkout (or from `--repo DIR`).

- x86-64 Linux with glibc (Debian/Ubuntu/Fedora/Arch are fine; Alpine is not, devkitPro's tools are glibc builds)
- 8 GB RAM, about 15 GB free for the work folder and 5 GB for the output
- Python 3.9 or newer, plus a zstd compressor: Python 3.14, or `pip install zstandard`, or the `zstd` program
- Your disc image: `.iso`, `.wbfs`, `.rvz`, `.wia`, `.ciso` or `.gcz`, PAL revision 0
- The **toolkit**: download `KartPadNX-Builder.zip` from the
  [releases page](https://github.com/KawaiiBunga/KartPadNX/releases), unzip it, and use the
  `KartPadNX-Builder.data` file in it. Only its `toolkit/` part is used (it holds the prebuilt
  runtime, translator, renderer and launcher; none of that is Windows-specific). Its Windows
  tools are ignored.

Build tools are taken from your system when they are usable, and downloaded otherwise:

| Tool | System copy used if | Debian/Ubuntu package |
| --- | --- | --- |
| cmake | 3.25 or newer | `cmake` |
| ninja | any | `ninja-build` |
| dotnet | runtime 8 or newer | `dotnet-runtime-8.0` |
| LLVM | **19.x** with clang, lld, llvm-ar/nm/objcopy/strip | `clang-19 lld-19 llvm-19` |
| nodtool | on `PATH` or `$KPNX_NODTOOL` | not packaged; downloaded |

devkitPro's files always come from the pinned Docker Hub layer (about 365 MB), as on Windows.

```
sudo apt install python3 cmake ninja-build clang-19 lld-19 llvm-19 zstd
```

## Use

```
cd aio/linux
./kartpadnx-builder --check --bundle ~/KartPadNX-Builder/KartPadNX-Builder.data --disc ~/RMCP01.wbfs
./kartpadnx-builder         --bundle ~/KartPadNX-Builder/KartPadNX-Builder.data --disc ~/RMCP01.wbfs
```

The first command lists what is missing and what would be downloaded. The second builds. The
first build takes 30 to 60 minutes. When it is done, copy everything in `./KartPadNX-SD` to the
root of your SD card, then start **MKW Launcher** from the Homebrew Menu.

| Option | Meaning |
| --- | --- |
| `--bundle FILE` / `--toolkit ZIP` | where the toolkit comes from |
| `--out DIR` | SD card folder (default `./KartPadNX-SD`) |
| `--repo DIR` | the KartPadNX checkout to take `builder/*.sh` from (default: the one this file is in) |
| `--data DIR` | work folder (default `~/.local/share/kartpadnx-builder`) |
| `--no-rr` | base game only; Retro Rewind is built by default, as the Windows checkbox is |
| `--link gnu\|lld` | `gnu` (default) links with devkitPro's own GNU ld, like the Docker build; `lld` links with LLVM lld through `nxtools.py`, like the Windows builder |
| `--download-tools` | ignore system tools; use the pinned downloads |
| `--jobs N` | parallel jobs (default: from CPU count and RAM) |

Builds resume: after an error or Ctrl-C, run the same command again. Logs are in
`<data>/logs`; attach the newest one to an issue.

When Retro Rewind updates, run the build again and copy the whole `KartPadNX-SD` folder over
again, same as on Windows. Do not update Retro Rewind on the card by itself.

## What differs from the Windows builder

- Tools: system `cmake`, `ninja`, `dotnet`, LLVM when usable; otherwise downloads (`manifest/tools-linux.json`).
- devkitPro: the Docker layer is a linux/amd64 image, so its real `aarch64-none-elf-g++` (GNU ld), `elf2nro`, `nacptool`, `bin2s`, `uam` and `pkg-config` are kept and used. `nxtools.py` fills in only for a program the layer does not contain (a small shell script named after it runs the port), or for the link step with `--link lld`.
- No GUI, no `nxshim.exe`, no Defender-related `.data` split. Progress is printed as text; `--json-events` gives the Windows `@@` lines.
- Executable bits and symlinks are preserved when unpacking.

## Verified, and not yet verified

Confirmed by a full build on Fedora (log excerpts: tools under `<data>/tools`, no `nxtools` or lld lines):

- cmake, dotnet and LLVM downloads from `tools-linux.json` work, and the LLVM tarball name is right.
- The link with devkitPro's real `aarch64-none-elf-g++` (GNU ld) via `link-switch.sh` works for both games.
- The toolkit's prebuilt libraries are reused (20 of 20 targets for each game).
- Retro Rewind is built from the official feed's newest version (6.13.5 in that run) and its full pack
  lands in `switch/kartpad/retro-rewind/RetroRewind6`.

Not verified:

1. **Download pins.** Linux tool downloads have no size or SHA-256 in `tools-linux.json`. The builder logs
   the SHA-256 of each unpinned download (`grep "not pinned" <data>/logs/*.log`); check and add them.
   The ninja and nodtool downloads were not seen in the logs.
2. **Other distributions and Python versions.** Python below 3.14 needs `pip install zstandard` or the
   `zstd` program for the launcher packing (that code path has not run for real). System cmake, dotnet and
   LLVM (instead of the downloads) are only unit-tested.
3. **`--link lld`** (the Windows builder's link method) has not been run on Linux.
4. **The Windows code paths** are the original code behind `IS_WIN`, unchanged but not run after these edits.

## What was tested

38 checks on a Linux box without network, a disc, or the toolkit: tar/zip extraction (modes, relative
and absolute symlinks, hard links, include filters), devkitPro prefix rewriting, wrapper installation and
switching between GNU and lld link modes, the wrapper scripts actually running `nxtools.py`
(`nacptool` produces a valid-size NACP), the generated CMake command (no `.exe`, correct linker), manifest
overlay, system tool detection and version rejection, Wii FST parsing and the FNV hash, progress
output, and a full `main()` run with fake tools up to the disc check. `python3 -m py_compile` passes on
all changed files, and every Windows code path is the original code behind `IS_WIN`.

## Files

```
aio/pipeline/kpnx_aio.py          cross-platform pipeline (replaces the original; see aio-linux.patch)
aio/pipeline/nxtools.py           only the executable names changed
aio/manifest/tools-linux.json     Linux tool entries, laid over tools.json
aio/linux/kartpadnx-builder       launcher script
```

The code is GPL-3.0 like the rest of the repository. Nothing here contains Nintendo code or data.
