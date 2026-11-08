#!/usr/bin/env python3
"""KartPad-NX web builder backend. The site (web/site, on GitHub Pages) uploads a player's disc
image here in chunks; this server checks it, runs the normal AIO pipeline (aio/pipeline/kpnx_aio.py)
on this PC, and hands back that player's whole SD card folder as one zip (launcher, their game
files, the matching RetroRewind6), the same folder the PC builder makes. Standard library only.

  aio\\out\\python\\python.exe web\\server\\build_server.py --work D:\\kpnx-web ^
      --origin https://kawaiibunga.github.io --warm-disc iso\\RMCP01.wbfs

Then expose the port with a tunnel, e.g. `cloudflared tunnel --url http://localhost:8787`, and put
that https URL in web/site/config.js. web/README.md has the whole setup.

- One build runs at a time; uploads can arrive in parallel (MAX_ACTIVE jobs at once).
- Builds share one work folder, and every accepted disc has the same main.dol/StaticR.rel, so after
  the first build a player's build only re-reads their disc, copies its files and packs (minutes,
  not an hour). Each finished job keeps its SD folder (~6.5 GB) until it expires.
- Builds always use the newest GitHub release of the repo (pre-releases count): its
  KartPadNX-Builder.zip (toolkit, prebuilt parts, Python) and the pipeline scripts at its tag.
  A new release is picked up within RELEASE_POLL seconds; a build already running finishes on the
  release it started with. --pipeline builds from this checkout instead (development).
- --warm-disc: rebuild with the owner's disc whenever Retro Rewind's feed or the release changes,
  so players' builds afterwards are cache hits.
- Uploads are deleted as soon as their build ends; results after RESULT_HOURS.
"""
import argparse
import collections
import json
import os
import queue
import re
import secrets
import shutil
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import zipfile
from email.utils import parsedate_to_datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CHUNK = 32 << 20                 # below Cloudflare's 100 MB request limit
MAX_SIZE = 9 << 30               # a dual-layer ISO is ~8 GB
MAX_ACTIVE = 6                   # jobs uploading or waiting
OUTPUT_SIZE = 8 << 30            # one finished SD folder: game files ~4.4 GB + RetroRewind6 ~2 GB
FREE_MARGIN = 30 << 30           # keep this much disk free for the build itself
TICKET_SECONDS = 600             # a download link works once, for 10 minutes
UPLOAD_IDLE_HOURS = 2
RESULT_HOURS = 24
EXTS = ('.iso', '.wbfs', '.rvz')
RR_FEED = 'https://update.rwfc.net/RetroRewind/RetroRewindVersion.txt'
JOB_ID = re.compile(r'^[A-Za-z0-9_-]{16,64}$')
RELEASE_POLL = 600
RELEASE_ASSET = 'KartPadNX-Builder.zip'
# The pipeline is embedded in the release's exe; these are the same files at the release's tag.
RELEASE_SCRIPTS = ('aio/pipeline/kpnx_aio.py', 'aio/pipeline/nxtools.py', 'aio/manifest/tools.json')

ARGS = None
JOBS = {}
TICKETS = {}   # one-time download ticket -> (job id, expiry)
LOCK = threading.Lock()
QUEUE = queue.Queue()
LAST_RR = {'version': None}
BUILDER = {}   # what builds run: tag, date, pipeline (kpnx_aio.py), args
RR_INFO = {'version': None, 'date': None, 'checked': 0}


def fetch(url, accept=None):
    headers = {'User-Agent': 'KartPadNX-web'}
    if accept:
        headers['Accept'] = accept
    return urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=60)


# ----------------------------------------------------------------- builder release ---

def use_release(tag, date, root):
    with LOCK:
        BUILDER.update(tag=tag, date=date, pipeline=root / 'app/kpnx_aio.py',
                       args=['--exe-dir', str(root / 'dist'), '--shim', str(ARGS.shim),
                             '--bundle', str(root / 'dist/KartPadNX-Builder.data')])


