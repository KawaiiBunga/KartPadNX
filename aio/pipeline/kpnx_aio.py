#!/usr/bin/env python3
"""KartPad-NX AIO builder pipeline: the user's disc image in, a ready SD card folder out.

  python kpnx_aio.py --data DIR --disc IMAGE --out DIR [--rr] [--jobs N]
                     [--toolkit ZIP|URL] [--manifest tools.json] [--shim nxshim.exe]

--out gets exactly what goes on the SD card (switch/mkw-launcher.nro, the extracted game in
switch/kartpad/data, and with --rr Retro Rewind in switch/kartpad/retro-rewind), so nothing
has to be imported on the Switch.

Runs on Windows with nothing installed. Every tool is a pinned, hash-checked portable
download kept under --data (see tools.json). Stages are resumable: each records what it was
built from in <data>/state and is skipped when nothing changed.

  tools      .NET runtime, CMake, Ninja, nodtool, LLVM 19.1.7, devkitPro target files
  toolkit    prebuilt KartPad-NX parts (aio/toolkit/make-toolkit.py) -> <data>/ws
  disc       nodtool extracts the ISO/WBFS/RVZ; main.dol/StaticR.rel must be PAL RMCP01 rev0
  translate  static recompilation (base, and Retro Rewind with --rr)
  transform  the repo's source passes over the translation
  build      CMake + Ninja + clang -> game NROs
  launcher   both games zstd-packed into the launcher RomFS -> <out>/switch/mkw-launcher.nro
  sd-game    (in the background) the disc's sys/ and files/ plus the extraction manifest
             -> <out>/switch/kartpad/data
  rr-sd      with --rr: the built version's full Retro Rewind pack
             -> <out>/switch/kartpad/retro-rewind/RetroRewind6

Lines starting with "@@" are JSON progress events for the frontend; everything else is log.
"""
import argparse
import concurrent.futures
import ctypes
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import threading
import time
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path, PurePosixPath

HERE = Path(__file__).resolve().parent
USER_AGENT = 'KartPadNX-Builder/1.0'
EXPECT_DOL = '80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05'
EXPECT_REL = '16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d'
BASE_FUNCTION_COUNT = 29065
ENTRY = '0x800060A4'
TRANSFORM_PASSES = ('inject-scene-register-boundary', 'inject-archive-wait-checkpoint',
                    'inject-archive-call-boundary', 'fix-rr-menu-model-flow', 'inject-globe-scene-boundary')
FETCH_DEPS = ('fmt', 'imgui', 'sqlite3', 'tracy', 'xxhash', 'zstd')
PROFILES = {
    'base': {'build': 'build/switch-fullapp', 'translation': 'private/self-build/translation',
             'app': 'KartPad-NX', 'nro': 'kartpad.nro', 'pgo': 'current', 'cpu_rewrites': False,
             'variant': 'vanilla', 'payload': 'kartpad.nro.zst'},
    'retro-rewind': {'build': 'build/switch-fullapp-rr', 'translation': 'private/self-build/retro-rewind/translation',
                     'app': 'KartPad Retro Rewind', 'nro': 'kartpad-retro-rewind.nro', 'pgo': 'current-retro-rewind',
                     'cpu_rewrites': True, 'variant': 'rr', 'payload': 'kartpad-retro-rewind.nro.zst'},
}


class Failure(Exception):
    pass


# ------------------------------------------------------------------------------- ui ---

LOG = None
OUT_LOCK = threading.RLock()   # the devkitPro job prints from a background thread


def emit(**event):
    with OUT_LOCK:
        print('@@' + json.dumps(event), flush=True)


def log(msg):
    with OUT_LOCK:
        print(msg, flush=True)
        if LOG:
            LOG.write(msg + '\n')
            LOG.flush()


def stage(name, title):
    emit(stage=name, title=title)
    log(f'\n=== {title} ===')


# -------------------------------------------------------------------------- helpers ---

def sha256_file(path, chunk=1 << 20):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while b := f.read(chunk):
            h.update(b)
    return h.hexdigest()


def total_ram_gb():
    class MEMORYSTATUSEX(ctypes.Structure):
        _fields_ = [('dwLength', ctypes.c_ulong), ('dwMemoryLoad', ctypes.c_ulong),
                    ('ullTotalPhys', ctypes.c_ulonglong), ('ullAvailPhys', ctypes.c_ulonglong),
                    ('ullTotalPageFile', ctypes.c_ulonglong), ('ullAvailPageFile', ctypes.c_ulonglong),
                    ('ullTotalVirtual', ctypes.c_ulonglong), ('ullAvailVirtual', ctypes.c_ulonglong),
                    ('ullAvailExtendedVirtual', ctypes.c_ulonglong)]
    try:
        s = MEMORYSTATUSEX()
        s.dwLength = ctypes.sizeof(s)
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(s))
        return s.ullTotalPhys / (1 << 30)
    except Exception:
        return 8.0


def default_jobs():
    # A translated shard needs up to ~2.5 GB of RAM at -O3; leave room for the OS.
    by_ram = int((total_ram_gb() - 4) // 2.5)
    return max(1, min(os.cpu_count() or 2, by_ram))


def fwd(path):
    return str(path).replace('\\', '/')


class State:
    """Per-stage completion keys under <data>/state."""

    def __init__(self, root):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)

    def done(self, name, key):
        p = self.root / (name + '.done')
        return p.is_file() and p.read_text(encoding='utf-8') == key

    def mark(self, name, key):
        (self.root / (name + '.done')).write_text(key, encoding='utf-8')

    def clear(self, name):
        p = self.root / (name + '.done')
        if p.exists():
            p.unlink()


# ------------------------------------------------------------------------- download ---

PARALLEL_MIN = 24 << 20      # files at least this big are fetched over several connections
PARALLEL_PART = 8 << 20
PARALLEL_CONNECTIONS = 8


def _request(url, headers=None, unredirected=None, byte_range=None):
    req = urllib.request.Request(url, headers={'User-Agent': USER_AGENT, **(headers or {})})
    for k, v in (unredirected or {}).items():
        req.add_unredirected_header(k, v)
    if byte_range:
        req.add_header('Range', 'bytes=%d-%d' % byte_range)
    return urllib.request.urlopen(req, timeout=60)


def _ranged_download(url, part, size, label):
    """Fetch [0, size) of url into part over several connections. Returns False when the
    server does not serve ranges."""
    with _request(url, byte_range=(0, 0)) as r:
        if r.status != 206 or not (r.headers.get('Content-Range') or '').endswith('/%d' % size):
            return False
    with open(part, 'wb') as f:
        f.truncate(size)
    ranges = [(a, min(a + PARALLEL_PART, size) - 1) for a in range(0, size, PARALLEL_PART)]
    lock = threading.Lock()
    got = [0]

    def fetch(rng):
        for attempt in range(4):
            done_here = 0
            try:
                with _request(url, byte_range=rng) as r, open(part, 'r+b') as f:
                    if r.status != 206:
                        raise Failure('range request refused')
                    f.seek(rng[0])
                    while chunk := r.read(1 << 18):
                        f.write(chunk)
                        done_here += len(chunk)
                        with lock:
                            got[0] += len(chunk)
                if done_here != rng[1] - rng[0] + 1:
                    raise Failure('short range')
                return
            except Exception:
                with lock:
                    got[0] -= done_here
                if attempt == 3:
                    raise
                time.sleep(1 + attempt)

    with concurrent.futures.ThreadPoolExecutor(PARALLEL_CONNECTIONS) as pool:
        futures = [pool.submit(fetch, rng) for rng in ranges]
        while not all(f.done() for f in futures):
            time.sleep(0.5)
            emit(progress='download', item=label, done=got[0], total=size)
        for f in futures:
            f.result()
    emit(progress='download', item=label, done=size, total=size)
    return True


