#!/usr/bin/env python3
"""Pack the native renderer's seeds from what consoles recorded (private/deko-training).

  python builder/deko-training.py pack-dksh OUT.bin INPUT.bin...
  python builder/deko-training.py pack-pipelines OUT.db INPUT.db...

pack-dksh merges DKSH cache files (switch/deko/dksh_file.h): the seed first, then each console's
sdmc:/switch/kartpad/Cache/deko/dksh_local.bin. Every input must carry the first one's compiler id.
Records are checked as read_dksh_file checks them (CRC, layout). A shader is kept once per stage and
GLSL hash (a compiled one replaces a failed one), a module once per hash, and of the named/stage
records the last one of each name, which is what the console keeps when it reads the seed and then
its local file. Writes OUT and OUT.json.

pack-pipelines merges Aurora pipeline recipe databases (the seed, then each console's
sdmc:/switch/kartpad/Cache/pipeline_cache_deko.db): one row per (type, hash) of the newest config
version of each type, first_frame_used the earliest seen, as a fresh schema-1 database. Writes OUT,
OUT's .id (a new id makes consoles merge the seed again) and OUT.json.
"""
import hashlib, json, sqlite3, struct, sys, zlib
from pathlib import Path

HEADER = struct.Struct('<4sIQ')
RECORD = struct.Struct('<BII')
KIND_SHADER, KIND_MODULE, KIND_NAMED, KIND_STAGE = 1, 2, 3, 4
STAGES = (0, 4)  # ShaderStage: vertex, fragment
PIPELINE_SCHEMA = 1


def read_dksh(path):
    data = Path(path).read_bytes()
    magic, version, compiler = HEADER.unpack_from(data, 0)
    if magic != b'SWDK' or version != 1:
        sys.exit(f'{path}: not a version-1 SWDK file')
    records, bad, i = [], 0, HEADER.size
    while i + RECORD.size <= len(data):
        kind, size, crc = RECORD.unpack_from(data, i)
        if i + RECORD.size + size > len(data):
            break  # cut short: the file ends here
        payload = data[i + RECORD.size:i + RECORD.size + size]
        i += RECORD.size + size
        if zlib.crc32(payload, zlib.crc32(bytes([kind]))) != crc:
            bad += 1
            continue
        key = record_key(kind, payload)
        if key is not None:
            records.append((key, kind, payload))
    return compiler, records, {'path': str(path), 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data),
                               'complete_bytes': i, 'bad_crc': bad, 'torn_bytes': len(data) - i}


def record_key(kind, p):
    """The record's identity, or None for one read_dksh_file would skip."""
    if kind == KIND_SHADER and len(p) >= 9 and p[0] in STAGES:
        return ('shader', p[0], p[1:9])
    if kind == KIND_MODULE and len(p) == 24:
        return ('module', p[0:8])
    if kind in (KIND_NAMED, KIND_STAGE) and len(p) >= 2 and p[0] in STAGES:
        n = p[1]
        if len(p) < 2 + n + 9:
            return None
        stride = 18 if kind == KIND_STAGE else 4
        if len(p) != 2 + n + 9 + stride * p[2 + n + 8]:
            return None
        return ('named', p[2:2 + n])
    return None


