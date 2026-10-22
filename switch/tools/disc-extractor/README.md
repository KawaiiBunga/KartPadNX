# KartPad Disc Extractor

This standalone Switch homebrew reads the user's own Mario Kart Wii **PAL
RMCP01 revision 0** WBFS or ISO and extracts the encrypted DATA partition to
`sdmc:/switch/kartpad/data`. It is separate from the game NRO and contains no game files or per-disc title key. It **does** embed the public Wii common-key constants used to unwrap the disc title key, matching the reference ports.

Build it from a Linux/devkitPro environment (the existing `kartpad-dawn`
container works) without rebuilding any graphics dependencies:

```sh
make -C switch/tools/disc-extractor
```

The packaged files are placed in `build/switch-extractor/`. Copy
`kartpad-disc-extractor.nro` to `sd:/switch/` and launch it from the Homebrew
Menu. Put a single-file WBFS at `sd:/iso/RMCP01.wbfs` (the default), or a raw
ISO at `sd:/iso/RMCP01.iso`; press **X** to enter another SD path. Split WBFS
files such as `.wbf1` are unsupported. A full raw Wii ISO is larger than FAT32
permits, so it needs a suitable filesystem; the extractor does not concatenate
split ISO files.

**No key input is required.** The Wii common key is built in and selected per
disc by the ticket's `common_key_index` (the same public constant Dolphin and the
PC/mobile KartPad ports embed), so extraction starts immediately after you choose
the source. Advanced users may override it by placing an exact 16-byte binary key
at `sdmc:/switch/kartpad/common-key.bin`; an override is held only in RAM, cleared
before exit, and never written to the screen or log. Press **+** while extracting
to cancel at an extraction checkpoint.

The console performs a public AES-128-CBC known-answer test on the Switch before
extracting, then shows stages and totals. Its durable log is
`sdmc:/kartpad-disc-extractor.log`; it is flushed, fsynced, and committed after
every front-end event. A failed or cancelled run can leave partial output; run
the extractor again after correcting the problem.

## Build evidence

The final package was built with the existing `kartpad-dawn` image using the
command above. `kartpad-disc-extractor.nro` is 829,637 bytes with SHA-256
`2E133CAA30079FFD7CB419BDC77F8A7886917B323ADC591D1088C85F46B9DAD7`; its ELF is
an ELF64 AArch64 position-independent executable.

Hardware extraction is **verified**: a real PAL RMCP01 rev0 WBFS on a physical
Switch extracted 2032 files (2,682,689,784 bytes, fst=64224, dol=2766496) to
`sdmc:/switch/kartpad/data` with no key input.