def download(url, dest, size=None, sha256=None, label=None, headers=None, unredirected=None):
    dest = Path(dest)
    if dest.is_file() and sha256 and dest.stat().st_size == (size or dest.stat().st_size) \
            and sha256_file(dest) == sha256:
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + '.part')
    label = label or dest.name
    ranged = False
    if size and size >= PARALLEL_MIN:
        try:
            # Resolve redirects once (registry -> CDN, GitHub -> object store); the parts go to
            # the final URL, which carries its own signature and needs no Authorization.
            with _request(url, headers, unredirected, byte_range=(0, 0)) as r:
                final = r.geturl()
            ranged = _ranged_download(final, part, size, label)
        except Exception as e:
            log(f'{label}: parallel download unavailable ({e}); using one connection')
            ranged = False
    if not ranged:
        got, last = 0, 0
        with _request(url, headers, unredirected) as r, open(part, 'wb') as f:
            total = size or int(r.headers.get('Content-Length') or 0)
            while chunk := r.read(1 << 20):
                f.write(chunk)
                got += len(chunk)
                if time.time() - last > 0.5:
                    last = time.time()
                    emit(progress='download', item=label, done=got, total=total)
        emit(progress='download', item=label, done=got, total=total or got)
    if size and part.stat().st_size != size:
        part.unlink()
        raise Failure(f'{label}: downloaded {part.stat().st_size} bytes, expected {size}')
    if sha256:
        digest = sha256_file(part)
        if digest != sha256:
            part.unlink()
            raise Failure(f'{label}: SHA-256 mismatch (got {digest})')
    os.replace(part, dest)
    return dest


def docker_blob(spec, dest):
    with urllib.request.urlopen(urllib.request.Request(spec['auth'], headers={'User-Agent': USER_AGENT}),
                                timeout=60) as r:
        token = json.load(r)['token']
    url = f"{spec['registry']}/v2/{spec['repository']}/blobs/{spec['digest']}"
    # The registry redirects to a CDN; the bearer token must not follow it.
    return download(url, dest, spec['size'], spec['digest'].split(':', 1)[1], label='devkitPro (Docker Hub)',
                    unredirected={'Authorization': 'Bearer ' + token})


# -------------------------------------------------------------------------- extract ---

def _strip(name, strip):
    name = name.replace('\\', '/')
    if isinstance(strip, int):
        parts = name.split('/')
        return '/'.join(parts[strip:]) if len(parts) > strip else ''
    if isinstance(strip, str):
        return name[len(strip):] if name.startswith(strip) else ''
    return name


def _wanted(rel, include):
    return include is None or any(rel == p or (p.endswith('/') and rel.startswith(p)) for p in include)


def extract_zip(archive, dest, strip=0, include=None, label=None, workers=8):
    """Extract (part of) a zip with several threads: inflate releases the GIL, and Windows'
    per-file costs (create, antivirus scan) overlap. Works on a zip appended to an exe."""
    dest = Path(dest)
    jobs = []
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            rel = _strip(info.filename, strip)
            if not rel or not _wanted(rel, include):
                continue
            if '..' in PurePosixPath(rel).parts or PurePosixPath(rel).is_absolute():
                raise Failure(f'unsafe path in {archive}: {info.filename}')
            if info.is_dir():
                (dest / rel).mkdir(parents=True, exist_ok=True)
            else:
                jobs.append((info, dest / rel))
    for d in {out.parent for _, out in jobs}:
        d.mkdir(parents=True, exist_ok=True)
    # Largest first, dealt round-robin, so no worker ends up holding all the big files.
    jobs.sort(key=lambda j: -j[0].file_size)
    buckets = [jobs[i::workers] for i in range(workers)]
    total = sum(info.file_size for info, _ in jobs)
    lock = threading.Lock()
    done = [0]

    def work(bucket):
        with zipfile.ZipFile(archive) as z:
            for info, out in bucket:
                with z.open(info) as src, open(out, 'wb') as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
                with lock:
                    done[0] += info.file_size

    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        futures = [pool.submit(work, b) for b in buckets if b]
        while not all(f.done() for f in futures):
            time.sleep(0.5)
            if label:
                emit(progress='extract', item=label, done=done[0], total=total)
        for f in futures:
            f.result()
    return len(jobs)


def extract_tar(archive, dest, mode, strip=0, include=None, label=None):
    """Extract selected members; links become copies of their targets (Windows has no
    unprivileged symlinks)."""
    dest = Path(dest)
    links = []
    count = 0
    with tarfile.open(archive, mode) as t:
        for m in t:
            rel = _strip(m.name, strip)
            if not rel or not _wanted(rel, include):
                continue
            if '..' in PurePosixPath(rel).parts or PurePosixPath(rel).is_absolute():
                raise Failure(f'unsafe path in {archive}: {m.name}')
            out = dest / rel
            if m.isdir():
                out.mkdir(parents=True, exist_ok=True)
            elif m.isfile():
                out.parent.mkdir(parents=True, exist_ok=True)
                with t.extractfile(m) as src, open(out, 'wb') as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
                count += 1
                if count % 500 == 0:
                    emit(progress='extract', item=label or Path(archive).name, done=count, total=0)
            elif m.issym() or m.islnk():
                links.append((rel, m.linkname, m.issym()))
    for rel, target, sym in links:
        if sym:
            tgt = PurePosixPath(rel).parent / target if not target.startswith('/') else PurePosixPath(_strip(target.lstrip('/'), strip))
        else:
            tgt = PurePosixPath(_strip(target, strip))
        src = dest / str(tgt)
        if src.is_file():
            out = dest / rel
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, out)


# ---------------------------------------------------------------------------- bundle ---

class Bundle:
    """The zip appended to KartPadNX-Builder.exe (aio/toolkit/make-bundle.py): python/,
    tools/<name>/ for every redistributable tool, toolkit/, and bundle.json. Python's zipfile
    reads it in place; nothing is copied out of the exe first."""

    def __init__(self, path):
        self.path = Path(path)
        with zipfile.ZipFile(self.path) as z:
            self.names = z.namelist()
            self.info = json.loads(z.read('bundle.json'))

    def has_tool(self, name):
        return name in self.info.get('tools', {})

    def install_tool(self, name, dest):
        emit(progress='extract', item=name, done=0, total=0)
        extract_zip(self.path, dest, strip=f'tools/{name}/', label=name)
        # Same-binary aliases (clang++ = clang, ld.lld = lld, ...) are stored once.
        for alias, target in self.info.get('aliases', {}).get(name, {}).items():
            a, t = dest / alias, dest / target
            a.unlink(missing_ok=True)
            try:
                os.link(t, a)
            except OSError:
                shutil.copyfile(t, a)

    def has_toolkit(self):
        return 'toolkit_sha256' in self.info

    def extract_toolkit(self, dest):
        return extract_zip(self.path, dest, strip='toolkit/', label='toolkit')


class Background:
    """Runs fn on a thread; join() re-raises its exception."""

    def __init__(self, fn):
        self.error = None

        def body():
            try:
                fn()
            except BaseException as e:  # noqa: BLE001 - re-raised in join()
                self.error = e
        self.thread = threading.Thread(target=body, daemon=True)
        self.thread.start()

    def join(self):
        self.thread.join()
        if self.error:
            raise self.error


