#!/usr/bin/env python3
"""Guest accesses as base register + displacement (a transform pass over the translated shards).

    python builder/flat-guest-displacement.py BUILD_SHARDS_DIR [--check]

The translator emits a flat guest access as MemoryInline::FlatRead32((r3 + 204)): a wrapping 32-bit
add that is then zero-extended, so the compiler's alias analysis cannot tell r3 + 204 from r3 + 8
and must assume every guest store may change every guest load. This rewrites the exact forms

    MemoryInline::Flat<Op>((rN + C))        ->  MemoryInline::Flat<Op>At(rN, C)
    MemoryInline::Flat<Op>((rN + C), value) ->  MemoryInline::Flat<Op>At(rN, C, value)
    MemoryInline::Flat<Op>(rN) / (rN, ...)  ->  MemoryInline::Flat<Op>At(rN, 0) / (rN, 0, ...)

for |C| < 65536, whose host address is base + zext(rN) + C (runtime memory_access.h). That equals
the 32-bit form unless rN + C wraps past 4 GiB, which no valid guest pointer does, and the flat
reservation keeps 64 KiB guards on both sides so a wrapped garbage address still faults. Everything
else (indexed rN + rM, constant addresses, the checked and resolved paths) is left as emitted.
Lines are rewritten in place, never added or removed, so #line locations and sample-PGO line offsets
stay valid. Idempotent. --check counts without writing.
"""
import argparse
import re
import sys
from pathlib import Path

OP = r'(Read(?:8|16|32|Float32|Float64)|Write(?:Ram)?(?:8|16|32|Float32|Float64))'
NAME = r'(r\d+(?:_[A-Za-z0-9]+)*)'
DISP = r'(-?\d{1,5})'
FORMS = [
    (re.compile(r'MemoryInline::Flat' + OP + r'\(\(' + NAME + r' \+ ' + DISP + r'\)\)'), True, ')'),
    (re.compile(r'MemoryInline::Flat' + OP + r'\(\(' + NAME + r' \+ ' + DISP + r'\), '), True, ', '),
    (re.compile(r'MemoryInline::Flat' + OP + r'\(' + NAME + r'\)'), False, ')'),
    (re.compile(r'MemoryInline::Flat' + OP + r'\(' + NAME + r', '), False, ', '),
]
LIMIT = 65536


def rewrite(text):
    count = 0
    for pattern, displaced, tail in FORMS:
        def repl(m):
            nonlocal count
            d = int(m.group(3)) if displaced else 0
            if abs(d) >= LIMIT:
                return m.group(0)
            count += 1
            return f'MemoryInline::Flat{m.group(1)}At({m.group(2)}, {d}{tail}'
        text = pattern.sub(repl, text)
    return text, count


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('shards', type=Path)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()
    files = changed = total = 0
    for path in sorted(args.shards.rglob('*.cpp')):
        raw = path.read_bytes()
        text = raw.decode('utf-8')
        new, n = rewrite(text)
        files += 1
        if n:
            if new.count('\n') != text.count('\n'):
                sys.exit(f'{path}: the rewrite changed the line count')
            changed += 1
            total += n
            if not args.check:
                path.write_bytes(new.encode('utf-8'))
    print(f'flat-guest-displacement: {total} accesses in {changed} of {files} files'
          + (' (check only)' if args.check else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
