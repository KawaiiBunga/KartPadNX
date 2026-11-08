# KartPadNX

Mario Kart Wii running natively on the Switch, Retro Rewind included.

This isn't an emulator. The game's PowerPC code gets translated to C++ ahead of time and
compiled for the Switch, using [WiiCompiled](https://github.com/patchzyy/Wiicompiled) and
[KartPad](https://github.com/chrissotraidis/kartpad). Because of that, the game is never
downloaded from here. It's built from your own copy of the disc.

## What you need

- A Switch running homebrew (Atmosphère)
- About 5 GB free on your SD card
- Your own Mario Kart Wii disc image: **PAL (RMCP01), revision 0**, as `.iso`, `.wbfs` or `.rvz`.
  NTSC (RMCE01), Japanese and Korean discs, and later PAL revisions don't work.

Then build it one of two ways. Both give you the same SD card files.

## Option 1: the web builder (no PC setup)

**https://kawaiibunga.github.io/KartPadNX/**

1. Pick your disc image. The site checks it's the right version before uploading.
2. Leave **Include Retro Rewind** ticked if you want it, confirm the disc is yours, and click
   **Upload and build**. The build runs on the maintainer's PC; you can close the tab and come
   back.
3. Download the zip (about 6 GB), unzip it, and copy everything inside to the root of your SD
   card.

The builder only works while the maintainer's PC is on; the page says when it's offline. Your
upload is deleted as soon as your build ends, and your files after 24 hours.

## Option 2: build on your own PC

You also need a Windows 10 or 11 PC (64-bit) with about 15 GB free and 8 GB of RAM.

1. Download `KartPadNX-Builder.zip` from
   [Releases](https://github.com/KawaiiBunga/KartPadNX/releases), unzip it and run
   `KartPadNX-Builder.exe`. Windows might warn you because it isn't signed: click **More info**,
   then **Run anyway**.
2. Pick your disc image, leave **Retro Rewind** ticked if you want it, and click **Build**.
   The first build takes 30 to 60 minutes. Nothing gets installed on your PC.
3. When it's done, copy everything in the **SD card** folder to the root of your SD card.

## Playing

On the Switch, hold **R** while starting any game to open the Homebrew Menu, and start
**MKW Launcher**. There's nothing to import: the SD card files already hold the launcher with both
games inside, your game files, and Retro Rewind's own files in the version that was built.

Start it from a game this way, not from the Album: from the Album the Switch gives homebrew too
little memory, and the launcher shows a warning.

In the launcher, pick **Mario Kart Wii** or **Retro Rewind** and press **Play** (or **+**). The
other tabs: **Mods** (below), **Saves** (backups, made automatically before each start),
**Licenses** (names, Miis, VR/BR), **Miis** (your Switch Miis, used by both games), **Game Files**
(re-import or check the disc, add Retro Rewind) and **Settings** (volume, FPS counter,
widescreen, launcher updates).

## Retro Rewind updates

When Retro Rewind updates, build again (on the site or with the PC builder) and copy the new files
over the old ones. Your saves are kept. The builder always takes the newest Retro Rewind straight
from Retro Rewind's own update feed, so there's nothing to wait for; only Retro Rewind gets
rebuilt, and its files come out matching the game.

Don't update Retro Rewind on the SD card by itself (with Wheel Wizard, or by copying a new
`RetroRewind6` folder over). The game has Retro Rewind's code built in, so a different version
on the card won't line up with it.

## Mods

Mods that replace game files (textures, tracks, music, or a Riivolution pack with a `riivolution`
folder) need no rebuild:

1. Copy each mod's folder to `/switch/kartpad/mods/` on the SD card. Inside, files are laid out
   like the disc (for example `Race/Course/...`), or it's a Riivolution pack with a `riivolution`
   folder.
2. In the launcher's **Mods** tab, pick the game and switch the mods on. Each game has its own
   list, and a game's card shows how many mods are on.

Mods that change the game's code (Kamek/Pulsar `.pul` code, Gecko codes) aren't supported this way.

## If something goes wrong

- **The game crashes or won't start:** send `/switch/kartpad/kartpad-runtime.log` and the newest
  file in `/atmosphere/crash_reports/` from your SD card when you
  [open an issue](https://github.com/KawaiiBunga/KartPadNX/issues). Say what you were doing
  (menu, which track, how long in).
- **"Low memory mode" in the launcher:** start it by holding R on a game, not from the Album.
- **Something looks broken after an update:** in the launcher, Settings > Reset performance
  tweaks (only shown if you changed them), or Game Files > Check game files.
- **PC builder:** it keeps a log in its work folder under `logs`; attach the newest one to an
  issue. Builds pick up where they stopped, so after an error or a cancel just click **Build**
  again. Some antivirus programs flag it: it's new, unsigned, and downloads and runs compilers in
  the background, which looks a bit like what malware does. It isn't.
- **Web builder:** if it says the disc is the wrong version, it really is: only PAL revision 0
  works. If the upload stops, reload the page and pick the same file again; it carries on where
  it left off.

## Status

- The base game and Retro Rewind are playable: menus, races, Grand Prix.
- Retro WFC online connects and races start, but it's still being tested.
- It runs below a locked 60 fps for now. Performance is what I'm working on most.

## What's in this repo

| Folder | What it is |
|---|---|
| [`aio/`](aio/README.md) | The Windows builder and the scripts that make its releases |
| [`web/`](web/README.md) | The web builder: the site (GitHub Pages) and the build server it talks to |
| `switch/` | The Switch port: runtime glue, the deko3d renderer, CMake, patches |
| `launcher/` | MKW Launcher: disc import, mods, profiles, saves, Miis, settings |
| `builder/` | Scripts for building everything from source with Docker |
| `ref/kartpad-main/` | The few KartPad files this port needs |
| `docs/` | [How it works](docs/ARCHITECTURE.md), [building from source](docs/BUILDING-SWITCH.md), [dependencies](docs/DEPENDENCIES.md), [what can be shared](docs/DISTRIBUTION.md), [perf.cfg settings](docs/PERF-CFG-KEYS.md), [drop-in Retro Rewind updates (in progress)](docs/RR-DROP-IN.md) |

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
builders don't download any. Everything game-related comes from your own disc. What you build is
for you: please don't share it.

The code is GPL-3.0 ([LICENSE](LICENSE)), since it builds on WiiCompiled and KartPad, which
are both GPL-3.0. Bundled libraries keep their own licenses (see
[docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) and `switch/deko/THIRD_PARTY.md`). Mario Kart Wii
belongs to Nintendo. This project isn't affiliated with or endorsed by Nintendo.

## Support

If you like what I do, you can support me here: https://ko-fi.com/kawaiibunga
