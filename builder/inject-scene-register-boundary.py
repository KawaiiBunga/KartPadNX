#!/usr/bin/env python3
"""Enforce EABI registers at GameScene::enter's cold SDK/virtual boundaries.

Only emitted GameScene::enter bodies are changed. Call lines retain their line
numbers for sample PGO; the header precedes the shard's #line reset. Other
translated functions and optimizations are preserved. Safe to run repeatedly.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re

HEAD = re.compile(r'extern "C" void (?:func_8051AB58(?:_retro_rewind)?|rr_overlay_8051AB58)\(CpuContext\* MKW_RESTRICT ctx\)\s*\{')
CALL = re.compile(r'^(\s*)(InvokeIndirectCpu\((ctr|r12), ctx\);|(?:MKW_STATIC_TRANSLATED_CALL\(0x80540680u, \w+, ctx\);|InvokeDirectCpu<0x80540680u>\(ctx\);))(\r?)$', re.M)
HEADER = '#include "scene_register_boundary.h"\n'

def transform(text):
    matches = list(HEAD.finditer(text))
    count = 0
    for match in reversed(matches):
        opening = text.index('{', match.start())
        end, depth = opening + 1, 1
        while depth:
            depth += (text[end] == '{') - (text[end] == '}')
            end += 1
        body = text[opening:end]
        if 'kartpad::SceneRegisterBoundary' in body:
            continue
        def replace(call):
            target = call.group(3) or '0x80540680u'
            return (call.group(1)+'{ kartpad::SceneRegisterBoundary sceneBoundary(ctx->gpr, r1, '
                    '{r26, r27, r28, r29, r30, r31}, '+target+', ctx->lr); '+call.group(2)+' }'+call.group(4))
        updated, n = CALL.subn(replace, body)
        assert n >= 2, 'Expected SDK archive call and virtual boundaries in GameScene::enter'
        assert updated.count('\n') == body.count('\n')
        count += n
        text = text[:opening]+updated+text[end:]
    if count and HEADER not in text:
        assert '#line 1 ' in text, 'This hook operates on aggregate shards only'
        offset = text.index('#line 1 ')
        text = text[:offset]+HEADER+text[offset:]
    return text, count

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shards', type=Path)
    parser.add_argument('--archive', type=Path)
    args = parser.parse_args()
    results = []
    for path in sorted(args.shards.rglob('*.cpp')):
        raw = path.read_bytes()
        if b'8051AB58(CpuContext* MKW_RESTRICT ctx)' not in raw and b'8051AB58_retro_rewind(CpuContext* MKW_RESTRICT ctx)' not in raw:
            continue
        text = raw.decode('utf-8')
        updated, count = transform(text)
        if not count: continue
        if args.archive:
            backup = args.archive/path.relative_to(args.shards)
            backup.parent.mkdir(parents=True, exist_ok=True)
            assert not backup.exists(), backup
            backup.write_bytes(raw)
        data = updated.encode('utf-8')
        path.write_bytes(data)
        assert transform(updated) == (updated, 0)
        results.append(dict(path=str(path), boundaries=count,
                            before_sha256=hashlib.sha256(raw).hexdigest(),
                            sha256=hashlib.sha256(data).hexdigest()))
    print(json.dumps(results, indent=2))

if __name__ == '__main__': main()
