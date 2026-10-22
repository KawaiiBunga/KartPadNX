#!/usr/bin/env python3
"""Windows stand-ins for the devkitPro host tools the Switch build calls.

devkitPro publishes its toolchain as Linux/msys2 packages; the AIO builder only takes the
target-side files (libnx, newlib, libstdc++, crt objects, headers) from devkitPro's official
Docker Hub image and replaces every host program with one of these:

  link      aarch64-none-elf-g++ as a link driver: applies switch.specs and links with LLVM lld
  elf2nro   port of switchbrew/switch-tools elf2nro (+ romfs builder), same byte layout
  nacptool  port of switch-tools nacptool --create
  bin2s     port of devkitPro general-tools bin2s
  uam       copies the toolkit's precompiled .dksh for a shader (no GLSL compiler on Windows)
  pkg-config  minimal stub: Switch.cmake requires the program to exist

Each tool is reached through nxshim.exe copies named after the real program; the shim runs
`python nxtools.py <tool> <args...>`. Environment: KPNX_DEVKITPRO (the extracted devkitPro
tree), KPNX_LLVM (LLVM root), KPNX_DKSH (precompiled shader directory).
"""
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

GCC_VERSION = '15.2.0'


def env_path(name):
    value = os.environ.get(name)
    if not value:
        sys.exit(f'nxtools: {name} is not set')
    return Path(value)


# --------------------------------------------------------------------- response files ---

