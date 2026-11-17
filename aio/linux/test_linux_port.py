import io, os, sys, stat, tarfile, zipfile, json, tempfile, shutil, importlib.util, struct
from pathlib import Path
AIO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AIO / 'pipeline'))
import kpnx_aio as k

tmp = Path(tempfile.mkdtemp())
ok = []
def check(name, cond):
    print(('PASS ' if cond else 'FAIL ') + name); ok.append(cond)

# ---- 1. extract_tar: layer-like archive (strip + include + modes + symlinks + hardlink)
arc = tmp / 'layer.tar.gz'
with tarfile.open(arc, 'w:gz') as t:
    def add(name, data=b'x', mode=0o644, typ=None, link=None):
        ti = tarfile.TarInfo(name); ti.mode = mode
        if typ: ti.type = typ; ti.linkname = link
        else: ti.size = len(data)
        t.addfile(ti, io.BytesIO(data) if not typ else None)
    add('opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++', b'#!elf', 0o755)
    add('opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcc', typ=tarfile.SYMTYPE, link='aarch64-none-elf-g++')
    add('opt/devkitpro/tools/bin/elf2nro', b'bin', 0o755)
    add('opt/devkitpro/tools/bin/elf2nro-hard', typ=tarfile.LNKTYPE, link='opt/devkitpro/tools/bin/elf2nro')
    add('opt/devkitpro/libnx/switch.ld', b'ld', 0o644)
    add('opt/devkitpro/libnx/abs', typ=tarfile.SYMTYPE, link='/opt/devkitpro/libnx/switch.ld')
    add('opt/devkitpro/libnx/outside', typ=tarfile.SYMTYPE, link='/usr/lib/x')
    add('opt/devkitpro/pacman/skip', b'no')
    add('usr/other', b'no')
dest = tmp / 'dkp'
k.extract_tar(arc, dest, 'r:gz', strip='opt/devkitpro/', include=['devkitA64/', 'libnx/', 'tools/'])
g = dest / 'devkitA64/bin/aarch64-none-elf-g++'
check('tar: exec bit kept', g.exists() and os.access(g, os.X_OK))
check('tar: data file not executable', not os.access(dest / 'libnx/switch.ld', os.X_OK))
sl = dest / 'devkitA64/bin/aarch64-none-elf-gcc'
check('tar: relative symlink is a symlink and resolves', sl.is_symlink() and sl.resolve() == g.resolve())
ab = dest / 'libnx/abs'
check('tar: absolute symlink rewritten inside tree', ab.is_symlink() and not os.readlink(ab).startswith('/') and ab.resolve() == (dest / 'libnx/switch.ld').resolve())
check('tar: symlink to outside skipped', not (dest / 'libnx/outside').exists() and not (dest / 'libnx/outside').is_symlink())
check('tar: hardlink usable', (dest / 'tools/bin/elf2nro-hard').read_bytes() == b'bin')
check('tar: include filter', not (dest / 'pacman').exists() and not (dest / 'usr').exists())

# ---- 2. extract_zip keeps x bit
z = tmp / 't.zip'
with zipfile.ZipFile(z, 'w') as zf:
    zi = zipfile.ZipInfo('ninja'); zi.create_system = 3; zi.external_attr = (0o755 | stat.S_IFREG) << 16
    zf.writestr(zi, b'n')
    zf.writestr('plain.txt', b'p')
d2 = tmp / 'z'
k.extract_zip(z, d2)
check('zip: exec bit kept', os.access(d2 / 'ninja', os.X_OK) and not os.access(d2 / 'plain.txt', os.X_OK))

# ---- 3. FST parse + FNV
names = [b'', b'a.bin', b'dir', b'b.bin']
fst = bytearray(); nametab = b''; offs = []
for n in (b'a.bin', b'dir', b'b.bin'):
    offs.append(len(nametab)); nametab += n + b'\0'
fst += struct.pack('>III', 1 << 24, 0, 4)                 # root, 4 entries
fst += struct.pack('>III', offs[0], 0, 100)               # file a.bin size 100
fst += struct.pack('>III', (1 << 24) | offs[1], 0, 4)     # dir 'dir', ends at index 4
fst += struct.pack('>III', offs[2], 0, 7)                 # file dir/b.bin size 7
fst = bytes(fst) + nametab
check('fst: paths and sizes', k.fst_files(fst) == [('a.bin', 100), ('dir/b.bin', 7)])
check('fnv1a known vector', k.fnv1a(b'a') == 0xaf63dc4c8601ec8c)

# ---- 4. wrappers on a fake devkitPro tree
dkp = tmp / 'tree'
def mk(rel, content=b'\x7fELF', mode=0o755):
    p = dkp / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(content); os.chmod(p, mode); return p
