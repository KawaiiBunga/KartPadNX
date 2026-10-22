# builder/

Scripts for the full source build. Build scripts run in the `kartpad-dawn` Docker image with
this checkout at `/project` ([docs/BUILDING-SWITCH.md](../docs/BUILDING-SWITCH.md)); every
script's header has its full usage.

| Script | Does |
|---|---|
| `fetch-deps.sh` | clone the pinned upstreams into `ref/` |
| `stage-runtime.py` | stage WiiCompiled + `switch/patches/wiicompiled-switch-complete.patch` into `build/wiicompiled-fpscr` |
| `build-switch.sh` | extract your disc and translate it (`--stage extract`, `--stage translate --profile ...`) |
| `translate-profile.py`, `translator-post-hooks.py`, `apply-translator-reload-elision.py` | profile translation, KartPad's translation hooks, the translator's reload elision |
| `build-graphics.sh`, `prepare-deko.py` | Dawn Null + Tint and SDL3 for the Switch |
| `build-fullapp.sh <base\|retro-rewind> <patch\|configure\|transform\|build\|all>` | the game NRO |
| `integrate-deko.py`, `link-switch.sh` | connect `switch/deko` to Aurora; the final link |
| `inject-*.py`, `fix-rr-menu-model-flow.py`, `optimize-translated-compares.py`, `elide-redundant-flushes.py` | source passes over the translation (run by `build-fullapp.sh transform`) |
| `generate-native-model.py`, `generate-native-scene.py`, `capture-flat-base.py` | native regions generated from your translation (run by CMake) |
| `regen-runtime-patches.py`, `regen-sdl-audio-patch.py` | write `switch/patches/` back from an edited staged tree |
| `build-launcher.sh <nopayload\|payload> build`, `pack-payloads.py` | the MKW Launcher; `payload` packs your games in (local only) |
| `build-release.sh`, `make-manifest.py`, `sign-manifest.py`, `list-nro-romfs.py` | a public, game-free launcher release with a signed update manifest |
| `check-deko-artifacts.py`, `check-launcher-deko-payloads.py` | check built images |