def sync_release():
    """Makes BUILDER the newest non-draft release that has the builder zip, downloading it once."""
    with fetch(f'https://api.github.com/repos/{ARGS.repo}/releases?per_page=10', 'application/vnd.github+json') as r:
        releases = json.load(r)
    rel = next(r for r in releases if not r['draft'] and any(a['name'] == RELEASE_ASSET for a in r['assets']))
    tag = rel['tag_name']
    if BUILDER.get('tag') == tag:
        return
    root = ARGS.work / 'releases' / re.sub(r'[^A-Za-z0-9._-]', '_', tag)
    if not (root / 'ready').is_file():
        print(f'[release] downloading {tag}', flush=True)
        part = root.with_name(root.name + '.part')
        shutil.rmtree(part, ignore_errors=True)
        (part / 'app').mkdir(parents=True)
        asset = next(a for a in rel['assets'] if a['name'] == RELEASE_ASSET)
        with fetch(asset['browser_download_url']) as r, open(part / RELEASE_ASSET, 'wb') as f:
            shutil.copyfileobj(r, f, 1 << 20)
        with zipfile.ZipFile(part / RELEASE_ASSET) as z:
            for name in z.namelist():
                if name.startswith('/') or '..' in Path(name).parts:
                    raise RuntimeError(f'unsafe path in {RELEASE_ASSET}: {name}')
            z.extractall(part / 'unpacked')
        exe = next((part / 'unpacked').rglob('KartPadNX-Builder.exe'))
        os.replace(exe.parent, part / 'dist')
        shutil.rmtree(part / 'unpacked', ignore_errors=True)
        (part / RELEASE_ASSET).unlink()
        for path in RELEASE_SCRIPTS:
            url = f'https://raw.githubusercontent.com/{ARGS.repo}/{urllib.parse.quote(tag)}/{path}'
            with fetch(url) as r:
                (part / 'app' / Path(path).name).write_bytes(r.read())
        (part / 'ready').write_text(json.dumps({'tag': tag, 'date': rel['published_at']}))
        shutil.rmtree(root, ignore_errors=True)
        os.replace(part, root)
    use_release(tag, rel['published_at'], root)
    print(f'[release] builds now use {tag} ({rel["published_at"]})', flush=True)
    # Keep this release and the one before it (a running build may still use that one).
    ready = sorted((d for d in (ARGS.work / 'releases').iterdir() if (d / 'ready').is_file()),
                   key=lambda d: d.stat().st_mtime, reverse=True)
    for old in ready[2:]:
        shutil.rmtree(old, ignore_errors=True)


def release_watcher():
    while True:
        time.sleep(RELEASE_POLL)
        try:
            sync_release()
        except Exception as e:  # noqa: BLE001 - keep the current release
            print(f'[release] {e}', flush=True)


def rr_info():
    """The newest Retro Rewind in its feed and the date of its update zip, checked hourly."""
    if time.time() - RR_INFO['checked'] > 3600:
        try:
            with fetch(RR_FEED) as r:
                rows = [line.split() for line in r.read().decode('utf-8', 'replace').splitlines() if line.strip()]
            version, url = rows[-1][0], rows[-1][1]
            req = urllib.request.Request(url, method='HEAD', headers={'User-Agent': 'KartPadNX-web'})
            with urllib.request.urlopen(req, timeout=60) as r:
                modified = r.headers.get('Last-Modified')
            RR_INFO.update(version=version, date=parsedate_to_datetime(modified).isoformat() if modified else None)
        except Exception as e:  # noqa: BLE001 - keep the last answer
            print(f'[rr] {e}', flush=True)
        RR_INFO['checked'] = time.time()
    return {'version': RR_INFO['version'], 'date': RR_INFO['date']}


# ---------------------------------------------------------------------- disc check ---

def disc_header(path):
    """The first 0x80 bytes of the Wii disc inside an ISO, WBFS or RVZ image, or None."""
    with open(path, 'rb') as f:
        head = f.read(0x100)
        if head[:4] == b'WBFS':
            f.seek(1 << head[8])      # hd_sec_sz_s: the first disc's header is in sector 1
            return f.read(0x80)
        if head[:4] == b'RVZ\x01' or head[:4] == b'WIA\x01':
            return head[0x58:0xD8]   # wia_disc_t.dhead, right after the 0x48-byte file header
        return head[:0x80]


