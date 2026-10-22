# Building KartPad-NX for Switch

Current status: the translated base game and Retro Rewind both build and play on hardware. The game renders through native deko3d (Dawn Null supplies WebGPU objects and Tint shader translation; Mesa/NVK is not linked into the game). Retro WFC online play is under test. Performance work toward a locked 60 fps is ongoing.

## Targets

| Target / stage | Result |
|---|---|
| Default Switch CMake | platform shim/static libraries |
| `KARTPAD_BUILD_GRAPHICS=ON` | Aurora against local Switch Dawn/SDL3 |
| `KARTPAD_ENABLE_FULL_APP=ON` | real translated base-game target via `FullApp.cmake` |
| `builder/build-switch.sh --stage extract` | host extract/validate supported PAL dump |
| `builder/build-switch.sh --stage translate --profile ...` | translation graph generation |
| `builder/build-graphics.sh [dawn\|sdl\|all]` | Dawn Null + Tint and SDL3 dependencies |
| `builder/build-fullapp.sh [base\|retro-rewind] patch` | apply runtime/Aurora patches to the staged tree |
| `builder/build-fullapp.sh [base\|retro-rewind] configure` | patch, then configure CMake |
| `builder/build-fullapp.sh [base\|retro-rewind] transform` | apply the translation passes in place (idempotent) |
| `builder/build-fullapp.sh [base\|retro-rewind] build` | compile only; never changes the translation or staged runtime |
| `builder/build-fullapp.sh [base\|retro-rewind] all` | patch + configure + transform + build |

`builder/build-switch.sh` is still the extraction/translation driver; it is not the full NRO packager. Use `build-fullapp.sh` for the game NRO.

## Prerequisites

- devkitPro/devkitA64/libnx.
- Docker image `kartpad-dawn` from `switch/docker/Dockerfile.dawn` (Clang 19).
- CMake 3.25+ and Ninja inside the graphics container.
- Prepared `ref/kartpad-main` (translation staging), staged WiiCompiled runtime/Aurora under `build/wiicompiled-fpscr`, and translated base graph under `private/self-build/translation`.
- Switch Dawn under `ref/dawn` / `build/dawn-switch-deko` (Dawn Null + Tint for the deko3d renderer).
- external-graphics SDL3 under `ref/SDL` / `build/sdl-switch-vulkan`.
Pins are listed in [DEPENDENCIES.md](DEPENDENCIES.md); the patches in [switch/patches/README.md](../switch/patches/README.md).

Do not rebuild Dawn using native Windows CMake. The working recipe uses Docker/POSIX paths; native Windows/msys path mixing previously broke Dawn's generated paths.

## Docker invocation from PowerShell

From the repository root:

```powershell
# Only if the image is missing:
docker build -f switch/docker/Dockerfile.dawn -t kartpad-dawn switch/docker

$kartpadMount = (Get-Location).Path + ':/project'
```

Graphics dependencies (once, or after a Dawn/SDL change):

```powershell
docker run --rm -v $kartpadMount -w /project -e DEVKITPRO=/opt/devkitpro kartpad-dawn bash builder/build-graphics.sh all
```

`dawn` and `sdl` build one of them. The source trees come from `builder/fetch-deps.sh`; the script
patches them idempotently.

## Full base-game NRO

After the base translation and graphics dependencies exist:

```powershell
docker run --rm -v $kartpadMount -w /project -e DEVKITPRO=/opt/devkitpro kartpad-dawn bash builder/build-fullapp.sh all
```

For an incremental rebuild (compile only; the translation and staged runtime are not touched):

```powershell
docker run --rm -v $kartpadMount -w /project -e DEVKITPRO=/opt/devkitpro kartpad-dawn bash builder/build-fullapp.sh build
```

`build-fullapp.sh all`:

1. `patch`: applies the runtime, full-app, input, performance and FP-compare patches idempotently to the staged runtime, plus the batched DVD directory index;
2. `configure`: configures `switch/CMakeLists.txt` with `KARTPAD_ENABLE_FULL_APP=ON`;
3. `transform`: applies the translation passes in place. The preserved-register boundary passes always run. The CPU passes (FP compare reuse, flush elision) run for `retro-rewind` and are off for `base` by default, because the hardware-tested base NRO was built without them; `KARTPAD_CPU_REWRITES=1` or `0` overrides;
4. `build`: compiles the runtime and translated graph and links the deko3d/libnx stack;
5. emits `build/switch-fullapp/kartpad.nro` plus the matching `kartpad_full.elf` and `.map`.

Every build relinks a fresh build stamp, so the NRO hash changes even when no code changed. Compare builds by symbol table (`aarch64-none-elf-nm -S`), not NRO hash.

Preserve the ELF/map that correspond to each hardware-tested NRO. Atmosphère reports contain module-relative PCs that can be symbolized with devkitA64 `aarch64-none-elf-addr2line`.

The NRO contains locally translated Nintendo code/data and is **not distributable** under this project's policy.

## Retro Rewind NRO (Retro Rewind + Retro WFC)

To build the Retro Rewind variant (`kartpad-retro-rewind.nro`) using its dedicated build tree (`build/switch-fullapp-rr`):

```powershell
docker run --rm -v $kartpadMount -w /project -e DEVKITPRO=/opt/devkitpro kartpad-dawn bash builder/build-fullapp.sh retro-rewind all
```

Or for incremental rebuilds:

```powershell
docker run --rm -v $kartpadMount -w /project -e DEVKITPRO=/opt/devkitpro kartpad-dawn bash builder/build-fullapp.sh retro-rewind build
```