(dkp / 'libnx').mkdir(parents=True)
mk('devkitA64/bin/aarch64-none-elf-g++'); mk('tools/bin/elf2nro'); mk('tools/bin/nacptool')
mk('tools/bin/bin2s'); mk('portlibs/switch/bin/aarch64-none-elf-pkg-config', b'#!/bin/sh\nexec pkg-config /opt/devkitpro\n')
# uam deliberately missing
tools = k.Tools(tmp / 'data', k.load_manifest(str(AIO / 'manifest/tools.json')), None, {'link': 'auto'})
tools.dkp = dkp
tools.install_wrappers()
uam = dkp / 'tools/bin/uam'
check('wrappers: missing uam gets a wrapper', uam.is_file() and k.Tools.WRAPPER_MARK in uam.read_text() and ' uam "$@"' in uam.read_text())
check('wrappers: real tools untouched', (dkp / 'tools/bin/elf2nro').read_bytes() == b'\x7fELF')
check('wrappers: auto -> gnu link', tools.linker == 'gnu' and (dkp / 'devkitA64/bin/aarch64-none-elf-g++').read_bytes() == b'\x7fELF')
tools.opts['link'] = 'lld'
tools.install_wrappers()
gpp = dkp / 'devkitA64/bin/aarch64-none-elf-g++'
check('wrappers: --link lld wraps g++, real kept as backup', tools.linker == 'lld' and k.Tools.WRAPPER_MARK in gpp.read_text()
      and (gpp.with_name(gpp.name + '.dkp-original')).read_bytes() == b'\x7fELF')
tools.opts['link'] = 'auto'
tools.install_wrappers()
check('wrappers: switching back restores the real g++', tools.linker == 'gnu' and gpp.read_bytes() == b'\x7fELF'
      and not gpp.with_name(gpp.name + '.dkp-original').exists())
tools.install_wrappers()
check('wrappers: idempotent', gpp.read_bytes() == b'\x7fELF' and k.Tools.WRAPPER_MARK in uam.read_text())
# wrapper actually runs nxtools
env = dict(os.environ, KPNX_PYTHON=sys.executable, KPNX_NXTOOLS=str(AIO / 'pipeline/nxtools.py'))
import subprocess
r = subprocess.run([str(dkp / 'tools/bin/uam')], env=env, capture_output=True, text=True)
check('wrapper executes nxtools (uam usage message)', 'uam: -o <out>' in r.stderr)
nacp = tmp / 'x.nacp'
w = dkp / 'tools/bin/nacptool'; w.write_text(f'#!/bin/sh\n{k.Tools.WRAPPER_MARK}\nexec "${{KPNX_PYTHON:-python3}}" "$KPNX_NXTOOLS" nacptool "$@"\n')
r = subprocess.run([str(w), '--create', 'N', 'A', '1.0', str(nacp)], env=env)
check('wrapper nacptool writes a 0x4000-byte NACP', r.returncode == 0 and nacp.stat().st_size == 0x4000)

# ---- 5. fix_devkitpro prefix rewrite
(dkp / 'cmake/Platform').mkdir(parents=True)
(dkp / 'cmake/dkp-initialize-path.cmake').write_text('set(X /opt/devkitpro)\n')
(dkp / 'cmake/Platform/NintendoSwitch.cmake').write_text('# none\n')
(dkp / 'portlibs/switch/lib/pkgconfig').mkdir(parents=True)
(dkp / 'portlibs/switch/lib/pkgconfig/z.pc').write_text('prefix=/opt/devkitpro/portlibs/switch\n')
tools.fix_devkitpro()
check('fix: .cmake rewritten', str(dkp) in (dkp / 'cmake/dkp-initialize-path.cmake').read_text())
check('fix: .pc rewritten', str(dkp) in (dkp / 'portlibs/switch/lib/pkgconfig/z.pc').read_text())
check('fix: bin script rewritten, ELF untouched',
      str(dkp) in (dkp / 'portlibs/switch/bin/aarch64-none-elf-pkg-config').read_text()
      and (dkp / 'devkitA64/bin/aarch64-none-elf-g++').read_bytes() == b'\x7fELF')

# ---- 6. manifest overlay and CMake command
m = k.load_manifest(str(AIO / 'manifest/tools.json'))
check('manifest: overlay replaces tools', 'linux' in m['cmake']['url'].lower() and 'linux' in m['llvm']['url'].lower() and not m['cmake']['url'].endswith('.zip'))
check('manifest: retro_rewind kept from tools.json', m['retro_rewind']['version'] == '6.13.4' and 'python' in m)
check('manifest: devkitpro include has host dirs', 'devkitA64/' in m['devkitpro']['include'] and 'tools/' in m['devkitpro']['include'])
ws = tmp / 'ws'
run = k.Runner(tools, ws)
tr = ws / 'private/self-build/translation'
cmd = [str(c) for c in k.cmake_configure_cmd(run, 'base', ws / 'build/x', tr)]
check('cmake: no .exe anywhere', not any('.exe' in c for c in cmd))
check('cmake: gnu ld as CMAKE_LINKER', any(c.startswith('-DCMAKE_LINKER=') and c.endswith('aarch64-none-elf-ld') for c in cmd))
tools.linker = 'lld'
cmd = [str(c) for c in k.cmake_configure_cmd(run, 'base', ws / 'build/x', tr)]
check('cmake: lld mode uses ld.lld', any(c.startswith('-DCMAKE_LINKER=') and c.endswith('/ld.lld') for c in cmd))
check('runner env: PATH has llvm, dkp tools and system dirs', run.env['PATH'].split(os.pathsep)[0].endswith('llvm/bin')
      and str(dkp / 'tools/bin') in run.env['PATH'] and '/usr/bin' in run.env['PATH'])
