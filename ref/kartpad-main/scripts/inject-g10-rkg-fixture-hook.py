#!/usr/bin/env python3
"""Inject the opt-in RKG fixture guard into translated Wii input handling."""

from __future__ import annotations

import argparse
from pathlib import Path


DECLARATION = 'extern "C" bool KPad_RkgFixture_CalcInner(CpuContext* ctx);'
SIGNATURE = 'extern "C" void func_8051FC84(CpuContext* MKW_RESTRICT ctx)\n{'
GUARD = "    if (KPad_RkgFixture_CalcInner(ctx)) {\n        return;\n    }"


def inject(path: Path) -> bool:
    source = path.read_text()
    if source.count(SIGNATURE) != 1:
        raise SystemExit(f"expected exactly one Wii calc signature in {path}")

    already_injected = source.count(DECLARATION) == 1 and source.count(GUARD) == 1
    if already_injected:
        return False
    if DECLARATION in source or GUARD in source:
        raise SystemExit(f"partial or duplicate RKG fixture hook in {path}")

    source = source.replace(
        SIGNATURE,
        f"{DECLARATION}\n\n{SIGNATURE}\n{GUARD}",
        1,
    )
    path.write_text(source)
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("function", type=Path)
    args = parser.parse_args()
    changed = inject(args.function)
    print(f"{'injected' if changed else 'verified'} RKG fixture hook: {args.function}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