Alternatively set `KARTPAD_PROFILE=retro-rewind`:

```powershell
docker run --rm -v $kartpadMount -w /project -e DEVKITPRO=/opt/devkitpro -e KARTPAD_PROFILE=retro-rewind kartpad-dawn bash builder/build-fullapp.sh build
```

This build:
1. Points to the Retro Rewind translation directory (`private/self-build/retro-rewind/translation`);
2. Defines `MKW_PROFILE_RETRO_REWIND=1` and `MKW_ENABLE_RETRO_WFC=1`;
3. Compiles the 4,096 translated mod functions, sensitive shards, and mod data patches via `switch/cmake/RetroRewind.cmake`;
4. Automatically shares all runtime and translator performance optimizations (lean codegen, native model, etc.);
5. Uses PGO profile from `private/pgo/current-retro-rewind` (falling back to `private/pgo/current`);
6. Emits `build/switch-fullapp-rr/kartpad-retro-rewind.nro` with NACP title `"KartPad Retro Rewind"`.

Run it standalone from `sdmc:/switch/kartpad-retro-rewind/kartpad-retro-rewind.nro` (its own
folder, so hbmenu detects it), or pack both games into the launcher with
`builder/build-launcher.sh payload build`.

## CPU code-generation options

`build-fullapp.sh` passes these to CMake; `build` reconfigures by itself when one differs from
the build directory's cache.

| Variable | Default | Effect |
|---|---|---|
| `KARTPAD_OUTLINE_SCALAR_FP` | `ON` | keeps the finite scalar-FP fast paths inline and shares the exceptional fallbacks (`switch/src/scalar_fp_cold.cpp`); log tag `cpu=cold-fp` |
| `KARTPAD_THINLTO_SHARDS` | `OFF` | experimental ThinLTO across translated shards (`KARTPAD_THINLTO_JOBS`, default 1); log tag `cold-fp+thin-shards` |
| `KARTPAD_CPU_REWRITES` | on for `retro-rewind`, off for `base` | the `transform` stage's FP-compare reuse and flush elision |
| `KARTPAD_PGO` | on when `private/pgo/current[-retro-rewind]` names a profile | sample PGO and hot-function order |

Use a separate `KARTPAD_BUILD_DIR` to compare variants. Archive the exact ELF and verify the
console NRO by readback before comparing runs.

This requires matching `llvm-ar`, `llvm-ranlib` and `LLVMgold.so` for the
Clang major version. The Docker recipe installs the LLVM tools and linker
packages. It retains the existing GNU Switch linker and libnx specs. The
three shards defining native-intercepted guest functions remain native so
ThinLTO cannot bypass their `--wrap` dispatch. Configuration stops if a
wrapped definition cannot be uniquely identified. Runtime, native model,
audio and graphics libraries retain ordinary compilation. No fast-math or
implicit FP contraction is enabled for guest code.

Before using a new toolchain image, run its synthetic integration probe:

```sh
python3 builder/tests/probe-thinlto-switch.py /tmp/kartpad-thinlto-probe
```

The output directory must be new. It checks cross-file optimization, native
wrapper/fallback calls, `_start` at zero and valid NRO headers using only
first-party synthetic source. ARM64 production and strict ThinLTO FP checks
each passed 940,800 differential cases. Hardware performance still requires
an exact-build stock-clock lap; a smaller image alone does not establish a gain.

The native model region is generated locally from the user's translation by
`builder/generate-native-model.py`. Generated game C++/JSON is ignored and is
not a required tracked source file. The complete core-source patch can be
replayed onto pinned WiiCompiled with `builder/stage-runtime.py` in a fresh
output directory; its clean translator suite passed 585 tests. This does not
yet establish a fully validated dependency-to-NRO build from a fresh clone:
dependency provisioning and graphics patch integration are still being repaired.

## Testing on hardware

1. Copy the NRO to the SD card (keep the ELF and map of every build you test: Atmosphère crash
   reports give module offsets you symbolize with `aarch64-none-elf-addr2line` against them).
2. `sdmc:/switch/kartpad/data` must hold a completed extraction (`.kartpad-extraction-complete`,
   `sys/fst.bin`, `files/`); the launcher's Game Files screen makes it.
3. Keep NAND, saves and shader caches between runs; test a clean bootstrap only with a separately
   preserved test NAND.
4. Launch from the launcher or hbmenu in application mode.
5. Evidence of a run: `sdmc:/switch/kartpad/kartpad-runtime.log`, `perf.txt`, `Logs/` and the
   newest Atmosphère crash report.

The first runtime-log line names the build (`[build] KARTPAD-BUILD <stamp> ...`), so a stale NRO
on the card is visible. stdout/stderr go to `kartpad-runtime.log` before the text console is
released ([ARCHITECTURE.md](ARCHITECTURE.md)).

## Disc / translation stages

```bash
bash builder/build-switch.sh /path/to/RMCP01.wbfs --stage extract
bash builder/build-switch.sh --stage translate --profile base
bash builder/build-switch.sh --stage translate --profile online --payload /path/to/payload.bin
bash builder/build-switch.sh --stage translate --profile retro-rewind --payload /path/to/payload.bin --retro-root /path/to/RetroRewind6
```

Base and Retro Rewind are the full-app products (`build-fullapp.sh base|retro-rewind`).

## Without Docker

`aio/` builds the same NROs natively on Windows with portable tools (LLVM lld instead of
devkitA64's GNU ld). See [aio/README.md](../aio/README.md).
