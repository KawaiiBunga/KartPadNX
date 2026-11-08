#!/usr/bin/env python3
"""Checks dropin::LoadKamek byte for byte against what translate-mod baked for the same Code.pul:
the relocated module image (overlay_images/kamek_module_81800000.bin), the code SHA-1, and every
patch in mod_data_patches.cpp (hook branches remapped from the module link base to 0x81800000).

  python test_kamek_loader.py <kamek_loader_dump> <Code.pul> <translate-mod output dir>
"""
import re
import subprocess
import sys
import tempfile
from pathlib import Path

LINK_BASE, GUEST_BASE = 0x803992E0, 0x81800000


def expected_writes(cpp, code_size):
    """(address, size, value) for every Kamek patch translate-mod emitted."""
    out = []
    text = cpp.read_text()
    for m in re.finditer(r'EncodeStaticPpcBranch\(0x([0-9A-F]+)u, 0x([0-9A-F]+)u, (true|false)', text):
        src, dst, link = int(m[1], 16), int(m[2], 16), m[3] == 'true'
        if LINK_BASE <= dst < LINK_BASE + code_size:  # translate-mod resolves hooks at the link base, then remaps
            dst = dst - LINK_BASE + GUEST_BASE
        out.append((src, 4, 0x48000000 | ((dst - src) & 0x03FFFFFC) | link))
    for line in text.splitlines():
        if '// Retro WFC' in line:  # Retro-WFC payload pointers, not Kamek commands
            continue
        m = re.search(r'(?:PatchGuestU|Memory::Write)(8|16|32)\(0x([0-9A-F]+)u, 0x([0-9A-F]+)u\)', line)
        if m:
            out.append((int(m[2], 16), int(m[1]) // 8, int(m[3], 16)))
    return out


def main():
    tool, pul, mod = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    failed = []

    def check(cond, what):
        print(('PASS ' if cond else 'FAIL ') + what)
        if not cond:
            failed.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        image_path = Path(tmp) / 'module.bin'
        run = subprocess.run([str(tool), str(pul), str(image_path)], capture_output=True, text=True)
        check(run.returncode == 0, f'loader ran ({run.stderr.strip()})')
        if run.returncode:
            return 1
        lines = run.stdout.splitlines()
        image = image_path.read_bytes()
    _, start, code, bss, ctor_start, ctor_end = lines[0].split()
    code, bss = int(code, 16), int(bss, 16)
    sha = lines[1][1:]
    got = [(int(a, 16), int(s), int(v, 16)) for _, a, s, v in (l.split() for l in lines if l[0] == 'W')]
    skipped = [int(l.split()[1], 16) for l in lines if l[0] == 'K']

    baked = (mod / 'overlay_images/kamek_module_81800000.bin').read_bytes()
    check(start == f'{GUEST_BASE:08X}', 'module placed at 0x81800000')
    check(image[:code] == baked[:code], f'relocated code+data matches translate-mod byte for byte (0x{code:X} bytes)')
    check(image[code:] == bytes(bss) and baked[code:code + bss] == bytes(bss), f'bss zeroed (0x{bss:X} bytes)')
    check(sha == (mod / 'cpp/mod_data_patches_blobs/kamek_code_sha1.bin').read_bytes().hex(), 'code SHA-1 matches')
    ctors = re.search(r'kCtorStart = 0x([0-9A-F]+)u;\s*constexpr uint32_t kCtorEnd = 0x([0-9A-F]+)u',
                      (mod / 'cpp/mod_data_patches.cpp').read_text())
    check((int(ctor_start, 16), int(ctor_end, 16)) == (int(ctors[1], 16), int(ctors[2], 16)), 'constructor table range matches')

    want = expected_writes(mod / 'cpp/mod_data_patches.cpp', code)
    final_got = {}
    for a, s, v in got:
        final_got[(a, s)] = v
    final_want = {}
    for a, s, v in want:
        final_want[(a, s)] = v
    missing = [k for k in final_want if final_got.get(k) != final_want[k]]
    extra = [k for k in final_got if k not in final_want]
    check(not missing, f'all {len(final_want)} translate-mod patches applied with the same value'
          + (f'; first mismatches {[(hex(a), s, hex(final_want[(a, s)]), hex(final_got.get((a, s), 0))) for a, s in missing[:5]]}' if missing else ''))
    check(not extra, f'no patches translate-mod did not make' + (f'; first {[(hex(a), s) for a, s in extra[:5]]}' if extra else ''))
    check(sorted(skipped) == [0x80001920, 0x800ED6E8] or sorted(skipped) == [0x800ED6E8],
          f'only the codehandler probe and WFC bootstrap hook skipped ({[hex(s) for s in skipped]})')
    print('FAILED' if failed else 'Kamek loader matches translate-mod.')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
