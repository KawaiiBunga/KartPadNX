# Switch runtime platform sources

Switch-specific sources for the **real game runtime** (`build/wiicompiled-fpscr/
runtime`), kept here rather than inside that tree so the runtime ships no
platform glue. The full-app build in `switch/cmake/FullApp.cmake` references the runtime-facing sources that are required by the current base-game NRO.

## Files

- `host_cpu_baseline_stub.cpp` — replaces the Windows x86 CPUID guard. The Switch
  CPU is fixed (Tegra X1, Cortex-A57 aarch64) and the whole runtime is built for
  it, so there is nothing to probe. Mirrors the Apple port's stub.
- `wup028_adapter_stub.cpp` — reports "no official GameCube adapter present" for
  every call. libnx USB host support (usbHs) is out of scope for bring-up;
  Aurora's normal HID path is unaffected. Replace with a usbHs implementation if
  native GC-adapter support is wanted later.
- `disc_reader.{h,cpp}` — WBFS / raw-ISO container reader for the user-supplied
  dump (`sd:/iso/RMCP01.wbfs` or `.iso`). No Nintendo data ships with the port.
  Opens the container, undoes WBFS block scattering to expose a **linear**
  disc-offset read, validates the unencrypted disc header (RMCP01, PAL, rev0),
  parses the partition table to locate the DATA partition, and reads the
  partition header (ticket → encrypted title key + IV, and the absolute offsets
  of TMD / cert / H3 / encrypted data).
- `wii_fst.{h,cpp}` — parses a (decrypted) Wii FST blob into the file tree
  (paths, byte offsets, sizes). It validates tree parent/end metadata and
  FAT-safe, collision-free output components before the extraction stage writes
  `dvd_root`'s `sys/fst.bin` + `files/`. Operates on in-memory bytes only.
- `wii_cluster.{h,cpp}` — geometry of the encrypted partition (0x7C00 payload
  inside each 0x8000 cluster). Maps logical (FST) offsets to the physical
  encrypted cluster ranges the decrypt stage reads. Pure arithmetic.
- `guest_flat_memory_switch.cpp` — the libnx backing for the flat 4 GiB guest
  address space (piece 2). Reserves the flat VA with `virtmemFindAslr`, and backs
  each MEM1/MEM2/owned section with one `svcCreateSharedMemory` object mapped at
  its cached/uncached/physical guest mirrors plus a host alias — the aliasing
  model an on-console probe (since removed) confirmed works. Paired with
  the `__SWITCH__` branch of `include/guest_flat_memory.h` (in the runtime patch)
  that publishes the base as `gFlatGuestBase`. Bring-up scope: no page-protection
  interception yet (deferred reads / exec guard fall back to the checked path).
  Compiles for the Switch ABI against libnx; the full interface is exported. The aliasing/reservation primitives are hardware-validated by that probe, and the full 4 MiB-slice mapping is hardware-confirmed in the real game — guest RAM allocates and the game is playable.

- `wii_crypto.{h,cpp}` + `wii_crypto_switch.cpp` — Wii partition decryption. The
  logic (title-key unwrap with the common key + title-id IV; per-cluster payload
  decrypt with the IV at cluster offset 0x3D0) takes the AES primitive as a
  parameter so it is testable off-device with a mock; `wii_crypto_switch.cpp`
  supplies the real primitive via libnx `aes128CbcDecrypt`.
- `wii_common_key.{h,cpp}` — the embedded Wii **common key** (retail + Korean,
  selected by the ticket's `common_key_index`), the same public constant Dolphin
  and the PC/mobile KartPad ports carry, so on-console extraction needs no
  user-supplied key. Kept separate so `wii_crypto` stays pure logic and tests that
  inject their own key need not link it. An optional `common-key.bin` overrides it.

  `disc_extractor.{h,cpp}` stitches these components into `dvd_root`
  materialization. It streams and decrypts the DATA partition, writes files via
  staging plus sync/rename and best-effort SD commit, permits checkpoint
  cancellation, and writes the completion manifest only after all files are
  published. A power loss during replacement can leave a file absent; device-level
  metadata durability is not yet verified. Cancelled and failed runs do not
  publish the completion manifest. Its standalone libnx
  front end is [`switch/tools/disc-extractor/`](../../tools/disc-extractor/README.md),
  targeting `sdmc:/switch/kartpad/data`. Hardware extraction is verified, and `runtime_host_switch.cpp` now accepts the final manifest plus `files/` and `sys/fst.bin`; `ResolvedDvdRoot()` defaults to this tree on Switch.

## Verification

`host_cpu_baseline_stub.cpp` and `wup028_adapter_stub.cpp` compile under the
runtime's aarch64 Clang (C++20, `-Wall -Wextra`) against the real runtime +
aurora headers.

Each ingestion module has an off-device functional test under `switch/tests/`
that builds synthetic inputs (no Nintendo data — just the six-character game id
and public format scaffolding):

- `disc_reader_test.cpp` — WBFS block-scatter reassembly, sparse holes, ISO EOF
  zero-fill, RMCP01/rev0 validation (accept + reject), DATA partition location,
  and partition-header key/IV/section-offset extraction.
- `wii_fst_test.cpp` — FST walk: paths, directory nesting, byte offsets (Wii
  stores file offsets >>2), sizes, malformed metadata, unsafe components, and
  exact/ASCII-case-folded FAT path collisions.
- `wii_cluster_test.cpp` — logical↔cluster geometry, boundary straddles,
  multi-cluster ranges, and header-skip offsets.
- `wii_crypto_test.cpp` — title-key unwrap and cluster-data decrypt plumbing via
  a mock involutive cipher: verifies the right key/IV/range and that the IV is
  read from 0x3D0 (a wrong IV breaks the round-trip).
- `disc_extractor_test.cpp` — a synthetic encrypted partition, including a file
  that straddles a cluster boundary, plus staged output, cancellation, malformed
  tree, and completion-manifest behavior.

Run any of them in the `kartpad-dawn` container (example for the disc reader):

```
# host build + run (fast logic check)
clang++ -std=c++17 -O2 -Wall -Wextra -I switch/src/runtime \
  switch/tests/disc_reader_test.cpp switch/src/runtime/disc_reader.cpp -o /tmp/disc_test
/tmp/disc_test            # -> RESULT: PASS (0 failure(s))

# real aarch64 execution under qemu
aarch64-linux-gnu-g++ -std=c++17 -O2 -static -I switch/src/runtime \
  switch/tests/disc_reader_test.cpp switch/src/runtime/disc_reader.cpp -o /tmp/disc_aarch64
qemu-aarch64 /tmp/disc_aarch64

# Switch ABI compile-check (devkitA64 sysroot, C++20 like the runtime)
clang++ --target=aarch64-none-elf --sysroot=$DEVKITPRO/devkitA64/aarch64-none-elf \
  -stdlib=libstdc++ -march=armv8-a+crc+crypto -D__SWITCH__ -std=c++20 \
  -I switch/src/runtime -c switch/src/runtime/disc_reader.cpp -o /tmp/disc.o
```

The ingestion tests pass on host and under qemu-aarch64, and their runtime
sources build to clean Switch-ABI objects. The standalone extractor NRO has a
verified AArch64 build and a **verified hardware extraction** (real PAL RMCP01
dump: 2032 files / 2,682,689,784 bytes → `sdmc:/switch/kartpad/data`).