# ----------------------------------------------------------------------------- tools ---

class Tools:
    def __init__(self, data, manifest, shim):
        self.data = Path(data)
        self.root = self.data / 'tools'
        self.dl = self.data / 'downloads'
        self.m = manifest
        self.shim = Path(shim) if shim else None
        self.python = Path(sys.executable)
        self.dotnet = self.root / 'dotnet' / 'dotnet.exe'
        self.cmake = self.root / 'cmake' / 'bin' / 'cmake.exe'
        self.ninja = self.root / 'ninja' / 'ninja.exe'
        self.nodtool = self.root / 'nodtool' / 'nodtool.exe'
        self.llvm = self.root / 'llvm'
        self.dkp = self.root / 'devkitpro'

    def key(self, name):
        spec = self.m[name]
        return spec.get('sha256') or spec.get('digest')

    LOCAL = ('dotnet', 'cmake', 'ninja', 'nodtool', 'llvm')

    def install_local(self, state, bundle, keep_downloads=False):
        """Everything but devkitPro: from the bundle inside the exe when it has the tool,
        otherwise from its pinned download."""
        stage('tools', 'Setting up build tools')
        for name in self.LOCAL:
            dest = self.root / name
            if state.done('tool-' + name, self.key(name)) and dest.exists():
                continue
            if dest.exists():
                shutil.rmtree(dest)
            if bundle and bundle.has_tool(name):
                bundle.install_tool(name, dest)
            else:
                self.download_tool(name, dest, keep_downloads)
            state.mark('tool-' + name, self.key(name))

    def download_tool(self, name, dest, keep_downloads):
        spec = self.m[name]
        log(f'{name}: {spec["url"]}')
        fname = spec.get('name') or Path(spec['url'].split('?')[0]).name
        arc = download(spec['url'], self.dl / fname, spec['size'], spec['sha256'], label=name)
        emit(progress='extract', item=name, done=0, total=0)
        if spec['type'] == 'zip':
            extract_zip(arc, dest, strip=spec.get('strip', 0), include=spec.get('include'), label=name)
        elif spec['type'] == 'tar':
            extract_tar(arc, dest, 'r:*', strip=spec.get('strip', 0), include=spec.get('include'), label=name)
        elif spec['type'] == 'file':
            dest.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(arc, dest / fname)
        if not keep_downloads:
            arc.unlink(missing_ok=True)

    def install_devkitpro(self, state, keep_downloads=False):
        """devkitPro's own Docker Hub image layer; it cannot be rehosted, so it is always a
        download. Runs in the background while the disc is read and the game translated."""
        if state.done('tool-devkitpro', self.key('devkitpro')) and self.dkp.exists():
            return
        spec = self.m['devkitpro']
        log(f'devkitpro: {spec["repository"]} (background)')
        if self.dkp.exists():
            shutil.rmtree(self.dkp)
        arc = docker_blob(spec, self.dl / 'devkitpro-layer.tar.gz')
        emit(progress='extract', item='devkitPro', done=0, total=0)
        extract_tar(arc, self.dkp, 'r:gz', strip=spec['strip'], include=spec['include'], label='devkitPro')
        self.fix_devkitpro()
        if not keep_downloads:
            arc.unlink(missing_ok=True)
        state.mark('tool-devkitpro', self.key('devkitpro'))
        log('devkitpro ready')

    def finish(self):
        self.install_shims()
        for exe in (self.dotnet, self.cmake, self.ninja, self.nodtool, self.llvm / 'bin/clang.exe',
                    self.llvm / 'bin/ld.lld.exe', self.dkp / 'libnx/switch.ld'):
            if not exe.exists():
                raise Failure(f'tool missing after install: {exe}')

    def fix_devkitpro(self):
        # devkitPro's CMake refuses a Windows host (it expects msys2 CMake); everything it
        # actually needs works with native CMake once the host check is gone.
        p = self.dkp / 'cmake' / 'dkp-initialize-path.cmake'
        text = p.read_text(encoding='utf-8')
        text = re.sub(r'if\(CMAKE_HOST_WIN32\)\s*message\(FATAL_ERROR[^)]*\)\s*endif\(\)', '', text)
        p.write_text(text, encoding='utf-8')
        # Quote the platform's linker-flag paths: the work folder may contain spaces.
        p = self.dkp / 'cmake' / 'Platform' / 'NintendoSwitch.cmake'
        old = '"-L${NX_ROOT}/lib -L${DEVKITPRO}/portlibs/switch/lib -fPIE -specs=${NX_ROOT}/switch.specs"'
        text = p.read_text(encoding='utf-8')
        if old in text:
            p.write_text(text.replace(old, '"\\"-L${NX_ROOT}/lib\\" \\"-L${DEVKITPRO}/portlibs/switch/lib\\" '
                                           '-fPIE \\"-specs=${NX_ROOT}/switch.specs\\""'), encoding='utf-8')
        # Package configs, pkg-config and libtool files name the image's install prefix.
        here = fwd(self.dkp)
        for f in self.dkp.rglob('*'):
            if f.is_file() and (f.suffix in ('.cmake', '.pc', '.la') or f.name.endswith('-config')):
                data = f.read_bytes()
                if b'/opt/devkitpro' in data:
                    f.write_bytes(data.replace(b'/opt/devkitpro', here.encode('utf-8')))

    def install_shims(self):
        if not self.shim or not self.shim.is_file():
            raise Failure('nxshim.exe not found (--shim)')
        targets = [self.dkp / 'devkitA64/bin/aarch64-none-elf-g++.exe',
                   self.dkp / 'tools/bin/elf2nro.exe', self.dkp / 'tools/bin/nacptool.exe',
                   self.dkp / 'tools/bin/bin2s.exe', self.dkp / 'tools/bin/uam.exe',
                   self.dkp / 'portlibs/switch/bin/aarch64-none-elf-pkg-config.exe']
        for t in targets:
            t.parent.mkdir(parents=True, exist_ok=True)
            if not t.exists() or t.stat().st_size != self.shim.stat().st_size or sha256_file(t) != sha256_file(self.shim):
                shutil.copyfile(self.shim, t)


# -------------------------------------------------------------------------- process ---

