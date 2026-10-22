# Patches to upstream sources

The port builds pinned upstream trees with these changes. They are applied in this order, and
each step is idempotent: an already patched tree is detected and left alone.

| Order | Patch | Applied to | By |
|---:|---|---|---|
| 1 | `wiicompiled-switch-complete.patch` | WiiCompiled runtime + Aurora, when staging `build/wiicompiled-fpscr` | `builder/stage-runtime.py` |
| 2 | `wiicompiled-switch-runtime.patch` | staged runtime: libnx fibers, guest memory, entry point | `build-fullapp.sh patch` |
| 3 | `wiicompiled-switch-fullapp.patch` | staged runtime: Switch build arms, SD paths, NAND/DVD roots | `build-fullapp.sh patch` |
| 4 | `wiicompiled-switch-input.patch` | staged runtime: Classic Controller HLE over libnx HID | `build-fullapp.sh patch` |
| 5 | `runtime/` (see `runtime/series`) | staged runtime + Aurora: everything KartPad changed since 2026-09-23 | `build-fullapp.sh patch` |
| - | `dawn-switch-complete.patch`, `dawn-abseil-switch.patch` | Dawn and its abseil, for the Switch Dawn Null build | `build-graphics.sh` |
| - | `sdl-switch-external-graphics.patch` | SDL3: the window is drawn by the game renderer; no EGL/GLES | `build-graphics.sh` |
| - | `sdl-switch-audio-queue.patch` | SDL3: AUDOUT double buffering and buffer ownership | `build-graphics.sh` |

`builder/integrate-deko.py` then connects the native deko3d renderer (`switch/deko`) to the
staged Aurora during `build-fullapp.sh configure`.

## runtime/

One patch per subsystem: platform, CPU semantics, OS and scheduling, storage, audio, GX HLE,
input, network, Aurora GX, Aurora graphics and caches, Aurora input. Each file is in exactly one
patch. They are diffs against the 2026-09-23 staged baseline (after patches 1-4), so they can
be reviewed one subsystem at a time.

The staged tree is the working copy. After changing it:

```bash
python3 builder/regen-runtime-patches.py          # rewrite runtime/*.patch and series
python3 builder/tests/check-perf-patch-replay.py  # baseline + series == staged tree
```

A new file must be added to a group in `regen-runtime-patches.py`; the script refuses files that
belong to no group.