def disc_problem(path):
    """Why `path` is not Mario Kart Wii PAL rev 0, or None. The pipeline then checks the exact
    main.dol and StaticR.rel hashes, so this only turns obviously wrong files away early."""
    try:
        h = disc_header(path)
    except OSError as e:
        return f'could not read the file ({e})'
    if not h or len(h) < 0x20 or h[0x18:0x1C] != bytes.fromhex('5D1C9EA3'):
        return 'this is not a Wii disc image (ISO, WBFS or RVZ)'
    if h[:6] != b'RMCP01':
        return f'this is {h[:6].decode("ascii", "replace")}, not Mario Kart Wii PAL (RMCP01)'
    if h[7] != 0:
        return f'this is revision {h[7]}; only revision 0 is supported'
    return None


# ------------------------------------------------------------------------- jobs ---

def new_job(name, size, rr, disc=None):
    job_id = secrets.token_urlsafe(18)
    d = ARGS.work / 'jobs' / job_id
    d.mkdir(parents=True)
    ext = Path(name).suffix.lower()
    job = {'id': job_id, 'token': secrets.token_urlsafe(24), 'size': size, 'rr': rr, 'dir': d,
           'disc': Path(disc) if disc else d / f'upload{ext}', 'internal': disc is not None,
           'state': 'uploading', 'received': 0, 'touched': time.time(), 'stage': '', 'progress': None,
           'error': None, 'log': collections.deque(maxlen=60), 'rr_version': None, 'out': None,
           'chunks': set(), 'nchunks': (size + CHUNK - 1) // CHUNK}
    if not job['internal']:
        with open(job['disc'], 'wb') as f:
            f.truncate(size)   # chunks arrive in any order and land at their offset
    JOBS[job_id] = job
    return job


PRIVATE_TEXT = [
    # Windows and UNC paths, to the end of the line or a quote (folder names can hold spaces).
    (re.compile(r'(?<![A-Za-z])[A-Za-z]:[\\/][^"\'<>|\r\n]*'), '<path>'),
    (re.compile(r'\\\\[^"\'<>|\r\n]+'), '<path>'),
    (re.compile(r'(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?::\d+)?(?![\w.])'), '<ip>'),  # IPv4 (LAN or public)
    (re.compile(r'\b(?:[0-9a-fA-F]{1,4}:){4,7}[0-9a-fA-F]{1,4}\b'), '<ip>'),      # IPv6
]


def scrub(value):
    """Everything the server sends to a browser goes through here: no local paths or IP addresses
    from build logs or error messages reach players."""
    if isinstance(value, str):
        for pattern, repl in PRIVATE_TEXT:
            value = pattern.sub(repl, value)
        return value
    if isinstance(value, dict):
        return {k: scrub(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [scrub(v) for v in value]
    return value


def public(job):
    position = None
    if job['state'] == 'queued':
        position = sum(1 for j in JOBS.values() if j['state'] == 'queued' and j['touched'] <= job['touched'])
    missing = [i for i in range(job['nchunks']) if i not in job['chunks']] if job['state'] == 'uploading' else []
    return {k: job[k] for k in ('id', 'state', 'size', 'received', 'stage', 'progress', 'error', 'rr_version')} | {
        'queue_position': position, 'log': list(job['log'])[-12:], 'missing': missing}


def drop_upload(job):
    if not job['internal']:
        try:
            job['disc'].unlink()
        except FileNotFoundError:
            pass


def run_build(job):
    # The player's own SD card folder: launcher, their disc's game files, and RetroRewind6.
    out = job['dir'] / 'out'
    with LOCK:
        builder = dict(BUILDER)   # a new release mid-build does not change this one
    job['builder'] = builder.get('tag')
    cmd = [ARGS.python, '-u', str(builder['pipeline']), '--data', str(ARGS.work / 'data'), '--disc', str(job['disc']),
           '--out', str(out), *builder['args']] + (['--rr'] if job['rr'] else [])
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                            errors='replace', cwd=REPO)
    for line in proc.stdout:
        line = line.rstrip()
        if not line.startswith('@@'):
            if line:
                job['log'].append(line)
            continue
        try:
            ev = json.loads(line[2:])
        except ValueError:
            continue
        if 'title' in ev:
            job['stage'], job['progress'] = ev['title'], None
        if ev.get('total'):
            job['progress'] = min(1.0, ev.get('done', 0) / ev['total'])
        if ev.get('rr_version'):
            job['rr_version'] = ev['rr_version']
        if ev.get('error'):
            job['error'] = ev['error']
    proc.wait()
    if proc.returncode != 0 or not (out / 'switch/mkw-launcher.nro').is_file():
        raise RuntimeError(job['error'] or f'the build stopped (exit {proc.returncode})')
    job['out'] = out


def worker():
    while True:
        job = QUEUE.get()
        job['state'], job['touched'] = 'building', time.time()
        try:
            problem = disc_problem(job['disc'])
            if problem:
                raise RuntimeError(problem)
            run_build(job)
            job['state'] = 'done'
            if job['rr_version']:
                LAST_RR['version'] = job['rr_version']
        except Exception as e:  # noqa: BLE001 - reported to the player
            job['state'], job['error'] = 'failed', str(e)
        finally:
            job['touched'] = time.time()
            drop_upload(job)


def janitor():
    while True:
        time.sleep(600)
        now = time.time()
        with LOCK:
            for job_id, job in list(JOBS.items()):
                idle = now - job['touched']
                stale = (job['state'] == 'uploading' and idle > UPLOAD_IDLE_HOURS * 3600) or \
                        (job['state'] in ('done', 'failed') and idle > RESULT_HOURS * 3600)
                if stale:
                    drop_upload(job)
                    shutil.rmtree(job['dir'], ignore_errors=True)
                    del JOBS[job_id]


def warmer():
    """Builds with the owner's disc when Retro Rewind's feed or the builder release has changed."""
    stamp = ARGS.work / 'warm-version.txt'
    while True:
        try:
            RR_INFO['checked'] = 0
            newest = f'{rr_info()["version"]}|{BUILDER.get("tag")}'
            built = stamp.read_text().strip() if stamp.is_file() else None
            if rr_info()['version'] and newest != built:
                with LOCK:
                    job = new_job('warm.iso', 0, True, disc=ARGS.warm_disc)
                    job['state'] = 'queued'
                QUEUE.put(job)
                while job['state'] in ('queued', 'building'):
                    time.sleep(30)
                if job['state'] == 'done':
                    stamp.write_text(newest)
                print(f'[warm] {newest}: {job["state"]} {job["error"] or ""}', flush=True)
        except Exception as e:  # noqa: BLE001 - try again next hour
            print(f'[warm] {e}', flush=True)
        time.sleep(3600)


# ------------------------------------------------------------------------- HTTP ---

class Handler(BaseHTTPRequestHandler):
    server_version = 'KartPadNX-web/1'
    sys_version = ''   # no Python version in the Server header

    def cors(self):
        self.send_header('Access-Control-Allow-Origin', ARGS.origin)
        self.send_header('Access-Control-Allow-Headers', 'Content-Type, X-Job-Token')
        self.send_header('Access-Control-Allow-Methods', 'GET, POST, PUT, OPTIONS')
        self.send_header('Access-Control-Expose-Headers', 'Content-Length')

    def reply(self, code, body):
        data = json.dumps(scrub(body)).encode()
        self.send_response(code)
        self.cors()
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def body_json(self):
        n = int(self.headers.get('Content-Length') or 0)
        if n > 4096:
            raise ValueError('request too large')
        return json.loads(self.rfile.read(n) or b'{}')

    def job(self, job_id):
        job = JOBS.get(job_id) if JOB_ID.match(job_id) else None
        if not job or job['internal'] or not secrets.compare_digest(self.headers.get('X-Job-Token', ''), job['token']):
            self.reply(404, {'error': 'no such build'})
            return None
        return job

    def do_OPTIONS(self):
        self.send_response(204)
        self.cors()
        self.end_headers()

    def do_GET(self):
        path = self.path.split('?', 1)[0]
        if path == '/api/status':
            busy = any(j['state'] == 'building' for j in JOBS.values())
            waiting = sum(j['state'] == 'queued' for j in JOBS.values())
            return self.reply(200, {'ok': True, 'busy': busy, 'queued': waiting, 'rr_version': LAST_RR['version'],
                                    'rr_latest': rr_info(),
                                    'builder': {'tag': BUILDER.get('tag'), 'date': BUILDER.get('date')},
                                    'chunk': CHUNK, 'max_size': MAX_SIZE})
        m = re.match(r'^/api/download/([A-Za-z0-9_-]{16,64})$', path)
        if m:
            # A plain link the browser downloads itself (a 6 GB file can't go through fetch/Blob).
            with LOCK:
                job_id, expiry = TICKETS.pop(m.group(1), (None, 0))
                job = JOBS.get(job_id)
            if not job or expiry < time.time() or job['state'] != 'done' or not job['out']:
                return self.reply(404, {'error': 'this download link has expired; press Download again'})
            job['touched'] = time.time()
            return self.send_zip(job['out'])
        m = re.match(r'^/api/jobs/([^/]+)$', path)
        if not m:
            return self.reply(404, {'error': 'not found'})
        job = self.job(m.group(1))
        if job:
            self.reply(200, public(job))

    def send_zip(self, root):
        """Streams `root` as a zip: stored (the game files don't compress), Zip64, built on the fly
        so a finished build costs no second copy on disk."""
        class Stream:   # unseekable: zipfile then writes data descriptors after each file
            def __init__(self, w):
                self.w = w

            def write(self, b):
                self.w.write(b)
                return len(b)

            def flush(self):
                self.w.flush()

        self.send_response(200)
        self.cors()
        self.send_header('Content-Type', 'application/zip')
        self.send_header('Content-Disposition', 'attachment; filename="KartPadNX-SD-card.zip"')
        self.send_header('Connection', 'close')
        self.end_headers()
        with zipfile.ZipFile(Stream(self.wfile), 'w', zipfile.ZIP_STORED, allowZip64=True) as z:
            for file in sorted(p for p in root.rglob('*') if p.is_file()):
                z.write(file, file.relative_to(root).as_posix())

    def do_POST(self):
        path = self.path.split('?', 1)[0]
        try:
            if path == '/api/jobs':
                req = self.body_json()
                name, size = str(req.get('name', '')), req.get('size')
                if not name.lower().endswith(EXTS):
                    return self.reply(400, {'error': 'pick an .iso, .wbfs or .rvz disc image'})
                if not isinstance(size, int) or not 0 < size <= MAX_SIZE:
                    return self.reply(400, {'error': 'that file is too big to be a Wii disc image'})
                with LOCK:
                    active = sum(j['state'] in ('uploading', 'queued') and not j['internal'] for j in JOBS.values())
                    if active >= MAX_ACTIVE:
                        return self.reply(503, {'error': 'the builder is busy; try again in a little while'})
                    if shutil.disk_usage(ARGS.work).free < size + OUTPUT_SIZE + FREE_MARGIN:
                        return self.reply(503, {'error': 'the builder is out of disk space right now'})
                    job = new_job(name, size, bool(req.get('rr', True)))
                return self.reply(201, {'id': job['id'], 'token': job['token'], 'chunk': CHUNK})
            m = re.match(r'^/api/jobs/([^/]+)/(start|ticket)$', path)
            if not m:
                return self.reply(404, {'error': 'not found'})
            job = self.job(m.group(1))
            if not job:
                return
            if m.group(2) == 'ticket':
                if job['state'] != 'done':
                    return self.reply(409, {'error': 'not ready'})
                ticket = secrets.token_urlsafe(24)
                with LOCK:
                    now = time.time()
                    for t in [t for t, (_, exp) in TICKETS.items() if exp < now]:
                        del TICKETS[t]
                    TICKETS[ticket] = (job['id'], now + TICKET_SECONDS)
                return self.reply(200, {'url': f'/api/download/{ticket}'})
            if job['state'] != 'uploading' or len(job['chunks']) != job['nchunks']:
                return self.reply(409, {'error': 'the upload is not complete'})
            problem = disc_problem(job['disc'])
            if problem:
                job['state'], job['error'] = 'failed', problem
                drop_upload(job)
                return self.reply(400, {'error': problem})
            job['state'], job['touched'] = 'queued', time.time()
            QUEUE.put(job)
            return self.reply(200, public(job))
        except (ValueError, TypeError) as e:
            return self.reply(400, {'error': str(e)})

    def do_PUT(self):
        m = re.match(r'^/api/jobs/([^/?]+)/upload\?offset=(\d+)$', self.path)
        if not m:
            return self.reply(404, {'error': 'not found'})
        job = self.job(m.group(1))
        if not job:
            return
        # One CHUNK-sized piece at a CHUNK-aligned offset; the site sends several at once, in any order.
        offset, n = int(m.group(2)), int(self.headers.get('Content-Length') or 0)
        if job['state'] != 'uploading':
            return self.reply(409, {'error': 'this upload is closed'})
        index = offset // CHUNK
        if offset % CHUNK or index >= job['nchunks'] or n != min(CHUNK, job['size'] - offset):
            return self.reply(409, {'error': 'bad piece', 'missing': public(job)['missing']})
        with open(job['disc'], 'r+b') as f:
            f.seek(offset)
            left = n
            while left:
                block = self.rfile.read(min(left, 1 << 20))
                if not block:
                    return self.reply(400, {'error': 'upload interrupted'})
                f.write(block)
                left -= len(block)
        with LOCK:
            job['chunks'].add(index)
            job['received'] = sum(min(CHUNK, job['size'] - i * CHUNK) for i in job['chunks'])
            job['touched'] = time.time()
        return self.reply(200, {'received': job['received']})

    def log_message(self, fmt, *args):
        if ARGS.verbose:
            super().log_message(fmt, *args)


def main(argv=None):
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--work', type=Path, required=True, help='folder for builds, uploads and results (~60 GB)')
    ap.add_argument('--origin', required=True, help='the site origin allowed to call this server, e.g. https://you.github.io')
    ap.add_argument('--port', type=int, default=8787)
    ap.add_argument('--host', default='127.0.0.1', help='keep 127.0.0.1 behind a tunnel')
    ap.add_argument('--python', default=str(REPO / 'aio/out/python/python.exe'))
    ap.add_argument('--repo', default='KawaiiBunga/KartPadNX', help='GitHub repo whose newest release builds use')
    ap.add_argument('--shim', type=Path, default=REPO / 'aio/out/obj/nxshim.exe')
    ap.add_argument('--pipeline', type=Path, help='build from this kpnx_aio.py (and this checkout\'s aio/out) '
                                                  'instead of the newest release')
    ap.add_argument('--warm-disc', type=Path, help="the owner's RMCP01 image; prebuild each new Retro Rewind/release")
    ap.add_argument('--verbose', action='store_true')
    ap.add_argument('pipeline_args', nargs='*', help='with --pipeline: kpnx_aio.py arguments (after --)')
    ARGS = ap.parse_args(argv)
    ARGS.work = ARGS.work.resolve()
    (ARGS.work / 'jobs').mkdir(parents=True, exist_ok=True)
    shutil.rmtree(ARGS.work / 'jobs', ignore_errors=True)   # jobs do not survive a restart
    (ARGS.work / 'jobs').mkdir()
    if ARGS.pipeline:
        aio = REPO / 'aio/out'
        BUILDER.update(tag='local checkout', date=None, pipeline=ARGS.pipeline, args=ARGS.pipeline_args or [
            '--exe-dir', str(aio / 'dist'), '--shim', str(ARGS.shim), '--bundle', str(aio / 'dist/KartPadNX-Builder.data')])
    else:
        if not ARGS.shim.is_file():
            sys.exit(f'{ARGS.shim} is missing: run aio\\release.ps1 once (it builds nxshim.exe)')
        (ARGS.work / 'releases').mkdir(exist_ok=True)
        try:
            sync_release()
        except Exception as e:  # noqa: BLE001 - offline: use the newest release already here
            ready = sorted((d for d in (ARGS.work / 'releases').iterdir() if (d / 'ready').is_file()),
                           key=lambda d: d.stat().st_mtime, reverse=True)
            if not ready:
                sys.exit(f'could not get the newest release ({e}) and none is downloaded yet')
            info = json.loads((ready[0] / 'ready').read_text())
            use_release(info['tag'], info['date'], ready[0])
            print(f'[release] {e}; using {info["tag"]} from disk', flush=True)
        threading.Thread(target=release_watcher, daemon=True).start()
    for target in (worker, janitor) + ((warmer,) if ARGS.warm_disc else ()):
        threading.Thread(target=target, daemon=True).start()
    server = ThreadingHTTPServer((ARGS.host, ARGS.port), Handler)
    server.daemon_threads = True
    print(f'KartPad-NX web builder on http://{ARGS.host}:{server.server_port} for {ARGS.origin}', flush=True)
    server.serve_forever()


if __name__ == '__main__':
    sys.exit(main())
