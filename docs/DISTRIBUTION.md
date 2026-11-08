# Distribution and local data policy

KartPad-NX distributes port/builder source under the applicable open-source licenses. It must **not** distribute Nintendo game code/assets, translated Nintendo C++, generated game-data initializers, private mod payloads, or a game NRO containing those derivatives.

## Current user flows

**PC builder (AIO, `aio/`):**

1. The user supplies their own Mario Kart Wii **PAL RMCP01 rev0** dump.
2. The builder, on the user's PC, extracts the disc, translates the game and builds the derivative
   game NROs, packs them into `mkw-launcher.nro`, and writes the SD card folder: the launcher, the
   extracted disc at `switch/kartpad/data` with its completion manifest, and Retro Rewind's own
   pack from its CDN.
3. The game checks the completion manifest and uses that tree as its default `dvd_root`.

The launcher can also import a dump on the console (Game Files > Import disc, or the standalone
extractor); that path is hardware-verified (2032 files / 2,682,689,784 bytes from a real dump).

**Web builder (`web/`):** the same pipeline runs on the maintainer's PC on a dump the user
uploads, and the user downloads the resulting SD card folder as a zip. That zip holds the derivative
game NROs and the files of the uploaded disc, so serving it is distribution of both from the
maintainer's machine, even though it goes only to the uploader, the upload is deleted after the
build and the result after 24 hours. This conflicts with the policy above; running the web builder
publicly is the maintainer's own decision and risk. Nothing disc-derived is ever committed or put
in a release.

## SD layout

Typical local layout:

```text
sdmc:/switch/
  mkw-launcher.nro            launcher with both locally built game NROs packed in

sdmc:/switch/kartpad/
  Config.toml
  kartpad-runtime.log
  data/                       the extracted disc
    sys/
      fst.bin
      ...
    files/
      ...
    .kartpad-extraction-complete
  retro-rewind/RetroRewind6/  Retro Rewind's own pack, the version that was built
  mods/<name>/                user file mods, switched on per game in the launcher
```

The game NROs contain translated/generated code/data derived from the user's dump, so they remain local derivatives and must not be published with source releases. Game assets are not embedded in ROMFS.

## Disc decryption

No key prompt is required for the normal extractor flow. The Wii common key is built into the source/binary and selected by the ticket's `common_key_index`, matching the reference ports. An advanced user may place an exact 16-byte `sdmc:/switch/kartpad/common-key.bin` override; it is not required for the verified retail PAL extraction path.

## Extraction durability

The extractor stages and syncs output files, publishes them, performs a best-effort SD commit, and writes `.kartpad-extraction-complete` last. Failed/cancelled runs do not publish the completion manifest. A power loss during replacement can still leave an individual file absent because device/filesystem metadata durability is not fully proven. The runtime therefore treats the final manifest as the completed-run sentinel rather than accepting arbitrary partial output.

## Profiles and private inputs

| Profile | Local/private inputs |
|---|---|
| base | supported PAL dump |
| online | dump plus Retro WFC payload |
| retro-rewind | dump, Retro Rewind pack/Code.pul and Retro WFC payload |

Online and Retro Rewind translation graphs exist, but that is not proof of Switch gameplay or networking support. Pack/payload inputs remain local and must not be bundled with source releases.

## Source-only diagnostic software

The Dawn/NVK graphics probe and synthetic G7 GX fixture contain no Mario Kart translation or game assets. They are distinct from the derivative full-game NRO and can be handled according to their own source/dependency licenses.

## Local-only trees

`iso/`, `private/`, `build/`, `dist/`, crash logs and hardware artifacts are local working data. Do not sweep these into a public source release. Dependency source trees in `ref/` retain their own licenses and may contain local port changes whose reproducibility is tracked separately.
