# KartPad-NX architecture

How the port is put together, and the constraints a change must keep. Build steps are in
[BUILDING-SWITCH.md](BUILDING-SWITCH.md); current status is in the [README](../README.md).

## Products

| Product | Source | What it is |
|---|---|---|
| Game NRO, base | `switch/`, staged runtime, local translation | `build/switch-fullapp/kartpad.nro` |
| Game NRO, Retro Rewind | same, plus the RR mod translation | `build/switch-fullapp-rr/kartpad-retro-rewind.nro` |
| MKW Launcher | `launcher/` | disc extraction, profiles, saves, Miis, settings, updates; starts a game NRO |

The game NROs contain translated Nintendo code and are local builds only
([DISTRIBUTION.md](DISTRIBUTION.md)). The public launcher is game-free; a local "payload" launcher
can carry both game NROs zstd-compressed in its RomFS and unpacks one to
`sdmc:/switch/mkw-launcher/.games/` before launching it with `envSetNextLoad`.

## From disc to NRO

```text
user's RMCP01 disc ──extract──> dvd_root on SD (sys/fst.bin, files/...)
main.dol + StaticR.rel ──translate (WiiCompiled recompiler)──> C++ shards + data initializers
shards ──transform (builder passes: ABI boundaries, optional CPU rewrites)──> build input
staged runtime + Aurora (+ switch/patches) ─┐
switch/src, switch/deko ────────────────────┼─ FullApp.cmake ──> kartpad_full.elf ──> NRO
Dawn Null, SDL3, libnx, deko3d ─────────────┘
```

There is no PowerPC JIT or interpreter: every game function is translated C++ compiled for
AArch64. Profiles are `base` and `retro-rewind`. The translated graph, data initializers and
generated native regions stay under `private/` and `build/`.

The staged runtime and Aurora (`build/wiicompiled-fpscr`) are a working copy of upstream
WiiCompiled plus `switch/patches` ([switch/patches/README.md](../switch/patches/README.md)).
Edit the staged tree, then regenerate the patches; a clean re-stage loses anything else.

## Runtime

```text
translated game functions
        │ guest memory, guest calls (ppc_runtime.h, abi_bridge.h)
WiiCompiled runtime: OS/scheduler, DVD/NAND, AX audio, GX, input, network HLE
        │
Aurora GX state, shader generation, pipeline cache
        │ WebGPU API on Dawn's Null backend (object handles only)
switch/deko: native deko3d adapter (__wrap_wgpu* link-time wrappers)
        │
deko3d ──> libnx NWindow
```

### Guest memory

Translated code expects a flat 4 GiB guest space with coherent cached, uncached and physical
mirrors. `switch/src/runtime/guest_flat_memory_switch.cpp` reserves 4 GiB of address space with
libnx virtmem and backs MEM1/MEM2 with shared-memory objects in 4 MiB slices, each slice mapped
at all its mirrors. One large backing object fails on hardware with `KernelError_OutOfResource`.
The MEM2 arena spans both 32 MiB MEM2 slices (`SystemBridge::SeedLowMemDefaults`); a 32 MiB arena
made archive decompression stomp MEM1.

### Threads, fibers and TLS

The guest scheduler runs guest threads as host fibers: `switch/src/aarch64_fiber*` implement an
AArch64 callee-saved coroutine, wired into `fiber_manager.cpp` by `wiicompiled-switch-runtime.patch`.

Clang reads TLS through `tpidr_el0`; libnx's GCC soft-TLS uses `__aarch64_read_tp()`.
`switch/src/clang_tls_switch.c` copies libnx's TLS base into `tpidr_el0` in an early constructor
and in a `pthread_create` wrapper, which also gives every worker an 8 MiB stack (Tint needs it);
`pthread_join` frees it. `exit()` is wrapped to `_exit()`: the full C++/libnx teardown overflowed
the main stack on hardware. Thread roles and core placement are in `switch/src/perf_switch.cpp`.

### Console and logs

The libnx software console and the renderer share the default NWindow. `main_switch.cpp`
redirects stdout/stderr to `sdmc:/switch/kartpad/kartpad-runtime.log` **before** `consoleExit()`:
`consoleExit()` frees the framebuffer but output still reaches the console devoptab, and the
first print afterwards faulted.

### Game data

`sdmc:/switch/kartpad/data` holds the extracted disc (`sys/fst.bin`, `files/`, and the
`.kartpad-extraction-complete` manifest). The launcher normally extracts it;
`runtime_host_switch.cpp` can extract from a dump on the SD card when it is missing. Saves and
NAND live under `sdmc:/switch/kartpad/NAND`, settings in `Config.toml` and `perf.cfg`, caches in
`Cache/`, logs in `kartpad-runtime.log` and `Logs/`.

### Input

`switch/src/input_switch.cpp` reads up to four libnx pads and maps them to the Classic Controller
contract (`switch/include/kartpad/android/gamepad_contract.h`). `wiicompiled-switch-input.patch`
feeds it to the KPAD/WPAD HLE as Classic Controllers.

## Graphics

Aurora keeps its WebGPU renderer. Dawn runs its Null backend, so WebGPU calls create real Dawn
objects but no GPU work; the `__wrap_wgpu*` functions in `switch/deko` (list:
`wrap-symbols.txt`) intercept the calls that matter and execute them with deko3d. See
`switch/deko/backend_internal.h` for the adapter's parts, locking and resource lifetimes.

Shaders: Aurora generates WGSL per pipeline; Tint translates it to GLSL; the UAM compiler
(`switch/uam`, one compiler thread) compiles it to DKSH. Three caches avoid recompiling:

| Cache | Key | Where |
|---|---|---|
| native stages (`dksh_local.bin`, journaled) | WGSL hash + stage + entry, compiler id | `sdmc:/switch/kartpad/Cache/deko/` |
| pipeline recipes (`pipeline_cache_deko.db`, SQLite) | Aurora `PipelineConfig` hash | `sdmc:/switch/kartpad/Cache/` |
| RomFS seeds: DKSH and recipes recorded from earlier sessions | as above | `romfs:/deko/`, copied to the SD when their id changes |

At boot every recorded recipe is rebuilt in the background in first-use order (prewarm), so
pipelines seen before do not compile while playing.

## Audio

The AX HLE mixes guest voices; SDL3's Switch audio backend (with
`sdl-switch-audio-queue.patch`: double-buffered AUDOUT, buffer ownership) plays them. The mix runs
on a hosted guest sound thread.

## Guest-call boundaries

A few guest calls need preserved registers republished around them (scene, archive, menu-model
and globe-scene boundaries; headers in `switch/src`, injected by `builder/inject-*.py`). They are
guards for observed corruption whose first producer is not yet identified; do not remove them
without that evidence. Never `PPC_NATIVE_OVERRIDE` an indirect-dispatch target such as
`0x8015df4c`: startup aborts with "Stale generated indirect dispatch winner".

## Launcher

`launcher/source` is split into `app` (loop, input, overlay), `screens`, `ui`, and services in
`data` (saves, licenses, Miis, settings), `disc` (extraction), `payload` (unpack, launch) and
`net` (signed updates: `source/net/README.md`). Screens call services; services own files.