def split_windows_args(text):
    """Tokenize a response file the way CMake writes it for Ninja on Windows: whitespace
    separated, double quotes group, a backslash escapes only a quote or a backslash run that
    precedes a quote (CommandLineToArgvW rules)."""
    args, cur, inq, have, i, n = [], [], False, False, 0, len(text)
    while i < n:
        c = text[i]
        if c == '\\':
            j = i
            while j < n and text[j] == '\\':
                j += 1
            count = j - i
            if j < n and text[j] == '"':
                cur.append('\\' * (count // 2))
                if count % 2:
                    cur.append('"')
                    i = j + 1
                else:
                    i = j
                have = True
                continue
            cur.append('\\' * count)
            have = True
            i = j
            continue
        if c == '"':
            inq = not inq
            have = True
            i += 1
            continue
        if c in ' \t\r\n' and not inq:
            if have:
                args.append(''.join(cur))
                cur, have = [], False
            i += 1
            continue
        cur.append(c)
        have = True
        i += 1
    if have:
        args.append(''.join(cur))
    return args


def expand_rsp(argv, depth=0):
    out = []
    for a in argv:
        if a.startswith('@') and depth < 8 and Path(a[1:]).is_file():
            out.extend(expand_rsp(split_windows_args(Path(a[1:]).read_text(encoding='utf-8')), depth + 1))
        else:
            out.append(a)
    return out


def quote_windows(arg):
    if arg and not any(c in arg for c in ' \t"'):
        return arg
    res, bs = ['"'], 0
    for c in arg:
        if c == '\\':
            bs += 1
        elif c == '"':
            res.append('\\' * (bs * 2 + 1) + '"')
            bs = 0
        else:
            res.append('\\' * bs + c)
            bs = 0
    res.append('\\' * (bs * 2) + '"')
    return ''.join(res)


# ------------------------------------------------------------------------- link driver ---

def section_order_to_symbols(order_file, out_file):
    """GNU ld --section-ordering-file (a linker-script fragment listing *(.text.<sym>)) to an
    lld --symbol-ordering-file. Translated code is built with -ffunction-sections, so each
    section name carries exactly one symbol."""
    syms = []
    for m in re.finditer(r'\*\(\.text\.([^\s)]+)\)', Path(order_file).read_text(encoding='utf-8')):
        syms.append(m.group(1))
    Path(out_file).write_text('\n'.join(syms) + '\n', encoding='utf-8')
    return len(syms)


def module_name_object(name, workdir, clang):
    """The .nx-module-name section GNU ld writes for --nx-module-name: u32 0, u32 length,
    the name and a NUL. Atmosphere crash reports print it as the module name."""
    data = name.encode() + b'\0'
    src = Path(workdir) / 'nx_module_name.S'
    obj = Path(workdir) / 'nx_module_name.o'
    src.write_text('\t.section .nx-module-name, "a"\n\t.word 0\n\t.word %d\n\t.byte %s\n'
                   % (len(name), ','.join(str(b) for b in data)), encoding='ascii')
    subprocess.run([str(clang), '--target=aarch64-none-elf', '-c', str(src), '-o', str(obj)], check=True)
    return obj


def lld_linker_script(switch_ld, out_file):
    """libnx's switch.ld, adapted for lld:
    - a PT_TLS header over .tdata/.tbss. GNU ld derives local-exec TLS offsets from the first
      TLS section without one; lld needs the segment, and gives the same offsets from it.
      elf2nro reads only the three PT_LOAD headers, so the extra header is not in the NRO.
    - .relr.dyn takes only .relr.dyn: lld also has an internal .relr.auth.dyn (pointer
      authentication, unused here) that the original *(.relr.*) glob would pull in.
    - .main.tls (the main thread's TLS reservation, only `. +=`) is typed NOBITS, as GNU ld
      makes it; lld would make it PROGBITS and write all of .bss into the file."""
    text = Path(switch_ld).read_text(encoding='utf-8')
    edits = [
        (r'(dyn\s+PT_DYNAMIC;)', r'\1\n\ttls    PT_TLS;'),
        (r'(\.relr\.dyn\s*:\s*\{\s*)\*\(\.relr\.\*\)', r'\1*(.relr.dyn)'),
        (r'(\.tdata\s*:\s*\{[^}]*\}\s*):data', r'\1:data :tls'),
        (r'(\.tbss\s*:\s*\{[^}]*\}\s*):data', r'\1:data :tls'),
        (r'(\.main\.tls\s+ALIGN\(MAX\(ALIGNOF\(\.tdata\),\s*ALIGNOF\(\.tbss\)\)\))\s*:', r'\1 (TYPE=SHT_NOBITS) :'),
    ]
    for pattern, repl in edits:
        text, n = re.subn(pattern, repl, text, count=1)
        if n != 1:
            sys.exit(f'nxtools link: libnx switch.ld changed; cannot apply {pattern!r}')
    Path(out_file).write_text(text, encoding='utf-8')
    return out_file


def tool_link(argv):
    dkp = env_path('KPNX_DEVKITPRO')
    llvm = env_path('KPNX_LLVM')
    lld = llvm / 'bin' / 'ld.lld.exe'
    clang = llvm / 'bin' / 'clang.exe'
    gcclib = dkp / 'devkitA64' / 'lib' / 'gcc' / 'aarch64-none-elf' / GCC_VERSION / 'pic'
    clib = dkp / 'devkitA64' / 'aarch64-none-elf' / 'lib' / 'pic'

    args = expand_rsp(argv)
    output = None
    user = []           # objects, archives and positional linker options, in order
    lib_dirs = []
    specs = None
    order_file = None
    i = 0
    while i < len(args):
        a = args[i]
        if a == '-o':
            output = args[i + 1]
            i += 2
            continue
        if a.startswith('-specs=') or a.startswith('--specs='):
            specs = a.split('=', 1)[1]
        elif a.startswith('-L'):
            lib_dirs.append(a[2:] if len(a) > 2 else args[i + 1])
            if len(a) == 2:
                i += 1
        elif a.startswith('-l'):
            user.append(a)
        elif a == '-Xlinker':
            user.append(args[i + 1])
            i += 1
        elif a.startswith('-Wl,'):
            user.extend(p for p in a[4:].split(',') if p)
        elif a in ('-pie', '-fPIE', '-fpie', '-fPIC', '-fpic', '-static-pie', '-pipe', '-g', '-s',
                   '-rdynamic', '-pthread', '-static') or a.startswith(('-O', '-m', '-f', '-std',
                   '--target', '--sysroot', '-stdlib', '--gcc-toolchain', '-W', '-D', '-I', '-g')):
            pass  # compile-only or driver-only flags
        elif a.startswith('-'):
            user.append(a)
        else:
            user.append(a)
        i += 1
    if not output:
        sys.exit('nxtools link: no -o output')

    # --section-ordering-file is a GNU ld 2.43 option; lld orders by symbol.
    fixed = []
    k = 0
    tmp = Path(tempfile.mkdtemp(prefix='kpnx-link-'))
    while k < len(user):
        u = user[k]
        if u.startswith('--section-ordering-file'):
            path = u.split('=', 1)[1] if '=' in u else user[k + 1]
            if '=' not in u:
                k += 1
            sym_file = tmp / 'order.sym'
            count = section_order_to_symbols(path, sym_file)
            print(f'nxtools link: {count} hot functions ordered from {Path(path).name}', flush=True)
            fixed += ['--symbol-ordering-file=' + str(sym_file), '--no-warn-symbol-ordering']
        elif u in ('-Map',):
            fixed += ['-Map', user[k + 1]]
            k += 1
        else:
            fixed.append(u)
        k += 1

    cmd = ['-m', 'aarch64elf', '-Bstatic']
    if specs:
        # libnx switch.specs: -T switch.ld -pie --no-dynamic-linker --spare-dynamic-tags=0
        # --gc-sections -z text -z now -z nodynamic-undefined-weak -z pack-relative-relocs
        # --build-id=sha1 --nx-module-name; startfile crti crtbegin --require-defined=main.
        # GNU-only pieces: --nx-module-name is linked in as an object; spare DT_NULL tags are
        # not needed by the NRO loader; lld already resolves undefined weak symbols to 0 in a
        # PIE without shared libraries (-z nodynamic-undefined-weak); --require-defined=main
        # becomes --undefined=main (crt0 references main, so a missing one still fails).
        script = lld_linker_script(dkp / 'libnx' / 'switch.ld', tmp / 'switch-lld.ld')
        cmd += ['-T', str(script), '-pie', '--no-dynamic-linker',
                '--gc-sections', '-z', 'text', '-z', 'now',
                '-z', 'pack-relative-relocs', '--build-id=sha1', '--undefined=main']
        cmd += [str(gcclib / 'crti.o'), str(gcclib / 'crtbegin.o')]
        cmd.append(str(module_name_object(Path(output).stem, tmp, clang)))
    for d in lib_dirs:
        cmd.append('-L' + d)
    cmd += ['-L' + str(gcclib), '-L' + str(clib)]
    cmd += fixed
    # g++ default libraries and devkitA64's libgcc/lib spec.
    cmd += ['-lstdc++', '-lm', '--start-group', '-lgcc', '-lc', '-lsysbase', '--end-group']
    if specs:
        cmd += [str(gcclib / 'crtend.o'), str(gcclib / 'crtn.o')]
    cmd += ['-o', output]

    rsp = tmp / 'link.rsp'
    rsp.write_text('\n'.join(quote_windows(c) for c in cmd), encoding='utf-8')
    try:
        r = subprocess.run([str(lld), '--rsp-quoting=windows', '@' + str(rsp)])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return r.returncode


# ----------------------------------------------------------------------------- romfs ---

ROMFS_ENTRY_EMPTY = 0xFFFFFFFF
ROMFS_FILEPARTITION_OFS = 0x200


def _align(v, a):
    return (v + (a - 1)) & ~(a - 1)


def _hash_count(n):
    if n < 3:
        return 3
    if n < 19:
        return n | 1
    c = n
    while any(c % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        c += 1
    return c


def _path_hash(parent, name):
    h = (parent ^ 123456789) & 0xFFFFFFFF
    for b in name:
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= b
    return h


class _Dir:
    __slots__ = ('sum_path', 'name', 'entry_offset', 'parent', 'child', 'sibling', 'file', 'next', 'os_path')

    def __init__(self):
        self.sum_path = b''
        self.name = b''
        self.entry_offset = 0
        self.parent = self.child = self.sibling = self.file = self.next = None
        self.os_path = ''


class _File:
    __slots__ = ('sum_path', 'name', 'entry_offset', 'offset', 'size', 'parent', 'sibling', 'next', 'os_path')

    def __init__(self):
        self.sum_path = b''
        self.name = b''
        self.entry_offset = self.offset = self.size = 0
        self.parent = self.sibling = self.next = None
        self.os_path = ''


def build_romfs(root_dir, out, base_offset):
    """Port of switch-tools romfs.c build_romfs_into_file, including its ordering rules."""
    ctx = {'files': None, 'num_dirs': 1, 'num_files': 0, 'dir_table_size': 0x18, 'file_table_size': 0}
    root = _Dir()
    root.parent = root
    root.os_path = str(root_dir)
    root.sum_path = str(root_dir).replace('\\', '/').encode('utf-8')

    def visit(parent):
        child_dirs = None
        child_files = None
        for entry in os.scandir(parent.os_path):
            name = entry.name.encode('utf-8')
            sum_path = parent.sum_path + b'/' + name
            if entry.is_dir():
                d = _Dir()
                ctx['num_dirs'] += 1
                d.parent, d.sum_path, d.name, d.os_path = parent, sum_path, name, entry.path
                ctx['dir_table_size'] += 0x18 + _align(len(name), 4)
                if child_dirs is None or d.sum_path < child_dirs.sum_path:
                    d.sibling, child_dirs = child_dirs, d
                else:
                    prev, c = child_dirs, child_dirs.sibling
                    while c is not None and not (d.sum_path < c.sum_path):
                        prev, c = c, c.sibling
                    prev.sibling, d.sibling = d, c
                tmp, tmp_prev = parent.next, parent
                while tmp is not None and not (d.sum_path < tmp.sum_path):
                    tmp_prev, tmp = tmp, tmp.next
                tmp_prev.next, d.next = d, tmp
            elif entry.is_file():
                f = _File()
                ctx['num_files'] += 1
                f.parent, f.sum_path, f.name, f.os_path = parent, sum_path, name, entry.path
                f.size = entry.stat().st_size
                ctx['file_table_size'] += 0x20 + _align(len(name), 4)
                if child_files is None or f.sum_path < child_files.sum_path:
                    f.sibling, child_files = child_files, f
                else:
                    prev, c = child_files, child_files.sibling
                    while c is not None and not (f.sum_path < c.sum_path):
                        prev, c = c, c.sibling
                    prev.sibling, f.sibling = f, c
                head = ctx['files']
                if head is None or f.sum_path < head.sum_path:
                    f.next, ctx['files'] = head, f
                else:
                    prev, c = head, head.next
                    while c is not None and not (f.sum_path < c.sum_path):
                        prev, c = c, c.next
                    prev.next, f.next = f, c
            else:
                sys.exit(f'romfs: invalid object {entry.path}')
        parent.child, parent.file = child_dirs, child_files
        d = child_dirs
        while d is not None:
            visit(d)
            d = d.sibling

    visit(root)
    dir_count = _hash_count(ctx['num_dirs'])
    file_count = _hash_count(ctx['num_files'])
    dir_hash = [ROMFS_ENTRY_EMPTY] * dir_count
    file_hash = [ROMFS_ENTRY_EMPTY] * file_count
    dir_table = bytearray(ctx['dir_table_size'])
    file_table = bytearray(ctx['file_table_size'])

    partition = 0
    f, eo = ctx['files'], 0
    while f is not None:
        partition = _align(partition, 0x10)
        f.offset = partition
        partition += f.size
        f.entry_offset = eo
        eo += 0x20 + _align(len(f.name), 4)
        f = f.next
    d, eo = root, 0
    while d is not None:
        d.entry_offset = eo
        eo += 0x18 if d is root else 0x18 + _align(len(d.name), 4)
        d = d.next

    f = ctx['files']
    while f is not None:
        h = _path_hash(f.parent.entry_offset, f.name)
        struct.pack_into('<IIQQII', file_table, f.entry_offset, f.parent.entry_offset,
                         ROMFS_ENTRY_EMPTY if f.sibling is None else f.sibling.entry_offset,
                         f.offset, f.size, file_hash[h % file_count], len(f.name))
        file_hash[h % file_count] = f.entry_offset
        file_table[f.entry_offset + 0x20:f.entry_offset + 0x20 + len(f.name)] = f.name
        f = f.next
    d = root
    while d is not None:
        name = b'' if d is root else d.name
        h = _path_hash(0 if d is root else d.parent.entry_offset, name)
        struct.pack_into('<IIIIII', dir_table, d.entry_offset, d.parent.entry_offset,
                         ROMFS_ENTRY_EMPTY if d.sibling is None else d.sibling.entry_offset,
                         ROMFS_ENTRY_EMPTY if d.child is None else d.child.entry_offset,
                         ROMFS_ENTRY_EMPTY if d.file is None else d.file.entry_offset,
                         dir_hash[h % dir_count], len(name))
        dir_hash[h % dir_count] = d.entry_offset
        dir_table[d.entry_offset + 0x18:d.entry_offset + 0x18 + len(name)] = name
        d = d.next

    dir_hash_ofs = _align(partition + ROMFS_FILEPARTITION_OFS, 4)
    dir_table_ofs = dir_hash_ofs + 4 * dir_count
    file_hash_ofs = dir_table_ofs + len(dir_table)
    file_table_ofs = file_hash_ofs + 4 * file_count
    header = struct.pack('<10Q', 0x50, dir_hash_ofs, 4 * dir_count, dir_table_ofs, len(dir_table),
                         file_hash_ofs, 4 * file_count, file_table_ofs, len(file_table),
                         ROMFS_FILEPARTITION_OFS)
    out.seek(base_offset)
    out.write(header)
    f = ctx['files']
    while f is not None:
        out.seek(base_offset + f.offset + ROMFS_FILEPARTITION_OFS)
        with open(f.os_path, 'rb') as src:
            shutil.copyfileobj(src, out, 0x400000)
        f = f.next
    out.seek(base_offset + dir_hash_ofs)
    out.write(struct.pack('<%dI' % dir_count, *dir_hash))
    out.write(dir_table)
    out.write(struct.pack('<%dI' % file_count, *file_hash))
    out.write(file_table)
    return dir_hash_ofs + 4 * dir_count + len(dir_table) + 4 * file_count + len(file_table)


# --------------------------------------------------------------------------- elf2nro ---

def elf2nro(elf_path, nro_path, icon=None, nacp=None, romfs=None, romfs_dir=None, aligned=False):
    """Port of switch-tools elf2nro.c."""
    elf = Path(elf_path).read_bytes()
    if elf[:4] != b'\x7fELF' or struct.unpack_from('<H', elf, 0x12)[0] != 183:
        sys.exit('elf2nro: expected an AArch64 ELF')
    e_phoff, e_shoff = struct.unpack_from('<QQ', elf, 0x20)
    e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from('<HHHH', elf, 0x36)
    phdrs = [struct.unpack_from('<IIQQQQQQ', elf, e_phoff + k * 56) for k in range(e_phnum)]
    PT_LOAD = 1

    segments, bufs = [], []
    file_off = 0
    bss_size = 0
    j = 0
    for i in range(4):
        phdr = None
        while j < e_phnum:
            cur = phdrs[j]
            if i < 2 or (i == 2 and cur[0] != PT_LOAD):
                j += 1
            if cur[0] == PT_LOAD or i == 3:
                phdr = cur
                break
        if phdr is None:
            sys.exit('elf2nro: expected 3 loadable phdrs and a bss')
        p_type, p_flags, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align = phdr
        if i == 3:
            tmpsize = (p_filesz + 0xFFF) & ~0xFFF
            bss_size = ((p_memsz - tmpsize) + 0xFFF) & ~0xFFF if p_memsz > tmpsize else 0
            break
        size = (p_filesz + 0xFFF) & ~0xFFF
        segments.append((p_vaddr & 0xFFFFFFFF, size & 0xFFFFFFFF))
        buf = bytearray(size)
        buf[:p_filesz] = elf[p_offset:p_offset + p_filesz]
        bufs.append(buf)
        file_off += size
        file_off = (file_off + 0xFFF) & ~0xFFF

    build_id = bytes(0x20)
    for k in range(e_shnum):
        sh = struct.unpack_from('<IIQQQQIIQQ', elf, e_shoff + k * e_shentsize)
        if sh[1] == 7:  # SHT_NOTE
            off = sh[4]
            namesz, descsz, ntype = struct.unpack_from('<III', elf, off)
            name = elf[off + 12:off + 12 + namesz]
            if ntype == 3 and namesz == 4 and name == b'GNU\0':
                desc = elf[off + 12 + namesz:off + 12 + namesz + min(descsz, 0x20)]
                build_id = desc + bytes(0x20 - len(desc))

    header = b'NRO0' + struct.pack('<III', 0, file_off & 0xFFFFFFFF, 1 if aligned else 0)
    for fo, sz in segments:
        header += struct.pack('<II', fo, sz)
    header += struct.pack('<II', bss_size & 0xFFFFFFFF, 0) + build_id + bytes(0x20)
    assert len(header) == 0x70

    with open(nro_path, 'wb') as out:
        for (fo, sz), buf in zip(segments, bufs):
            out.seek(fo)
            out.write(buf)
        out.seek(0x10)
        out.write(header)
        if not (icon or nacp or romfs or romfs_dir):
            return
        assets = []
        tmp_off = 0x38
        icon_data = Path(icon).read_bytes() if icon else b''
        nacp_data = Path(nacp).read_bytes() if nacp else b''
        romfs_data = Path(romfs).read_bytes() if romfs else b''
        icon_sec = (tmp_off, len(icon_data)) if icon else (0, 0)
        tmp_off += len(icon_data)
        nacp_sec = (tmp_off, len(nacp_data)) if nacp else (0, 0)
        tmp_off += len(nacp_data)
        if romfs:
            romfs_sec = (tmp_off, len(romfs_data))
        elif romfs_dir:
            size = build_romfs(romfs_dir, out, file_off + tmp_off)
            romfs_sec = (tmp_off, size)
        else:
            romfs_sec = (0, 0)
        out.seek(file_off)
        out.write(b'ASET' + struct.pack('<I', 0) + struct.pack('<QQ', *icon_sec)
                  + struct.pack('<QQ', *nacp_sec) + struct.pack('<QQ', *romfs_sec))
        if icon:
            out.seek(file_off + icon_sec[0])
            out.write(icon_data)
        if nacp:
            out.seek(file_off + nacp_sec[0])
            out.write(nacp_data)
        if romfs:
            out.seek(file_off + romfs_sec[0])
            out.write(romfs_data)


def tool_elf2nro(argv):
    if len(argv) < 2:
        sys.exit('elf2nro <elf-file> <nro-file> [--icon= --nacp= --romfs= --romfsdir= --alignedheader]')
    opts = {}
    for a in argv[2:]:
        for key in ('--icon=', '--nacp=', '--romfs=', '--romfsdir='):
            if a.startswith(key):
                opts[key[2:-1]] = a[len(key):]
    elf2nro(argv[0], argv[1], icon=opts.get('icon'), nacp=opts.get('nacp'), romfs=opts.get('romfs'),
            romfs_dir=opts.get('romfsdir'), aligned='--alignedheader' in argv[2:])
    return 0


# -------------------------------------------------------------------------- nacptool ---

def make_nacp(name, author, version, titleid=0):
    nacp = bytearray(0x4000)
    nb, ab = name.encode('utf-8')[:0x1FF], author.encode('utf-8')[:0xFF]
    for i in range(12):
        base = i * 0x300
        nacp[base:base + len(nb)] = nb
        nacp[base + 0x200:base + 0x200 + len(ab)] = ab
    vb = version.encode('utf-8')[:0xF]
    nacp[0x3060:0x3060 + len(vb)] = vb
    if titleid:
        struct.pack_into('<Q', nacp, 0x3038, titleid)
        struct.pack_into('<QQ', nacp, 0x3070, titleid + 0x1000, titleid)
        struct.pack_into('<Q', nacp, 0x30B0, titleid)
    struct.pack_into('<I', nacp, 0x3024, 0x100)
    struct.pack_into('<I', nacp, 0x302C, 0xBFF)
    struct.pack_into('<I', nacp, 0x3034, 0x10000)
    nacp[0x3040:0x3060] = bytes([0x0C, 0xFF, 0xFF, 0x0A, 0xFF, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0D, 0x0D]
                                + [0xFF] * 20)
    struct.pack_into('<I', nacp, 0x3080, 0x3E00000)
    struct.pack_into('<I', nacp, 0x3088, 0x180000)
    struct.pack_into('<I', nacp, 0x30F0, 0x102)
    return bytes(nacp)


def tool_nacptool(argv):
    if len(argv) < 5 or not argv[0].startswith('--create'):
        sys.exit('nacptool --create <name> <author> <version> <outfile> [--titleid=]')
    titleid = 0
    for a in argv[5:]:
        if a.startswith('--titleid='):
            titleid = int(a[10:], 16)
    Path(argv[4]).write_bytes(make_nacp(argv[1], argv[2], argv[3], titleid))
    return 0


# ----------------------------------------------------------------------------- bin2s ---

def _ident(src):
    out, got_first = [], False
    for ch in src:
        if ch.isdigit() and not got_first:
            out.append('_')
        if not (ch.isascii() and (ch.isalnum() or ch == '_')):
            ch = '_' if ch in '-./' else ''
        if ch:
            out.append(ch)
            got_first = True
    return ''.join(out)


def tool_bin2s(argv):
    alignment, header, files, i = 4, None, [], 0
    while i < len(argv):
        a = argv[i]
        if a in ('-a', '--alignment'):
            alignment = int(argv[i + 1], 0)
            i += 2
            continue
        if a.startswith('-a') and len(a) > 2:
            alignment = int(a[2:], 0)
        elif a in ('-H', '--header'):
            header = argv[i + 1]
            i += 2
            continue
        else:
            files.append(a)
        i += 1
    sys.stdout.reconfigure(newline='\n')  # LF output, as bin2s writes it on Linux
    w = sys.stdout
    hdr = None
    if header:
        hdr = open(header, 'w', newline='\n')
        hdr.write('/* Generated by BIN2S - please don\'t edit directly */\n#pragma once\n'
                  '#include <stddef.h>\n#include <stdint.h>\n\n')
    for path in files:
        data = Path(path).read_bytes()
        if not data:
            print(f'bin2s: warning: skipping empty file {path}', file=sys.stderr)
            continue
        ident = _ident(re.split(r'[\\/]', path)[-1])
        lines = ['/* Generated by BIN2S - please don\'t edit directly */',
                 f'\t.section .rodata.{ident}, "a"', f'\t.balign {alignment}', f'\t.global {ident}',
                 f'{ident}:']
        rows = [','.join('%3u' % b for b in data[k:k + 16]) for k in range(0, len(data), 16)]
        lines.append('\t.byte ' + '\n\t.byte '.join(rows))
        lines += ['', f'\t.global {ident}_end', f'{ident}_end:', '']
        if hdr:
            hdr.write(f'extern const uint8_t {ident}[];\nextern const uint8_t {ident}_end[];\n'
                      f'#if __cplusplus >= 201103L\nstatic constexpr size_t {ident}_size={len(data)};\n'
                      f'#else\nstatic const size_t {ident}_size={len(data)};\n#endif\n')
        else:
            lines += [f'\t.global {ident}_size', '\t.balign 4', f'{ident}_size: .int {len(data)}']
        w.write('\n'.join(lines) + '\n\n\n#if defined(__linux__) && defined(__ELF__)\n'
                '.section .note.GNU-stack,"",%progbits\n#endif')
    if hdr:
        hdr.close()
    return 0


# ------------------------------------------------------------------------ uam, pkgconf ---

def tool_uam(argv):
    out, src, i = None, None, 0
    while i < len(argv):
        if argv[i] == '-o':
            out = argv[i + 1]
            i += 2
            continue
        if argv[i] == '-s':
            i += 2
            continue
        src = argv[i]
        i += 1
    if not out or not src:
        sys.exit('uam: -o <out> -s <stage> <source>')
    pre = env_path('KPNX_DKSH') / (Path(out).stem + '.dksh')
    manifest = env_path('KPNX_DKSH') / 'sources.sha256'
    if manifest.is_file():
        want = dict(line.split('  ', 1)[::-1] for line in manifest.read_text().splitlines() if line)
        have = hashlib.sha256(Path(src).read_bytes().replace(b'\r\n', b'\n')).hexdigest()
        if want.get(Path(src).name) != have:
            sys.exit(f'uam: {Path(src).name} differs from the shader the toolkit precompiled')
    if not pre.is_file():
        sys.exit(f'uam: no precompiled shader {pre.name} in the toolkit')
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(pre, out)
    return 0


def tool_pkgconfig(argv):
    if '--version' in argv:
        print('0.29.2')
        return 0
    return 1


TOOLS = {'link': tool_link, 'elf2nro': tool_elf2nro, 'nacptool': tool_nacptool, 'bin2s': tool_bin2s,
         'uam': tool_uam, 'pkg-config': tool_pkgconfig}


def main():
    if len(sys.argv) < 2 or sys.argv[1] not in TOOLS:
        sys.exit('usage: nxtools.py {%s} ...' % '|'.join(TOOLS))
    sys.exit(TOOLS[sys.argv[1]](sys.argv[2:]))


if __name__ == '__main__':
    main()
