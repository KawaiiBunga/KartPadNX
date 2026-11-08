#!/usr/bin/env python3
"""Self-test for build_server.py: disc header parsing, and a full upload -> build -> download run
against a fake pipeline. No disc or real build needed.

  python web/server/test_build_server.py
"""
import io
import json
import socket
import zipfile
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import build_server  # noqa: E402

FAKE_PIPELINE = r'''
import argparse, json, pathlib, time
ap = argparse.ArgumentParser()
ap.add_argument('--out'); ap.add_argument('--disc'); ap.add_argument('--data'); ap.add_argument('--rr', action='store_true')
a, _ = ap.parse_known_args()
out = pathlib.Path(a.out)
(out / 'switch/kartpad/data/sys').mkdir(parents=True, exist_ok=True)
(out / 'switch/kartpad/data/sys/boot.bin').write_bytes(b'BOOT' * 256)
(out / 'INSTALL.txt').write_text('copy to SD')
print('@@' + json.dumps({'stage': 'build-base', 'title': 'Compiling'}), flush=True)
print('fake log line', flush=True)
print(r'reading C:\Users\someone\KartPadNX\private\web-builder\jobs\x\upload.iso from 192.168.1.165:5000 and fe80:0:0:0:1:2:3:4', flush=True)
print('@@' + json.dumps({'progress': 'x', 'done': 1, 'total': 2}), flush=True)
print('@@' + json.dumps({'rr_version': '9.9.9' if a.rr else None}), flush=True)
nro = pathlib.Path(a.out) / 'switch/mkw-launcher.nro'
nro.parent.mkdir(parents=True, exist_ok=True)
nro.write_bytes(b'NRO0' + pathlib.Path(a.disc).read_bytes()[:16])
'''


def header(game=b'RMCP01', rev=0):
    h = bytearray(0x80)
    h[:6], h[7], h[0x18:0x1C] = game, rev, bytes.fromhex('5D1C9EA3')
    return bytes(h)


def check(cond, what):
    print(('PASS ' if cond else 'FAIL ') + what)
    if not cond:
        global FAILED
        FAILED = True


FAILED = False


def call(base, method, path, token=None, body=None, data=None):
    headers = {'X-Job-Token': token} if token else {}
    if body is not None:
        data, headers['Content-Type'] = json.dumps(body).encode(), 'application/json'
    req = urllib.request.Request(base + path, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req) as r:
            raw = r.read()
            return r.status, (json.loads(raw) if r.headers['Content-Type'] == 'application/json' else raw)
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read())


