#!/usr/bin/env python3
"""Remove reclaimed race-camera nodes in the translated shared list walker."""

from __future__ import annotations

import argparse
from pathlib import Path


SIGNATURE = 'extern "C" void func_805A1A8C(CpuContext* MKW_RESTRICT ctx)\n{'
ENTRY = "[[maybe_unused]] loc_805A1AAC:\n{\n    r12 = MemoryInline::FlatRead32(r30);"
MARKER = "// A race restart can leave a reclaimed camera node linked after scene teardown"
GUARD = """[[maybe_unused]] loc_805A1AAC:
{
    // A race restart can leave a reclaimed camera node linked after scene teardown.
    // Three-player retail races also contain legitimate non-player cameras with
    // player slot 0xff, so the slot alone is not a reclamation signal. Require
    // the scene-heap poison in the camera object before unlinking the stale node.
    const uint32_t camera_node = r30;
    const uint32_t camera = camera_node - 136u;
    if (MemoryInline::FlatRead8(camera + 156u) == 0xFFu &&
        MemoryInline::FlatRead32(camera) == 0x55440003u) {
        constexpr uint32_t list = 0x809C19A8u;
        const uint32_t offset = MemoryInline::FlatRead16(list + 10u);
        const uint32_t links = camera_node + offset;
        const uint32_t previous = MemoryInline::FlatRead32(links);
        const uint32_t next = MemoryInline::FlatRead32(links + 4u);

        if (previous == 0) {
            MemoryInline::FlatWrite32(list, next);
        } else {
            MemoryInline::FlatWrite32(previous + offset + 4u, next);
        }
        if (next == 0) {
            MemoryInline::FlatWrite32(list + 4u, previous);
        } else {
            MemoryInline::FlatWrite32(next + offset, previous);
        }
        MemoryInline::FlatWrite32(links, 0);
        MemoryInline::FlatWrite32(links + 4u, 0);
        const uint16_t count = MemoryInline::FlatRead16(list + 8u);
        if (count != 0) {
            MemoryInline::FlatWrite16(list + 8u, static_cast<uint16_t>(count - 1u));
        }
        r30 = 0;
        goto loc_805A1AC0;
    }
    r12 = MemoryInline::FlatRead32(r30);"""


def inject(path: Path) -> bool:
    source = path.read_text()
    if source.count(SIGNATURE) != 1:
        raise SystemExit(f"expected exactly one camera-list signature in {path}")

    marker_count = source.count(MARKER)
    if marker_count == 1:
        return False
    if marker_count != 0 or source.count(ENTRY) != 1:
        raise SystemExit(f"partial, duplicate, or unexpected camera-list walker in {path}")

    path.write_text(source.replace(ENTRY, GUARD, 1))
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("function", type=Path)
    args = parser.parse_args()
    changed = inject(args.function)
    print(f"{'injected' if changed else 'verified'} camera lifecycle guard: {args.function}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
