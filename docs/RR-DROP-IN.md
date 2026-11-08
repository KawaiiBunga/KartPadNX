# Drop-in Retro Rewind code updates: what it would take

Goal: copy a new `RetroRewind6` folder to the SD card and play, with no rebuild, even when
`Code.pul` changed. Status: **not started**. What ships today instead: the builder takes the newest
RR from its own feed (no approval, no push), the web builder rebuilds by itself, and file mods load
at runtime (launcher Mods tab -> `Config.toml [paths] overlay_roots`).

## Why a new Code.pul needs a rebuild today

Nothing loads `Code.pul` at runtime. `translate-mod` bakes all of it in:

- the module image at `0x81800000` (`kModuleImage`, `mod_data_patches.cpp`), with Kamek relocations
  already applied;
- 4,096 translated mod functions (`mkw_retro_rewind_functions`);
- about 600 base functions replaced whole (`overlay_<addr>.bin`, the "sensitive" base shards),
  because Kamek hooks patch instructions in the middle of game functions, and leaf inlining would
  otherwise skip a hook;
- per-call register sync: a translated caller writes back only the registers its callee read
  *at translation time* (`GprReadMask` in `resolved_dispatch_profile.json`; see any
  `func_*.cpp` call site). New hook code that reads another register (Kamek hooks often read the
  caller's r30/r31) would see stale values.

So the new code has nothing to run on, and even if it had, the base game's calls into it would
pass the wrong registers.

## The design that would work

1. **Runtime Kamek loader** (~300 lines): parse `Code.pul` from the SD card (format:
   `translator/.../Parsing/Kamek/KamekPulFile.cs`, `KamekChunk.cs`), copy it to `0x81800000`,
   apply relocations and patches to guest memory, run its constructors.
2. **PowerPC interpreter** for code with no translation (~2,500 lines): decode Broadway
   integer/FP/paired-single instructions and execute them on `CpuContext` with the same helpers the
   translated code uses (`ppc_isa_float.h`, `ppc_isa_quantized.h`, `ppc_isa_int.h`), so FPSCR and
   quantization match. `bl` to a translated, unpatched function calls it natively; everything else
   is interpreted.
3. **Per-function validity gate** (translator change): each translated function records a hash of
   the guest instructions it was translated from (plus the functions inlined into it). At load,
   after Kamek patching, any function whose bytes changed is marked invalid; its entry jumps to the
   interpreter. The build-time RR version stays fully native; only what a newer `Code.pul` changed
   runs interpreted until the next build.
4. **Full register sync at hookable boundaries** (translator change): calls into functions that
   can be patched must write back every volatile and nonvolatile register, not the callee's
   build-time read mask. Doing it for every call costs FPS everywhere; doing it only for the
   functions today's RR already hooks (about 600) keeps the cost small, but a new hook outside that
   set would still need a rebuild (the loader can detect that and say so).
5. Mod functions move between versions (absolute addresses inside the module change), so matching
   build-time translations by address mostly fails; match by content with relocations masked.

## Cost and risk

- Weeks of work, two parts in the translator (C#, needs a Docker source build) and two in the
  runtime patches.
- Interpreted code is roughly 30-100x slower than translated code. Changed RR code that runs every
  frame (physics, item, HUD hooks) would cost FPS until the next build; the port isn't at a locked
  60 yet.
- Correctness has to be proven on hardware (interpreter vs translated results), across a full GP
  and online.
- The Retro-WFC payload (also translated at build time) has the same problem and would use the same
  loader.

Worth doing only if rebuilds stay painful after the feed-based builder and the web builder.
