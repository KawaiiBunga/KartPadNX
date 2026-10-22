# Distribution and local data policy

KartPad-NX distributes port/builder source under the applicable open-source licenses. It must **not** distribute Nintendo game code/assets, translated Nintendo C++, generated game-data initializers, private mod payloads, or a game NRO containing those derivatives.

## Current local-user flow

The implemented base flow is:

1. User supplies their own Mario Kart Wii **PAL RMCP01 rev0** dump.
2. The local builder extracts/translates the supported game and builds the derivative Switch NRO locally.
3. The user runs the standalone KartPad disc extractor on Switch to materialize the disc DATA partition at `sdmc:/switch/kartpad/data`.
4. The locally built game NRO checks the extractor's completion manifest and uses that tree as its default `dvd_root`.

The standalone extraction path is hardware-verified. A real supported dump produced 2032 files / 2,682,689,784 bytes on a physical Switch. The game runtime's data gate now accepts that completed tree; older statements that it only checked a two-file stub are obsolete.

The game does **not** currently perform the full extraction inline during its own first launch. The separate extractor is the supported current ingestion UI.

## SD layout

Typical local layout:

```text
sdmc:/switch/
  kartpad.nro                 locally built derivative game NRO
  kartpad-disc-extractor.nro  source-built extractor

sdmc:/iso/
  RMCP01.wbfs                 user's own dump (or supported raw ISO location)

sdmc:/switch/kartpad/
  Config.toml
  kartpad-runtime.log
  data/
    sys/
      fst.bin
      ...
    files/
      ...
    .kartpad-extraction-complete
```

The full game NRO contains translated/generated code/data derived from the user's dump, so it remains a local derivative and must not be published with source releases. Game assets are not embedded in ROMFS.

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
