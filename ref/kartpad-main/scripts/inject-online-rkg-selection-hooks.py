#!/usr/bin/env python3
"""Inject opt-in RKG metadata hooks into translated online menu functions."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Hook:
    declaration: str
    declaration_anchor: str
    statement: str
    statement_anchor: str
    statement_replacement: str | None = None
    declaration_replacement: str | None = None


HOOKS = {
    "func_8083DFA8.cpp": Hook(
        'extern "C" uint32_t KPad_RkgFixture_ForceCharacterId(uint32_t requested);',
        'extern "C" uint64_t func_80860484_statefree_v1(uint32_t, uint32_t);',
        "    r29 = KPad_RkgFixture_ForceCharacterId(r29);",
        "    r29 = r5;",
    ),
    "func_80846C1C.cpp": Hook(
        'extern "C" uint32_t KPad_RkgFixture_ForceVehicleId(uint32_t requested);',
        'extern "C" MkwStateFreeResult2 func_80860484_statefree_v0(uint32_t, uint32_t);',
        "    r31 = KPad_RkgFixture_ForceVehicleId(r31);",
        "    r31 = MemoryInline::FlatRead32((r4 + 576));",
    ),
    "func_8084E388.cpp": Hook(
        'extern "C" uint32_t KPad_RkgFixture_ForceDriftButtonId(uint32_t requested);',
        'extern "C" MkwStateFreeResult2 func_80631588_statefree(uint32_t, uint32_t, uint32_t);',
        "    r5 = KPad_RkgFixture_ForceDriftButtonId(r5);",
        "    r5 = MemoryInline::FlatRead32((r31 + 576));\n"
        "    r3 = MemoryInline::FlatRead32((r4 + 7736));",
        "    r5 = MemoryInline::FlatRead32((r31 + 576));\n"
        "    r5 = KPad_RkgFixture_ForceDriftButtonId(r5);\n"
        "    r3 = MemoryInline::FlatRead32((r4 + 7736));",
    ),
    "func_80643F48.cpp": Hook(
        'extern "C" uint32_t KPad_RkgFixture_ForceCourseId(uint32_t requested);',
        '#include "recomp_mod_loader.h"\n\n',
        "    r4 = KPad_RkgFixture_ForceCourseId(r4);",
        "[[maybe_unused]] loc_80643F48:\n{",
        None,
        '#include "recomp_mod_loader.h"\n\n'
        'extern "C" uint32_t KPad_RkgFixture_ForceCourseId(uint32_t requested);\n\n',
    ),
}


def _insert_after(source: str, anchor: str, insertion: str, path: Path) -> str:
    if source.count(anchor) != 1:
        raise SystemExit(f"expected exactly one anchor in {path}: {anchor!r}")
    return source.replace(anchor, f"{anchor}\n{insertion}", 1)


def inject(path: Path) -> bool:
    try:
        hook = HOOKS[path.name]
    except KeyError as error:
        raise SystemExit(f"unsupported RKG selection function: {path.name}") from error

    source = path.read_text()
    declaration_count = source.count(hook.declaration)
    statement_count = source.count(hook.statement)
    if declaration_count == 1 and statement_count == 1:
        return False
    if declaration_count != 0 or statement_count != 0:
        raise SystemExit(f"partial or duplicate RKG selection hook in {path}")

    if hook.declaration_replacement is None:
        source = _insert_after(source, hook.declaration_anchor, hook.declaration, path)
    else:
        if source.count(hook.declaration_anchor) != 1:
            raise SystemExit(
                f"expected exactly one anchor in {path}: {hook.declaration_anchor!r}"
            )
        source = source.replace(
            hook.declaration_anchor, hook.declaration_replacement, 1
        )
    if hook.statement_replacement is None:
        source = _insert_after(source, hook.statement_anchor, hook.statement, path)
    else:
        if source.count(hook.statement_anchor) != 1:
            raise SystemExit(
                f"expected exactly one anchor in {path}: {hook.statement_anchor!r}"
            )
        source = source.replace(
            hook.statement_anchor, hook.statement_replacement, 1
        )
    path.write_text(source)
    return True


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("function", type=Path)
    args = parser.parse_args()
    changed = inject(args.function)
    print(f"{'injected' if changed else 'verified'} RKG selection hook: {args.function}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