def main():
    tmp = Path(tempfile.mkdtemp())
    # Header parsing for each container.
    iso = tmp / 'game.iso'
    iso.write_bytes(header() + bytes(70 << 20))            # three 32 MiB chunks
    wbfs = tmp / 'game.wbfs'
    wbfs.write_bytes(b'WBFS' + bytes(4) + bytes([9]) + bytes(512 - 9) + header())
    rvz = tmp / 'game.rvz'
    rvz.write_bytes(b'RVZ\x01' + bytes(0x58 - 4) + header() + bytes(64))
    bad = tmp / 'usa.iso'
    bad.write_bytes(header(b'RMCE01') + bytes(1024))
    rev1 = tmp / 'rev1.iso'
    rev1.write_bytes(header(rev=1) + bytes(1024))
    check(build_server.disc_problem(iso) is None, 'ISO RMCP01 rev 0 accepted')
    check(build_server.disc_problem(wbfs) is None, 'WBFS header found in sector 1')
    check(build_server.disc_problem(rvz) is None, 'RVZ disc header found at 0x58')
    check('RMCE01' in (build_server.disc_problem(bad) or ''), 'NTSC disc refused')
    check('revision 1' in (build_server.disc_problem(rev1) or ''), 'revision 1 refused')

    fake = tmp / 'fake_pipeline.py'
    fake.write_text(FAKE_PIPELINE)
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
    server = subprocess.Popen([sys.executable, str(HERE / 'build_server.py'), '--work', str(tmp / 'work'),
                               '--origin', 'https://example.github.io', '--port', str(port), '--python', sys.executable,
                               '--pipeline', str(fake), '--', '--fake'])
    base = f'http://127.0.0.1:{port}'
    try:
        for _ in range(50):
            try:
                code, st = call(base, 'GET', '/api/status')
                break
            except OSError:
                time.sleep(0.2)
        check(code == 200 and st['ok'], 'status endpoint up')

        code, r = call(base, 'POST', '/api/jobs', body={'name': 'x.zip', 'size': 10})
        check(code == 400, 'non-disc file name refused')
        code, job = call(base, 'POST', '/api/jobs', body={'name': 'game.iso', 'size': iso.stat().st_size, 'rr': True})
        check(code == 201, 'job created')
        jid, tok, chunk = job['id'], job['token'], job['chunk']
        code, _ = call(base, 'GET', f'/api/jobs/{jid}', token='wrong')
        check(code == 404, 'wrong token refused')
        data = iso.read_bytes()
        code, r = call(base, 'PUT', f'/api/jobs/{jid}/upload?offset=5', tok, data=data[:10])
        check(code == 409 and r['received'] == 0, 'out-of-order chunk refused with resume offset')
        code, _ = call(base, 'POST', f'/api/jobs/{jid}/start', tok)
        check(code == 409, 'start refused before the upload is complete')
        off = 0
        while off < len(data):
            code, r = call(base, 'PUT', f'/api/jobs/{jid}/upload?offset={off}', tok, data=data[off:off + chunk])
            off = r['received']
        check(off == len(data), f'uploaded in {(len(data) + chunk - 1) // chunk} chunks')
        code, r = call(base, 'POST', f'/api/jobs/{jid}/start', tok)
        check(code == 200, 'build queued')
        for _ in range(100):
            code, s = call(base, 'GET', f'/api/jobs/{jid}', tok)
            if s['state'] in ('done', 'failed'):
                break
            time.sleep(0.2)
        check(s['state'] == 'done' and s['rr_version'] == '9.9.9' and 'fake log line' in s['log'],
              f'build finished with RR version and log ({s["state"]} {s["error"]})')
        leaked = json.dumps(s)
        check('someone' not in leaked and '192.168' not in leaked and 'fe80' not in leaked
              and 'reading <path>' in s['log'],
              'paths and IP addresses are scrubbed from what players see')
        code, t = call(base, 'POST', f'/api/jobs/{jid}/ticket', tok)
        check(code == 200 and t['url'].startswith('/api/download/'), 'one-time download link issued')
        code, raw = call(base, 'GET', t['url'])
        z = zipfile.ZipFile(io.BytesIO(raw))
        check(code == 200 and sorted(z.namelist()) == ['INSTALL.txt', 'switch/kartpad/data/sys/boot.bin',
                                                       'switch/mkw-launcher.nro']
              and z.read('switch/mkw-launcher.nro') == b'NRO0' + data[:16] and z.testzip() is None,
              'download is the whole SD card folder as a valid zip')
        code, _ = call(base, 'GET', t['url'])
        check(code == 404, 'a download link works only once')
        check(not (tmp / 'work/jobs' / jid / 'upload.iso').exists(), 'upload deleted after the build')

        code, job2 = call(base, 'POST', '/api/jobs', body={'name': 'usa.iso', 'size': bad.stat().st_size})
        call(base, 'PUT', f'/api/jobs/{job2["id"]}/upload?offset=0', job2['token'], data=bad.read_bytes())
        code, r = call(base, 'POST', f'/api/jobs/{job2["id"]}/start', job2['token'])
        check(code == 400 and 'RMCE01' in r['error'], 'wrong disc refused at start, before any build')
    finally:
        server.terminate()
        server.wait()
    print('FAILED' if FAILED else 'All web builder tests passed.')
    return 1 if FAILED else 0


if __name__ == '__main__':
    sys.exit(main())
