# KartPadNX

Mario Kart Wii running natively on the Switch, Retro Rewind included.

This isn't an emulator. The game's PowerPC code gets translated to C++ ahead of time and
compiled for the Switch, using [WiiCompiled](https://github.com/patchzyy/Wiicompiled) and
[KartPad](https://github.com/chrissotraidis/kartpad). Because of that, the game is never
downloaded from here. You build it yourself on your PC from your own copy of the disc.

## Download

**[Download the builder (KartPadNX-Builder.zip)](https://github.com/KawaiiBunga/KartPadNX/releases/latest/download/KartPadNX-Builder.zip)**,
or grab any version from the [Releases page](https://github.com/KawaiiBunga/KartPadNX/releases).

You need:

- A Windows 10 or 11 PC (64-bit), about 10 GB free and 8 GB of RAM
- Your own Mario Kart Wii disc image: **PAL (RMCP01), revision 0**, as `.iso`, `.wbfs` or `.rvz`
- A Switch running homebrew

## How to use it

1. Unzip `KartPadNX-Builder.zip` and run `KartPadNX-Builder.exe`. Windows might warn you
   because it isn't signed: click **More info**, then **Run anyway**.
2. Pick your disc image, leave **Retro Rewind** ticked if you want it, and click **Build**.
   The first build takes 30 to 60 minutes. Nothing gets installed on your PC.
3. When it's done, copy the `switch` folder from the output folder to the root of your SD
   card, and put your disc image in `sd:/iso`.
4. On the Switch, open the Homebrew Menu by holding **R** while starting a game, launch
   **MKW Launcher**, and import your disc under **Game Files**.

### Retro Rewind

Retro Rewind needs its own files on the SD card, and they have to be the exact version the
builder built. Leave **Also download the Retro Rewind SD files** ticked (about 1.9 GB) and the
builder puts the right ones in the output folder as `RetroRewind6`. Copy that folder to the
root of your SD card too, then import it under **Game Files**.

If you untick it, download the **full pack** of the version named in `INSTALL.txt` from
[rwfc.net](https://rwfc.net), copy its `RetroRewind6` folder to `sd:/RetroRewind6`, and import
it under **Game Files**. The folder has to contain `xml/RetroRewind6.xml`. If the launcher
says that file is missing, you copied an update instead of the full pack.

When Retro Rewind updates, wait until the new version is supported here, then click **Build**
again. Only Retro Rewind gets rebuilt, and the `RetroRewind6` folder is updated too.

### If something goes wrong

- The builder keeps a log in its work folder under `logs`. Attach the newest one if you open
  an issue.
- Builds pick up where they stopped, so after an error or a cancel just click **Build** again.
- Some antivirus programs flag the builder. It's new, unsigned, and downloads and runs
  compilers in the background, which looks a bit like what malware does. It isn't.

## Status

- The base game and Retro Rewind are playable: menus, races, Grand Prix.
- Retro WFC online connects and races start, but it's still being tested.
- It runs below a locked 60 fps for now. Performance is what I'm working on most.

## What's in this repo

| Folder | What it is |
|---|---|
| [`aio/`](aio/README.md) | The Windows builder and the scripts that make its releases |
| `switch/` | The Switch port: runtime glue, the deko3d renderer, CMake, patches |
| `launcher/` | MKW Launcher: disc import, profiles, saves, Miis, settings |
| `builder/` | Scripts for building everything from source with Docker |
| `ref/kartpad-main/` | The few KartPad files this port needs |
| `docs/` | [How it works](docs/ARCHITECTURE.md), [building from source](docs/BUILDING-SWITCH.md), [dependencies](docs/DEPENDENCIES.md), [what can be shared](docs/DISTRIBUTION.md), [perf.cfg settings](docs/PERF-CFG-KEYS.md) |

If you want to work on the port itself rather than just play it, start with
[docs/BUILDING-SWITCH.md](docs/BUILDING-SWITCH.md).

## Credits

This was only possible because of other people's work:

- **[WiiCompiled](https://github.com/patchzyy/Wiicompiled)** by patchzyy: the static
  recompiler and runtime everything here is built on
- **[KartPad](https://github.com/chrissotraidis/kartpad)** by chrissotraidis: Mario Kart Wii
  on phones and Mac, and the translation patches, configs and Retro Rewind support this port
  starts from
- **[Aurora](https://github.com/encounter/aurora)** and **[nod](https://github.com/encounter/nod)**
  by Luke Street (encounter): the GameCube/Wii graphics layer, and the disc reader the
  builder uses
- **[SwitchWaker](https://github.com/centollOS/SwitchWaker)** by centollOS: the deko3d
  renderer code and shader compiler fixes this port adapted
- **[devkitPro](https://devkitpro.org)** and **[switchbrew](https://github.com/switchbrew)**:
  devkitA64, libnx, deko3d and uam (fincs)
- **[Retro Rewind](https://rwfc.net)** and **Retro WFC**: the mod pack and the online
  service
- **[Dawn and Tint](https://dawn.googlesource.com/dawn)** (Google),
  **[SDL3](https://github.com/libsdl-org/SDL)** and its
  [Switch port](https://github.com/neomody77/sdl3-switch) (neomody77),
  **[Mesa](https://mesa3d.org)** (the GLSL compiler inside uam),
  [sse2neon](https://github.com/DLTcollab/sse2neon), Crypto++, pugixml, toml11, zstd,
  xxHash, fmt, Dear ImGui, SQLite and Tracy

## Legal

This repo has source code only. No Nintendo code, game data or keys are included, and the
builder doesn't download any. Everything game-related comes from your own disc, on your own
PC. What you build is for you: please don't share it.

The code is GPL-3.0 ([LICENSE](LICENSE)), since it builds on WiiCompiled and KartPad, which
are both GPL-3.0. Bundled libraries keep their own licenses (see
[docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) and `switch/deko/THIRD_PARTY.md`). Mario Kart Wii
belongs to Nintendo. This project isn't affiliated with or endorsed by Nintendo.

## Support

If you like what I do, you can support me here: https://ko-fi.com/kawaiibunga