def pack_dksh(out, inputs):
    compiler, shaders, modules, named, meta = None, {}, {}, {}, []
    for path in inputs:
        cid, records, info = read_dksh(path)
        if compiler is None:
            compiler = cid
        elif cid != compiler:
            sys.exit(f'{path}: compiler id {cid:016x}, not {compiler:016x}: its shaders are stale')
        meta.append(info)
        for key, kind, payload in records:
            if key[0] == 'shader':
                old = shaders.get(key)
                if old is None or (len(old[1]) == 9 and len(payload) > 9):
                    shaders[key] = (kind, payload)
            elif key[0] == 'module':
                modules.setdefault(key, (kind, payload))
            else:
                named.pop(key, None)  # re-insert: dict order becomes the order of each name's last record
                named[key] = (kind, payload)
    body = bytearray(HEADER.pack(b'SWDK', 1, compiler))
    for kind, payload in list(shaders.values()) + list(modules.values()) + list(named.values()):
        body += RECORD.pack(kind, len(payload), zlib.crc32(payload, zlib.crc32(bytes([kind])))) + payload
    Path(out).write_bytes(body)
    compiled = sum(1 for _, p in shaders.values() if len(p) > 9)
    summary = {'compiler_id': f'{compiler:016x}', 'shaders': compiled, 'failed': len(shaders) - compiled,
               'modules': len(modules), 'binding_records': len(named), 'bytes': len(body),
               'sha256': hashlib.sha256(body).hexdigest(), 'inputs': meta}
    Path(out).with_suffix('.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items() if k != 'inputs'}))


def pack_pipelines(out, inputs):
    rows, meta = {}, []
    columns = 'type, hash, config_version, config_size, config, first_frame_used'
    for path in inputs:
        db = sqlite3.connect(f'file:{path}?mode=ro', uri=True)
        if db.execute('SELECT value FROM aurora_schema').fetchone() != (PIPELINE_SCHEMA,):
            sys.exit(f'{path}: not pipeline cache schema {PIPELINE_SCHEMA}')
        read = db.execute(f'SELECT {columns} FROM pipeline_cache').fetchall()
        db.close()
        meta.append({'path': str(path), 'sha256': hashlib.sha256(Path(path).read_bytes()).hexdigest(), 'rows': len(read)})
        for t, h, version, size, config, first in read:
            old = rows.get((t, h))
            if old is None or version > old[0]:
                rows[(t, h)] = (version, size, bytes(config), first)
            elif version == old[0]:
                rows[(t, h)] = (version, size, old[2], min(first, old[3]))
    newest = {}
    for (t, _), (version, *_rest) in rows.items():
        newest[t] = max(newest.get(t, version), version)
    kept = sorted(((t, h) + v for (t, h), v in rows.items() if v[0] == newest[t]), key=lambda r: (r[0], r[5], r[1]))
    out = Path(out)
    out.unlink(missing_ok=True)
    db = sqlite3.connect(out)
    db.execute('CREATE TABLE aurora_schema(value INTEGER)')
    db.execute('INSERT INTO aurora_schema VALUES (?)', (PIPELINE_SCHEMA,))
    db.execute('CREATE TABLE pipeline_cache (\n'
               '          type INTEGER NOT NULL, hash INTEGER NOT NULL, config_version INTEGER NOT NULL,\n'
               '          config_size INTEGER NOT NULL, config BLOB NOT NULL, first_frame_used INTEGER NOT NULL,\n'
               '          PRIMARY KEY (type, hash))')
    db.executemany(f'INSERT INTO pipeline_cache ({columns}) VALUES (?, ?, ?, ?, ?, ?)', kept)
    db.commit()
    db.execute('VACUUM')
    db.close()
    digest = hashlib.sha256()
    for r in kept:
        digest.update(struct.pack('<qqqq', r[0], r[1], r[2], r[3]) + r[4] + struct.pack('<q', r[5]))
    seed_id = digest.hexdigest()[:16]
    out.with_suffix('.id').write_text(seed_id, encoding='ascii')
    summary = {'seed_id': seed_id, 'recipes': len(kept), 'clear': sum(1 for r in kept if r[0] == 0),
               'gx': sum(1 for r in kept if r[0] == 1), 'config_versions': {str(t): v for t, v in sorted(newest.items())},
               'stale': len(rows) - len(kept), 'bytes': out.stat().st_size,
               'sha256': hashlib.sha256(out.read_bytes()).hexdigest(), 'inputs': meta}
    out.with_suffix('.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: v for k, v in summary.items() if k != 'inputs'}))


if __name__ == '__main__':
    if len(sys.argv) < 4 or sys.argv[1] not in ('pack-dksh', 'pack-pipelines'):
        sys.exit(__doc__)
    (pack_dksh if sys.argv[1] == 'pack-dksh' else pack_pipelines)(sys.argv[2], sys.argv[3:])
