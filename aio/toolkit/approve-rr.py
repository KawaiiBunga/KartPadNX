#!/usr/bin/env python3
"""Add a Retro Rewind version to the approved list the AIO builder follows.

  python aio/toolkit/approve-rr.py [VERSION] [--list aio/manifest/rr-versions.json] [--no-latest] [--no-full]

VERSION defaults to the newest one in Retro Rewind's update feed. The script finds the update
zip that carries that version's Code.pul (walking back through older updates when the newest
only changed assets), records its URL, size and hashes, and makes it the list's "latest".

It also records the version's full pack (<version>-full.zip next to the update zips, ~2 GB),
which the builder unpacks into the player's output folder as RetroRewind6 when "Also download
the Retro Rewind SD files" is ticked. The pack must carry xml/RetroRewind6.xml and the same
Code.pul. --no-full skips it; the builder then tells players to fetch the files themselves.

Before publishing the list, check the version builds (and plays):

  python aio/pipeline/kpnx_aio.py --rr --rr-sd --rr-list aio/manifest/rr-versions.json --data ... --disc ... --out ...

Then upload the list to the public URL in tools.json (retro_rewind.approved_url). Players get
the version the next time they click Build; only Retro Rewind is re-translated.
"""
import argparse
import hashlib
import json
import sys
import time
import urllib.parse
import zipfile
from pathlib import Path

AIO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AIO / 'pipeline'))
import kpnx_aio  # noqa: E402

CODE_PUL = 'RetroRewind6/Binaries/Code.pul'
XML = 'RetroRewind6/xml/RetroRewind6.xml'


def feed():
    with kpnx_aio._request(kpnx_aio.RR_FEED) as r:
        rows = [line.split() for line in r.read().decode('utf-8', 'replace').splitlines() if line.strip()]
    return [(row[0], row[1]) for row in rows if len(row) >= 2]


def remote_size(url):
    with kpnx_aio._request(url, byte_range=(0, 0)) as r:
        if r.status == 206:
            return int(r.headers['Content-Range'].rsplit('/', 1)[1])
        return int(r.headers['Content-Length'])


def full_pack(version, update_url, code, cache):
    """The full pack's URL, size and hash, after checking it holds the approved Code.pul."""
    url = update_url.rsplit('/', 1)[0] + f'/{version}-full.zip'
    if kpnx_aio._full_pack_problem({'full_url': url, 'full_size': 1, 'full_sha256': '0' * 64}):
        sys.exit(f'{url} is not on {kpnx_aio.RR_CDN}')
    size = remote_size(url)
    path = kpnx_aio.download(url, cache / Path(urllib.parse.urlparse(url).path).name, size,
                             label=f'Retro Rewind {version} full pack')
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        if XML not in names or CODE_PUL not in names:
            sys.exit(f'{url} has no {XML} or {CODE_PUL}')
        if hashlib.sha256(z.read(CODE_PUL)).hexdigest() != code:
            sys.exit(f'{url} carries a different Code.pul than the {version} update chain')
    return {'full_url': url, 'full_size': size, 'full_sha256': kpnx_aio.sha256_file(path)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('version', nargs='?')
    ap.add_argument('--list', type=Path, default=AIO / 'manifest/rr-versions.json')
    ap.add_argument('--cache', type=Path, default=AIO / 'out/cache/rr')
    ap.add_argument('--no-latest', action='store_true', help='add the entry without making it the latest')
    ap.add_argument('--no-full', action='store_true', help='do not record the full pack (SD files)')
    args = ap.parse_args()

    updates = feed()
    version = args.version or updates[-1][0]
    index = max((i for i, (v, _) in enumerate(updates) if v == version), default=None)
    if index is None:
        sys.exit(f'{version} is not in the Retro Rewind feed')
    # The newest update at or before VERSION that ships Code.pul defines its code.
    for v, url in reversed(updates[:index + 1]):
        path = kpnx_aio.download(url, args.cache / Path(url).name, label=f'Retro Rewind {v}')
        with zipfile.ZipFile(path) as z:
            if CODE_PUL in z.namelist():
                code = hashlib.sha256(z.read(CODE_PUL)).hexdigest()
                break
        print(f'{v} has no Code.pul; looking at the previous update')
    else:
        sys.exit(f'no update up to {version} carries Code.pul')
    full = {} if args.no_full else full_pack(version, updates[index][1], code, args.cache)

    data = json.loads(args.list.read_text(encoding='utf-8')) if args.list.is_file() else {'schema': 1, 'versions': {}}
    data['versions'][version] = {'url': url, 'size': path.stat().st_size, 'sha256': kpnx_aio.sha256_file(path),
                                 'code_pul_sha256': code, 'code_from': v, **full,
                                 'approved_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())}
    if not args.no_latest:
        data['latest'] = version
    data['versions'] = dict(sorted(data['versions'].items(), key=lambda kv: kpnx_aio._version_key(kv[0])))
    args.list.write_text(json.dumps(data, indent=2) + '\n', encoding='utf-8')
    print(f'{version}: Code.pul {code[:12]} from the {v} update; '
          f'{"full pack " + full["full_sha256"][:12] if full else "no full pack"}; '
          f'latest = {data.get("latest")} in {args.list}')


if __name__ == '__main__':
    main()