class Runner:
    def __init__(self, tools, ws):
        self.t = tools
        self.ws = Path(ws)
        env = dict(os.environ)
        sysroot = os.environ.get('SystemRoot', r'C:\Windows')
        env['PATH'] = os.pathsep.join(str(p) for p in (
            tools.llvm / 'bin', tools.cmake.parent, tools.ninja.parent, tools.dotnet.parent, tools.python.parent,
            tools.nodtool.parent, Path(sysroot) / 'System32', Path(sysroot)))
        env.update({
            'DEVKITPRO': fwd(tools.dkp), 'DOTNET_ROOT': str(tools.dotnet.parent),
            'DOTNET_CLI_TELEMETRY_OPTOUT': '1', 'DOTNET_NOLOGO': '1', 'DOTNET_ROLL_FORWARD': 'Major',
            'PYTHONUTF8': '1', 'PYTHONIOENCODING': 'utf-8', 'PYTHONDONTWRITEBYTECODE': '1',
            'KPNX_PYTHON': str(tools.python), 'KPNX_NXTOOLS': str(HERE / 'nxtools.py'),
            'KPNX_DEVKITPRO': str(tools.dkp), 'KPNX_LLVM': str(tools.llvm),
            'KPNX_DKSH': str(self.ws / 'aio-prebuilt' / 'dksh'),
        })
        for k in ('PYTHONHOME', 'PYTHONPATH', 'CC', 'CXX', 'CFLAGS', 'CXXFLAGS', 'LDFLAGS', 'CMAKE_GENERATOR'):
            env.pop(k, None)
        self.env = env

    def run(self, cmd, cwd=None, progress=None, quiet=False):
        cmd = [str(c) for c in cmd]
        log('> ' + ' '.join(cmd if len(' '.join(cmd)) < 600 else cmd[:6] + ['...']))
        p = subprocess.Popen(cmd, cwd=str(cwd or self.ws), env=self.env, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, bufsize=1, text=True, encoding='utf-8', errors='replace',
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        tail = []
        for line in p.stdout:
            line = line.rstrip('\r\n')
            tail.append(line)
            del tail[:-60]
            if progress:
                progress(line)
            with OUT_LOCK:
                if not quiet or not progress:
                    print(line, flush=True)
                if LOG:
                    LOG.write(line + '\n')
        rc = p.wait()
        if rc:
            raise Failure(f'{Path(cmd[0]).name} failed (exit {rc}):\n' + '\n'.join(tail[-25:]))

    def python(self, script, *args, **kw):
        self.run([self.t.python, self.ws / 'builder' / script, *args], **kw)

    def translator(self, *args, **kw):
        dll = self.ws / 'build/wiicompiled-fpscr/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll'
        self.run([self.t.dotnet, dll, *args], **kw)


# ---------------------------------------------------------------------------- stages ---

def find_toolkit(arg, exe_dir, manifest, data, bundle):
    """(key, extract-into-function). A toolkit zip given or placed next to the exe wins over
    the one built into the exe, so a new toolkit can be tried without rebuilding the exe."""
    def from_zip(path):
        return sha256_file(path), lambda ws: extract_zip(path, ws, label='toolkit')
    if arg and Path(arg).is_file():
        return from_zip(Path(arg))
    if exe_dir:
        found = sorted(Path(exe_dir).glob('KartPadNX-Toolkit*.zip'))
        if found:
            return from_zip(found[-1])
    if bundle and bundle.has_toolkit():
        return bundle.info['toolkit_sha256'], bundle.extract_toolkit
    spec = manifest['toolkit']
    url = arg if arg and arg.startswith(('http://', 'https://')) else spec.get('url')
    if not url:
        raise Failure('No toolkit: this builder has none built in. Put KartPadNX-Toolkit.zip next to it.')
    return from_zip(download(url, Path(data) / 'downloads' / 'KartPadNX-Toolkit.zip', spec.get('size') or None,
                             spec.get('sha256') or None, label='KartPad-NX toolkit'))


def stage_toolkit(state, data, key, extract):
    ws = Path(data) / 'ws'
    if state.done('toolkit', key) and (ws / 'aio-toolkit.json').is_file():
        return ws
    stage('toolkit', 'Unpacking the KartPad-NX toolkit')
    if ws.exists():
        shutil.rmtree(ws)
    for s in ('disc', 'translate-base', 'translate-retro-rewind', 'build-base', 'build-retro-rewind'):
        state.clear(s)
    extract(ws)
    if not (ws / 'aio-toolkit.json').is_file():
        raise Failure('the toolkit is not a KartPad-NX toolkit (no aio-toolkit.json)')
    state.mark('toolkit', key)
    return ws


def stage_disc(state, run, disc):
    disc = Path(disc)
    if not disc.is_file():
        raise Failure(f'disc image not found: {disc}')
    out = run.ws / 'private/self-build/disc'
    key = f'{disc.resolve()}:{disc.stat().st_size}:{int(disc.stat().st_mtime)}'
    if state.done('disc', key) and (out / 'sys/main.dol').is_file() and (out / 'sys/fst.bin').is_file():
        return
    stage('disc', 'Reading your disc image')
    if out.exists():
        shutil.rmtree(out)
    run.run([run.t.nodtool, 'extract', '--quiet', disc, out])
    dol, rel = out / 'sys/main.dol', out / 'files/rel/StaticR.rel'
    if not dol.is_file() or not rel.is_file():
        raise Failure('The image does not look like a Wii disc (no main.dol / StaticR.rel).')
    if sha256_file(dol) != EXPECT_DOL or sha256_file(rel) != EXPECT_REL:
        raise Failure('This is not the supported game version. KartPad-NX needs Mario Kart Wii '
                      'PAL (RMCP01), revision 0.')
    # main.dol and StaticR.rel feed the translation; sys/ and files/ also go to the SD card.
    log('disc OK: Mario Kart Wii PAL rev0')
    state.mark('disc', key)


def ninja_progress(prefix):
    pat = re.compile(r'^\[(\d+)/(\d+)\]')
    last = [0.0]

    def cb(line):
        m = pat.match(line)
        if m and time.time() - last[0] > 0.3:
            last[0] = time.time()
            emit(progress='compile', item=prefix, done=int(m.group(1)), total=int(m.group(2)))
    return cb


def translate_base(run, jobs):
    ws = run.ws
    tr = ws / 'private/self-build/translation'
    proj = ws / 'build/mkwii-rmcp01-base.yml'
    shutil.copyfile(ws / 'ref/kartpad-main/tools/mkwii-rmcp01-base.yml', proj)
    run.translator('translate-recursive', ENTRY, '--project', proj, '--threads', min(jobs, 8),
                   '--prune-stale', '--output-metadata', tr / 'base_translation_output.json')
    run.python('translator-post-hooks.py', 'functions', tr / 'functions')
    run.translator('generate-data-init', '--project', proj)
    run.translator('emit-build-shards', '--project', proj, '--base-metadata', tr / 'base_translation_output.json',
                   '--base-functions-dir', tr / 'functions', '--native-source-dir',
                   ws / 'build/wiicompiled-fpscr/runtime/src', '--out', tr / 'build_shards')
    run.python('translator-post-hooks.py', 'shards', tr / 'build_shards')
    text = (tr / 'build_shards/shards.cmake').read_text(encoding='utf-8')
    if f'set(MKW_BASE_FUNCTION_COUNT {BASE_FUNCTION_COUNT})' not in text:
        raise Failure('base translation produced an unexpected function count')


RR_FEED = 'https://update.rwfc.net/RetroRewind/RetroRewindVersion.txt'
RR_CDN = 'cdn.update.rwfc.net'
SHA256_HEX = re.compile(r'^[0-9a-f]{64}$')


def _version_key(v):
    return tuple(int(x) if x.isdigit() else 0 for x in re.split(r'[.\-]', v))


def _full_pack_problem(entry):
    """Why an entry's full pack (full_url/full_size/full_sha256) cannot be used, or None."""
    try:
        url = urllib.parse.urlparse(entry['full_url'])
        if url.scheme != 'https' or url.netloc != RR_CDN:
            return f'{entry["full_url"]} is not on {RR_CDN}'
        if not (SHA256_HEX.match(entry['full_sha256']) and isinstance(entry['full_size'], int)
                and 0 < entry['full_size'] < (8 << 30)):
            return 'incomplete entry'
    except (KeyError, TypeError) as e:
        return f'incomplete entry ({e})'
    return None


def resolve_rr(manifest, override=None):
    """The Retro Rewind version to build: the newest entry of the approved list (hosted by the
    maintainer, aio/manifest/rr-versions.json, URL in tools.json retro_rewind.approved_url), or
    the version built into this exe when there is no list or it cannot be read. An entry may
    only point at Retro Rewind's own CDN, and every file it names is hash-checked, so the list
    chooses a version but cannot make the builder fetch anything else."""
    builtin = manifest['retro_rewind']
    source = override or builtin.get('approved_url')
    spec, origin = builtin, 'built in'
    if source:
        try:
            if source.startswith(('http://', 'https://')):
                with _request(source) as r:
                    raw = r.read(1 << 20)
            else:
                raw = Path(source).read_bytes()
            data = json.loads(raw.decode('utf-8-sig'))  # tolerate a BOM from Notepad
            version = data['latest']
            entry = data['versions'][version]
            url = urllib.parse.urlparse(entry['url'])
            if url.scheme != 'https' or url.netloc != RR_CDN:
                raise ValueError(f'{entry["url"]} is not on {RR_CDN}')
            if not (SHA256_HEX.match(entry['sha256']) and SHA256_HEX.match(entry['code_pul_sha256'])
                    and isinstance(entry['size'], int) and 0 < entry['size'] < (4 << 30)):
                raise ValueError('incomplete entry')
            spec = {**{k: v for k, v in builtin.items() if not k.startswith('full_')}, 'version': version,
                    'url': entry['url'], 'size': entry['size'], 'sha256': entry['sha256'],
                    'code_pul_sha256': entry['code_pul_sha256']}
            origin = 'approved list'
            if 'full_url' in entry:
                problem = _full_pack_problem(entry)
                if problem:
                    log(f'Retro Rewind {version}: ignoring the full pack in the approved list ({problem})')
                else:
                    spec.update({k: entry[k] for k in ('full_url', 'full_size', 'full_sha256')})
        except Exception as e:  # noqa: BLE001 - any problem with the list falls back
            log(f'Retro Rewind: approved version list unavailable ({e}); using the built-in {builtin["version"]}')
    log(f'Retro Rewind: building version {spec["version"]} ({origin})')
    try:
        with _request(RR_FEED) as r:
            newest = r.read().decode('utf-8', 'replace').split()[-4]
        if _version_key(newest) > _version_key(spec['version']):
            log(f'Retro Rewind {newest} is out but not approved for KartPad-NX yet; keep {spec["version"]} '
                f'on the SD card. Once it is approved, click Build again to update.')
    except Exception:
        pass
    return spec


def rr_inputs(run, spec):
    """Code.pul from the chosen Retro Rewind update, and the live Retro-WFC payload checked
    against the production signing key (the same checks as builder/translate-profile.py)."""
    work = run.ws / 'private/self-build/retro-rewind'
    zpath = download(spec['url'], run.t.dl / Path(spec['url']).name, spec['size'], spec['sha256'],
                     label=f'Retro Rewind {spec["version"]} update')
    pack = work / 'pack' / 'RetroRewind6'   # the overlay root name the runtime registers
    if pack.exists():
        shutil.rmtree(pack.parent)
    with zipfile.ZipFile(zpath) as z:
        for name in ('RetroRewind6/Binaries/Code.pul', 'RetroRewind6/version.txt'):
            if name not in z.namelist():
                if name.endswith('Code.pul'):
                    raise Failure(f'the Retro Rewind {spec["version"]} update has no Code.pul')
                continue
            out = pack / name.split('/', 1)[1]
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(z.read(name))
    code = pack / 'Binaries/Code.pul'
    if sha256_file(code) != spec['code_pul_sha256']:
        raise Failure('Retro Rewind Code.pul does not match the pinned version')
    payload = work / 'payload.RMCPD00.bin'
    download(spec['payload_url'], payload, label='Retro-WFC payload')
    sys.path.insert(0, str(run.ws / 'ref/kartpad-main/builder'))
    from kartpad_builder.retro_rewind import validate_rwfc_payload
    prov = {'url': spec['payload_url'], 'bytes': payload.stat().st_size, 'sha256': sha256_file(payload)}
    try:
        validate_rwfc_payload(payload, prov)
    except Exception as e:
        raise Failure(f'Retro-WFC payload check failed: {e}')
    (work / 'payload-provenance.json').write_text(json.dumps(prov, indent=2), encoding='utf-8')
    return pack, code, payload


def translate_rr(run, jobs, spec):
    ws = run.ws
    root, code, payload = rr_inputs(run, spec)
    work = ws / 'private/self-build/retro-rewind'
    out = work / 'translation'
    functions, metadata, base_dir, mod, shards = (out / 'functions', out / 'base_translation_output.json',
                                                  out / 'base', work / 'mod', out / 'build_shards')
    base_dir.mkdir(parents=True, exist_ok=True)
    staged = work / 'input/Binaries/Code.pul'
    staged.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(code, staged)
    proj = ws / 'build/mkwii-rmcp01-retro-rewind.yml'
    shutil.copyfile(ws / 'ref/kartpad-main/tools/mkwii-rmcp01-retro-rewind.yml', proj)
    common = ['--project', proj, '--profile', 'retro-rewind']
    j = min(jobs, 8)
    run.translator('translate-recursive', ENTRY, *common, '--threads', j, '--prune-stale', '--output-metadata', metadata)
    run.python('translator-post-hooks.py', 'functions', functions)
    run.translator('emit-base-manifest', *common, '--out', base_dir, '--functions-dir', functions,
                   '--translation-output-metadata', metadata, '--region', 'P')
    run.translator('translate-mod', *common, '--base-manifest', base_dir / 'mkwii_base_manifest.json',
                   '--base-translation-output-metadata', metadata, '--code-pul', code, '--mod-root', root,
                   '--mod-name', 'retro-rewind', '--region', 'P', '--out', mod, '--prefer-cached-inputs',
                   '--emit-cpp', '--threads', j, '--retro-wfc-payload', payload)
    run.translator('generate-data-init', *common)
    run.translator('emit-build-shards', *common, '--base-metadata', metadata, '--base-functions-dir', functions,
                   '--native-source-dir', ws / 'build/wiicompiled-fpscr/runtime/src',
                   '--resolved-profile', mod / 'resolved_dispatch_profile.json', '--retro-cpp-dir', mod / 'cpp',
                   '--out', shards)
    run.python('translator-post-hooks.py', 'shards', shards)
    if 'set(MKW_HAVE_RETRO_REWIND_SHARDS ON)' not in (shards / 'shards.cmake').read_text(encoding='utf-8'):
        raise Failure('Retro Rewind translation is missing the mod graph')


def transform(run, profile):
    p = PROFILES[profile]
    tr = run.ws / p['translation']
    if p['cpu_rewrites']:
        run.python('optimize-translated-compares.py', tr, '--apply')
        ex = tr / 'nonvolatile_reload_exemptions.json'
        if ex.is_file():
            run.python('elide-redundant-flushes.py', tr / 'build_shards', '--exemptions', ex)
    for s in TRANSFORM_PASSES:
        run.python(s + '.py', tr / 'build_shards')


def configure_seed(run, bdir):
    """What aio/toolkit/make-prebuilt.py recorded from a configure with the same pinned tools:
    the results of the compiler checks (zstd's and fmt's flag probes, FindThreads; each is a
    try-compile of ~5 s on Windows) as an initial cache, and CMake's compiler identification
    files for a new build tree. Returns the extra CMake arguments."""
    seed = run.ws / 'aio-prebuilt/cmake'
    if not (seed / 'seed.json').is_file():
        return []
    meta = json.loads((seed / 'seed.json').read_text(encoding='utf-8'))
    if {n: run.t.key(n) for n in meta['tools']} != meta['tools']:
        log('configure seed: recorded with other tool versions; CMake runs every check')
        return []
    if not (bdir / 'CMakeCache.txt').is_file():
        dest = bdir / 'CMakeFiles' / meta['cmake_files']
        dest.mkdir(parents=True, exist_ok=True)
        for name in meta['compiler_files']:
            text = (seed / name).read_text(encoding='utf-8').replace('@TOOLS@', fwd(run.t.root))
            (dest / name).write_text(text, encoding='utf-8')
    return ['-C', seed / 'checks.cmake']


def cmake_configure_cmd(run, profile, bdir, tr):
    """The game's CMake configure command; aio/toolkit/make-prebuilt.py uses it too, so the
    prebuilt objects are compiled with exactly these settings."""
    p = PROFILES[profile]
    ws, t = run.ws, run.t
    pgo_args = []
    marker = ws / 'private/pgo' / p['pgo']
    if not marker.is_file():
        marker = ws / 'private/pgo/current'
    if marker.is_file() and marker.read_text().strip():
        name = marker.read_text().strip()
        pgo_args = [f'-DKARTPAD_PGO_PROFILE={fwd(ws / "private/pgo" / (name + ".extbin"))}',
                    f'-DKARTPAD_SECTION_ORDER={fwd(ws / "private/pgo" / (name + ".order.ld"))}',
                    '-DKARTPAD_PGO_ALL=ON']
    dkp = fwd(t.dkp)
    llvm = t.llvm / 'bin'
    # Carry the hardware-tested v9 vanilla configuration into ordinary AIO
    # builds. Keep RR's existing renderer policy, and explicitly reset cached
    # CMake options when an existing build directory is reused.
    vanilla_gpu = 'ON' if profile == 'base' else 'OFF'
    cmd = [t.cmake, '-S', ws / 'switch', '-B', bdir, '-G', 'Ninja',
           f'-DCMAKE_MAKE_PROGRAM={fwd(t.ninja)}',
           f'-DCMAKE_TOOLCHAIN_FILE={fwd(ws / "switch/cmake/SwitchClang.cmake")}',
           '-DCMAKE_BUILD_TYPE=Release',
           f'-DCMAKE_EXE_LINKER_FLAGS="-L{dkp}/libnx/lib" "-L{dkp}/portlibs/switch/lib" -fPIE "-specs={dkp}/libnx/switch.specs"',
           f'-DPython3_EXECUTABLE={fwd(t.python)}',
           f'-DCMAKE_AR={fwd(llvm / "llvm-ar.exe")}', f'-DCMAKE_RANLIB={fwd(llvm / "llvm-ranlib.exe")}',
           f'-DCMAKE_LINKER={fwd(llvm / "ld.lld.exe")}', f'-DCMAKE_STRIP={fwd(llvm / "llvm-strip.exe")}',
           f'-DDKP_NM={fwd(llvm / "llvm-nm.exe")}', f'-DDKP_OBJCOPY={fwd(llvm / "llvm-objcopy.exe")}',
           f'-DKARTPAD_PROFILE={profile}', '-DKARTPAD_BUILD_GRAPHICS=ON', '-DKARTPAD_ENABLE_FULL_APP=ON',
           '-DKARTPAD_DEKO_DEBUG=OFF', '-DKARTPAD_DEKO_GPU_CHECKPOINTS=OFF',
           f'-DKARTPAD_DEKO_GPU_FLUSHES={vanilla_gpu}',
           f'-DKARTPAD_DEKO_GPU_BREADCRUMBS={vanilla_gpu}', '-DKARTPAD_DEKO_GPU_FLUSH_INTERVAL=32',
           f'-DKARTPAD_SHARDS={fwd(tr / "build_shards/shards.cmake")}',
           f'-DKARTPAD_TRANSLATION_DIR={fwd(tr)}',
           f'-DKARTPAD_RUNTIME_SRC={fwd(ws / "build/wiicompiled-fpscr/runtime")}',
           f'-DKARTPAD_DAWN_SOURCE={fwd(ws / "ref/dawn")}', f'-DKARTPAD_DAWN_BUILD={fwd(ws / "build/dawn-switch-deko")}',
           f'-DKARTPAD_SDL_SOURCE={fwd(ws / "ref/SDL")}', f'-DKARTPAD_SDL_BUILD={fwd(ws / "build/sdl-switch-vulkan")}',
           f'-DKARTPAD_AURORA_SOURCE={fwd(ws / "build/wiicompiled-fpscr/aurora-main")}',
           *pgo_args,
           '-DKARTPAD_OUTLINE_SCALAR_FP=ON', '-DKARTPAD_THINLTO_SHARDS=OFF', '-DKARTPAD_THINLTO_JOBS=1',
           f'-DKARTPAD_APP_NAME={p["app"]}', f'-DKARTPAD_NRO_NAME={p["nro"]}',
           '-DFETCHCONTENT_FULLY_DISCONNECTED=ON', '-DFETCHCONTENT_QUIET=ON']
    for d in FETCH_DEPS:
        cmd.append(f'-DFETCHCONTENT_SOURCE_DIR_{d.upper()}={fwd(ws / "aio-prebuilt/deps" / (d + "-src"))}')
    return cmd


def configure_and_build(run, profile, jobs):
    p = PROFILES[profile]
    ws, t = run.ws, run.t
    bdir = ws / p['build']
    llvm = t.llvm / 'bin'
    run.python('integrate-deko.py')
    cmd = cmake_configure_cmd(run, profile, bdir, ws / p['translation'])
    cmd += configure_seed(run, bdir)
    # Game-independent libraries compiled once by make-prebuilt.py; only the translated game
    # is compiled here (switch/cmake/Prebuilt.cmake).
    prebuilt = ws / 'aio-prebuilt/libs' / profile
    if (prebuilt / 'prebuilt.cmake').is_file():
        cmd.append(f'-DKARTPAD_PREBUILT_LIBS_DIR={fwd(prebuilt)}')
    run.run(cmd)
    run.run([t.cmake, '--build', bdir, '--target', 'kartpad_full_nro', '--parallel', jobs],
            progress=ninja_progress(p['app']), quiet=True)
    elf = bdir / 'kartpad_full.elf'
    nro = bdir / p['nro']
    out = subprocess.run([str(llvm / 'llvm-nm.exe'), str(elf)], capture_output=True, text=True).stdout
    start = next((l.split()[0] for l in out.splitlines() if l.split()[-1:] == ['_start']), None)
    if start != '0000000000000000':
        nro.unlink(missing_ok=True)
        raise Failure(f'image check failed: _start at {start}, not 0')
    log(f'built {nro.name}: {nro.stat().st_size:,} bytes, sha256 {sha256_file(nro)}')
    return nro


def pack_launcher(run, nros, out_dir):
    """builder/pack-payloads.py + the launcher's nx_create_nro, without recompiling it."""
    from compression import zstd
    sys.path.insert(0, str(HERE))
    import nxtools
    ws = run.ws
    stage_dir = ws / 'build/launcher/romfs'
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    shutil.copytree(ws / 'launcher/romfs', stage_dir)
    (stage_dir / 'payload').mkdir()
    entries = []
    for profile, nro in nros.items():
        p = PROFILES[profile]
        raw = nro.read_bytes()
        emit(progress='pack', item=p['app'], done=0, total=0)
        # pack-payloads.py: level 19 with long-distance matching.
        opts = {zstd.CompressionParameter.compression_level: 19,
                zstd.CompressionParameter.enable_long_distance_matching: 1}
        try:
            comp = zstd.compress(raw, options={**opts, zstd.CompressionParameter.nb_workers: min(os.cpu_count() or 1, 8)})
        except zstd.ZstdError:
            comp = zstd.compress(raw, options=opts)  # libzstd built without threads
        (stage_dir / 'payload' / p['payload']).write_bytes(comp)
        stamp = re.search(rb'KARTPAD-BUILD [0-9]{4}-[0-9TZ:-]+', raw)
        entries.append({'variant': p['variant'], 'file': p['payload'], 'raw_size': len(raw),
                        'raw_sha256': hashlib.sha256(raw).hexdigest(), 'zst_size': len(comp),
                        'build_stamp': stamp.group(0).decode() if stamp else 'KARTPAD-BUILD unknown'})
        log(f'packed {p["payload"]}: {len(raw):,} -> {len(comp):,} bytes')
    (stage_dir / 'payload/payloads.json').write_text(json.dumps({'schema': 1, 'payloads': entries}, indent=2),
                                                    encoding='utf-8')
    dest = Path(out_dir) / 'switch' / 'mkw-launcher.nro'
    dest.parent.mkdir(parents=True, exist_ok=True)
    nxtools.elf2nro(ws / 'build/launcher/mkw_launcher.elf', dest, icon=ws / 'launcher/assets/icon.jpg',
                    nacp=ws / 'build/launcher/mkw-launcher.nacp', romfs_dir=stage_dir)
    return dest


INSTALL_TEXT = """KartPadNX, built {when} from your own disc

Copy everything in this folder to the root of your Switch SD card. If Windows asks, replace
the files that are already there.

    switch/mkw-launcher.nro                        the launcher, with {games}
    switch/kartpad/data/                           your game files
{rr_line}
Then open the Homebrew Menu (hold R while starting any game) and start MKW Launcher.
There's nothing to import.
{rr_text}
These files are made from your own copy of the game. They're for you only: please don't
share them.

mkw-launcher.nro sha256: {sha}
"""

RR_LINE = "    switch/kartpad/retro-rewind/RetroRewind6/      Retro Rewind {rr}\n"

RR_TEXT_INCLUDED = """
When Retro Rewind updates, run the builder again and copy this folder over again. The game
and the Retro Rewind files always come out matching.
"""

RR_TEXT_MISSING = """
Retro Rewind {rr}'s own files couldn't be included: no full pack is approved for this version
yet. Download the full {rr} pack from rwfc.net and copy its RetroRewind6 folder (the one with
xml/RetroRewind6.xml inside) to switch/kartpad/retro-rewind/RetroRewind6 on the SD card.
"""

# ------------------------------------------------------------------- SD game files ---
# The game reads the extracted disc from sdmc:/switch/kartpad/data and starts only when
# .kartpad-extraction-complete is there (switch/src/runtime_host_switch.cpp). The builder
# writes the same tree and manifest the console's extractor would
# (switch/src/runtime/disc_extractor.cpp), so nothing has to be imported on the Switch.

MANIFEST_MAGIC = 'KARTPAD-DVD-EXTRACT-V1'
FNV_OFFSET, FNV_PRIME, MASK64 = 14695981039346656037, 1099511628211, (1 << 64) - 1


def fnv1a(data, h=FNV_OFFSET):
    for x in data:
        h = ((h ^ x) * FNV_PRIME) & MASK64
    return h


def _low_priority():
    try:
        ctypes.windll.kernel32.SetPriorityClass(ctypes.windll.kernel32.GetCurrentProcess(), 0x4000)
    except Exception:
        pass


def _fnv_file(path):
    h = FNV_OFFSET
    with open(path, 'rb') as f:
        while chunk := f.read(1 << 20):
            h = fnv1a(chunk, h)
    return h


def fst_files(fst):
    """(path, size) of every file in a Wii FST, in FST order: the order the extractor writes
    its manifest lines."""
    count = struct.unpack_from('>I', fst, 8)[0]
    names = 12 * count
    stack, out = [(count, '')], []
    for i in range(1, count):
        while i >= stack[-1][0]:
            stack.pop()
        word, _, value = struct.unpack_from('>III', fst, 12 * i)
        start = names + (word & 0xFFFFFF)
        name = fst[start:fst.index(b'\0', start)].decode('ascii')
        path = f'{stack[-1][1]}/{name}' if stack[-1][1] else name
        if word >> 24:
            stack.append((value, path))
        else:
            out.append((path, value))
    return out


def sd_game_files(state, run, out_dir, disc_key):
    """<out>/switch/kartpad/data: sys/ and files/ from the extracted disc plus the completion
    manifest. Runs in the background during translation and compiling; the per-file FNV
    hashes (about 12 CPU-minutes in Python) are spread over low-priority processes."""
    src = run.ws / 'private/self-build/disc'
    live = Path(out_dir) / 'switch/kartpad/data'
    key = f'{live.resolve()}|{disc_key}'
    if state.done('sd-game', key) and (live / '.kartpad-extraction-complete').is_file():
        return
    state.clear('sd-game')
    fst = (src / 'sys/fst.bin').read_bytes()
    files = fst_files(fst)
    for path, size in files:
        if (src / 'files' / path).stat().st_size != size:
            raise Failure(f'extracted file {path} does not match its size in the disc table')
    workers = max(1, min(4, (os.cpu_count() or 2) // 3))
    with concurrent.futures.ProcessPoolExecutor(workers, initializer=_low_priority) as pool:
        hashes = pool.map(_fnv_file, [str(src / 'files' / p) for p, _ in files], chunksize=8)
        part = live.with_name('data.part')
        if part.exists():
            shutil.rmtree(part)
        for sub in ('sys', 'files'):
            shutil.copytree(src / sub, part / sub)
        lines = [MANIFEST_MAGIC, 'fst %x' % fnv1a(fst)]
        lines += ['file %d %x' % (size, h) for (_, size), h in zip(files, hashes)]
    (part / '.kartpad-extraction-complete').write_text('\n'.join(lines) + '\n', encoding='ascii', newline='\n')
    if live.exists():
        old = live.with_name('data.old')
        if old.exists():
            shutil.rmtree(old)
        os.replace(live, old)
        shutil.rmtree(old)
    os.replace(part, live)
    log(f'SD card: game files written to {live} ({len(files)} files)')
    state.mark('sd-game', key)


def stage_rr_sd(state, tools, spec, out_dir):
    """<out>/switch/kartpad/retro-rewind/RetroRewind6, where the launcher and the game read it,
    from the full Retro Rewind pack of the version that was built (the update zips carry only
    the files that changed). The zip stays in the work folder's downloads. Returns False when
    the approved entry names no full pack."""
    version = spec['version']
    if not spec.get('full_url'):
        log(f'Retro Rewind {version}: the approved list has no full pack for this version, so the '
            'RetroRewind6 folder is not included; INSTALL.txt says where to get it.')
        return False
    root = Path(out_dir) / 'switch/kartpad/retro-rewind'
    dest = root / 'RetroRewind6'
    xml, code = dest / 'xml/RetroRewind6.xml', dest / 'Binaries/Code.pul'
    key = f'{dest.resolve()}|{spec["full_sha256"]}'
    if state.done('rr-sd', key) and xml.is_file() and code.is_file() \
            and sha256_file(code) == spec['code_pul_sha256']:
        return True
    stage('rr-sd', f'Getting the Retro Rewind {version} SD files')
    state.clear('rr-sd')
    name = Path(urllib.parse.urlparse(spec['full_url']).path).name
    for old in tools.dl.glob('*-full.zip'):
        if old.name != name:
            old.unlink()   # the pack of a version no longer built
    zpath = download(spec['full_url'], tools.dl / name, spec['full_size'], spec['full_sha256'],
                     label=f'Retro Rewind {version} full pack')
    emit(progress='extract', item='RetroRewind6', done=0, total=0)
    # Unpacked beside the old folder and swapped in, so files a newer version dropped do not
    # linger. Saves live in retro-rewind/riivolution, outside RetroRewind6, and are untouched.
    part = root / 'RetroRewind6.part'
    if part.exists():
        shutil.rmtree(part)
    count = extract_zip(zpath, part, include=['RetroRewind6/'], label='RetroRewind6')
    new = part / 'RetroRewind6'
    if not (new / 'xml/RetroRewind6.xml').is_file() or not (new / 'Binaries/Code.pul').is_file():
        raise Failure(f'the Retro Rewind {version} full pack has no RetroRewind6/xml/RetroRewind6.xml or Code.pul')
    if sha256_file(new / 'Binaries/Code.pul') != spec['code_pul_sha256']:
        raise Failure(f'the Retro Rewind {version} full pack has a different Code.pul than the one built')
    if dest.exists():
        old = root / 'RetroRewind6.old'
        if old.exists():
            shutil.rmtree(old)
        os.replace(dest, old)
        shutil.rmtree(old)
    os.replace(new, dest)
    shutil.rmtree(part)
    log(f'RetroRewind6: {count} files from {name} -> {dest}')
    state.mark('rr-sd', key)
    return True


def main():
    global LOG
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--data', required=True, type=Path)
    ap.add_argument('--disc', required=True, type=Path)
    ap.add_argument('--out', required=True, type=Path)
    ap.add_argument('--rr', action='store_true',
                    help="also build Retro Rewind, and put its matching RetroRewind6 folder in --out")
    ap.add_argument('--rr-sd', action='store_true', help=argparse.SUPPRESS)  # older frontends; implied by --rr
    ap.add_argument('--jobs', type=int, default=0)
    ap.add_argument('--toolkit', default=None)
    ap.add_argument('--exe-dir', default=None)
    ap.add_argument('--bundle', default=None, help='KartPadNX-Builder.data, or the exe with the bundle appended')
    ap.add_argument('--rr-list', default=None,
                    help='approved Retro Rewind version list (file or URL) instead of tools.json approved_url')
    ap.add_argument('--manifest', type=Path, default=next(
        (p for p in (HERE / 'tools.json', HERE.parent / 'manifest/tools.json') if p.is_file()), HERE / 'tools.json'))
    ap.add_argument('--shim', type=Path, default=None)
    ap.add_argument('--keep-downloads', action='store_true')
    args = ap.parse_args()

    data = args.data.resolve()
    (data / 'logs').mkdir(parents=True, exist_ok=True)
    LOG = open(data / 'logs' / time.strftime('build-%Y%m%d-%H%M%S.log'), 'w', encoding='utf-8')
    jobs = args.jobs or default_jobs()
    profiles = ['base'] + (['retro-rewind'] if args.rr else [])
    log(f'KartPad-NX AIO builder: data={data} out={args.out} jobs={jobs} profiles={",".join(profiles)}')
    t0 = time.time()
    try:
        manifest = json.loads(args.manifest.read_text(encoding='utf-8'))
        state = State(data / 'state')
        tools = Tools(data, manifest, args.shim or (data / 'tools' / 'bin' / 'nxshim.exe'))
        bundle = Bundle(args.bundle) if args.bundle else None
        tools.install_local(state, bundle, args.keep_downloads)
        # devkitPro is only needed to compile: fetch it while the disc is read and translated.
        dkp = Background(lambda: tools.install_devkitpro(state, args.keep_downloads))
        dkp_ready = [False]

        def need_devkitpro():
            if not dkp_ready[0]:
                if dkp.thread.is_alive():
                    stage('devkitpro', 'Waiting for the devkitPro download')
                dkp.join()
                tools.finish()
                dkp_ready[0] = True

        key, extract = find_toolkit(args.toolkit, args.exe_dir, manifest, data, bundle)
        ws = stage_toolkit(state, data, key, extract)
        run = Runner(tools, ws)
        stage_disc(state, run, args.disc)
        disc_key = state.root.joinpath('disc.done').read_text(encoding='utf-8')
        # The SD card's game files are copied and hashed while the game is translated and built.
        sd_game = Background(lambda: sd_game_files(state, run, args.out, disc_key))
        rr_spec = resolve_rr(manifest, args.rr_list) if args.rr else manifest['retro_rewind']
        nros = {}
        for profile in profiles:
            key = state.root.joinpath('disc.done').read_text(encoding='utf-8')
            if profile == 'retro-rewind':
                # A Retro Rewind update changes Code.pul: re-translate and rebuild just RR.
                key += '|rr:' + rr_spec['code_pul_sha256']
            if not state.done('translate-' + profile, key):
                stage('translate-' + profile, f'Translating the game ({PROFILES[profile]["app"]})')
                (translate_base(run, jobs) if profile == 'base' else translate_rr(run, jobs, rr_spec))
                stage('transform-' + profile, f'Applying source passes ({PROFILES[profile]["app"]})')
                transform(run, profile)
                state.mark('translate-' + profile, key)
                state.clear('build-' + profile)
            nro = ws / PROFILES[profile]['build'] / PROFILES[profile]['nro']
            if not state.done('build-' + profile, key) or not nro.is_file():
                need_devkitpro()
                stage('build-' + profile, f'Compiling {PROFILES[profile]["app"]} (this takes a while)')
                nro = configure_and_build(run, profile, jobs)
                state.mark('build-' + profile, key)
            nros[profile] = nro
        need_devkitpro()
        stage('launcher', 'Packing the launcher')
        dest = pack_launcher(run, nros, args.out)
        sha = sha256_file(dest)
        rr = rr_spec['version']
        rr_sd = args.rr and stage_rr_sd(state, tools, rr_spec, args.out)
        if sd_game.thread.is_alive():
            stage('sd-game', 'Finishing the game files for the SD card')
        sd_game.join()
        if not args.rr:
            rr_text = ''
        elif rr_sd:
            rr_text = RR_TEXT_INCLUDED
        else:
            rr_text = RR_TEXT_MISSING.format(rr=rr)
        games = ' and '.join(PROFILES[p]['app'] for p in profiles)
        (Path(args.out) / 'INSTALL.txt').write_text(INSTALL_TEXT.format(
            when=time.strftime('%Y-%m-%d %H:%M'), games=games,
            rr_line=RR_LINE.format(rr=rr) if rr_sd else '', rr_text=rr_text, sha=sha), encoding='utf-8')
        log(f'\nDone in {(time.time() - t0) / 60:.0f} min: SD card files in {Path(args.out).resolve()}')
        emit(done=True, output=str(Path(args.out).resolve()), sha256=sha)
        return 0
    except Failure as e:
        log(f'\nERROR: {e}')
        emit(error=str(e))
        return 1
    except Exception as e:
        import traceback
        log(traceback.format_exc())
        emit(error=f'{type(e).__name__}: {e}')
        return 2


if __name__ == '__main__':
    sys.exit(main())
