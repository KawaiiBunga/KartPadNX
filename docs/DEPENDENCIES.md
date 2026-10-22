# Dependencies

What the game and launcher are built from, where each pin lives, and what KartPad changes in it.
`builder/fetch-deps.sh` clones the git pins into `ref/`; the rest come from the toolchain image
or are vendored in this repository.

## Pinned upstream sources

| Dependency | Pin | Pinned in | KartPad changes |
|---|---|---|---|
| WiiCompiled (runtime, Aurora, translator) | `patchzyy/Wiicompiled` `1912292c804ff9b1b79938de89369ec4496f9fff` | `fetch-deps.sh`, `stage-runtime.py` | `switch/patches/wiicompiled-switch-*.patch`, `switch/patches/runtime/` |
| Dawn (Null backend, Tint) | `google/dawn` `80ee0043018a51532ea0fa2e77496cc66634157e` | `fetch-deps.sh` | `dawn-switch-complete.patch`, `dawn-abseil-switch.patch`, Tint GLSL I/O edits by `prepare-deko.py`; Dawn fetches its own third-party sources during the graphics build |
| SDL3 | `libsdl-org/SDL` `5848e584a1b606de26e3dbd1c7e4ecbc34f807a6` | `fetch-deps.sh` | |
| SDL3 Switch port | `neomody77/sdl3-switch` `182e511214d7600e4bdab8606d7caf0ef744afd6` | `fetch-deps.sh` | `sdl-switch-external-graphics.patch`, `sdl-switch-audio-queue.patch` |
| sse2neon | `DLTcollab/sse2neon` `13a42df35dc7fcc94f987568e7274a998bb6cc86` (sha256 `44b9fa3d…`) | `stage-runtime.py` | |
| KartPad (`ref/kartpad-main`) | the subset this port uses is kept in this repository | — | its 7 WiiCompiled translator patches, recompiler configs, profile metadata, 4 translation hooks |

## Vendored in this repository

| Component | Where | Origin and changes |
|---|---|---|
| UAM shader compiler (Mesa GLSL/TGSI + nouveau codegen) | `switch/uam` | devkitPro/uam 1.1.0 `5a5afc2` via SwitchWakerHD `df8fbde`; changes listed in `switch/uam/PATCHES.md`; zlib + MIT notices in `switch/uam/LICENSE` |
| KartPad runtime headers | `switch/include` | copied unchanged from KartPad's `runtime/include` (PPC semantics, host bridge, gamepad contract) |
| deko3d device, memory, descriptors, shader worker, WGSL-to-GLSL, object tracking | `switch/deko` | centollOS/SwitchWaker `04372277`, MIT (some files MPL-2.0); KartPad adaptations listed in `switch/deko/THIRD_PARTY.md` |
| nlohmann/json | `launcher/include/nlohmann/json.hpp` | single-header release |

## Toolchain image

`switch/docker/Dockerfile.dawn` (`kartpad-dawn`): `devkitpro/devkita64:latest` (libnx, deko3d,
libstdc++ for devkitA64) plus clang-19 from apt.llvm.org. The base image tag is not pinned to a
digest; record the image ID with any build you archive.

## Not needed

Mesa/NVK (`ref/mesa-switch-main`) belonged to the Vulkan-era renderer. The deko3d game build does
not compile or link anything from it.