check('runner env: DEVKITPRO and shim vars', run.env['DEVKITPRO'] == str(dkp) and run.env['KPNX_NXTOOLS'].endswith('nxtools.py'))

# ---- 7. fake system tools are found / rejected
fake = tmp / 'bin'; fake.mkdir()
def script(name, body):
    p = fake / name; p.write_text('#!/bin/sh\n' + body + '\n'); os.chmod(p, 0o755)
script('cmake', 'echo "cmake version 3.31.12"'); script('ninja', 'echo 1.13.2')
script('dotnet', 'echo "Microsoft.NETCore.App 8.0.12 [/x]"')
old = os.environ['PATH']; os.environ['PATH'] = f'{fake}:{old}'
check('system: cmake 3.31 accepted', k.find_system_tool('cmake') == fake / 'cmake')
check('system: ninja found', k.find_system_tool('ninja') == fake / 'ninja')
check('system: dotnet 8 accepted', k.find_system_tool('dotnet') is not None)
script('cmake', 'echo "cmake version 3.22.1"')
check('system: cmake 3.22 rejected', k.find_system_tool('cmake') is None)
script('dotnet', 'echo "Microsoft.NETCore.App 6.0.1 [/x]"')
check('system: dotnet 6 rejected', k.find_system_tool('dotnet') is None)
for n in k.LLVM_BINS: script(n, 'echo "clang version 19.1.7"')
root = tmp / 'llvmroot'; (root / 'bin').mkdir(parents=True)
os.environ['KPNX_LLVM_DIR'] = str(root)
for n in k.LLVM_BINS: shutil.copy(fake / n, root / 'bin' / n)
check('system: LLVM 19 root found', k.find_system_tool('llvm') == root)
for n in k.LLVM_BINS: (root / 'bin' / n).write_text('#!/bin/sh\necho "clang version 18.1.8"\n')
check('system: LLVM 18 rejected', k.find_system_tool('llvm') != root)
os.environ['PATH'] = old

# ---- 8. zstd fallbacks
raw = os.urandom(1000) + b'A' * 100000
try:
    c = k.zstd_compress(raw); print('zstd path produced', len(c), 'bytes')
    check('zstd: produced a zstd frame', c[:4] == b'\x28\xb5\x2f\xfd')
except k.Failure as e:
    print('zstd: no compressor available here:', str(e)[:60]); check('zstd: clear failure without a compressor', 'no zstd compressor' in str(e))

# ---- 9. progress output off-tty
import contextlib
buf = io.StringIO()
with contextlib.redirect_stdout(buf):
    for i in range(0, 101, 5): k.emit(progress='download', item='x', done=i * 1000000, total=100000000)
    k.emit(stage='a', title='b')
lines = buf.getvalue().strip().splitlines()
check('emit: plain progress, ~10 lines, no @@', 9 <= len(lines) <= 12 and not any('@@' in l for l in lines))


# ---- 10. builder/*.sh copied from the repo checkout (link-switch.sh)
import contextlib as _c
repo = tmp / 'repo'; (repo / 'builder').mkdir(parents=True)
(repo / 'builder/link-switch.sh').write_text('#!/bin/sh\nexec "$@"\n')
ws2 = tmp / 'ws2'; (ws2 / 'switch').mkdir(parents=True); (ws2 / 'builder').mkdir()
(ws2 / 'switch/CMakeLists.txt').write_text('bash "${CMAKE_CURRENT_SOURCE_DIR}/../builder/link-switch.sh"\n')
class R: pass
r2 = R(); r2.ws = ws2
k.REPO_ROOT = repo
with _c.redirect_stdout(io.StringIO()): k.ensure_builder_scripts(r2)
s = ws2 / 'builder/link-switch.sh'
print('PASS' if s.is_file() and os.access(s, os.X_OK) else 'FAIL', 'link-switch.sh copied and executable'); ok.append(s.is_file() and os.access(s, os.X_OK))
k.REPO_ROOT = tmp / 'norepo'; s.unlink()
try:
    with _c.redirect_stdout(io.StringIO()): k.ensure_builder_scripts(r2)
    good = False
except k.Failure as e:
    good = 'git clone' in str(e)
print('PASS' if good else 'FAIL', 'clear error when the repo has no script'); ok.append(good)
print('\n%d/%d passed' % (sum(ok), len(ok)))
sys.exit(0 if all(ok) else 1)
