#!/usr/bin/env python3
"""Publish resident GPRs before the shared blocking ArchiveMgr::Wait API.

Runs after register-flush elision. Keep each call on its original source line
for sample PGO. Does not alter the call, its outputs or guest memory accesses.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re

HEAD = re.compile(r'extern "C" void (\w+)\(CpuContext\* MKW_RESTRICT ctx\)\s*\{')
CALL = re.compile(r'^([ \t]*)(MKW_STATIC_TRANSLATED_CALL\(0x805400A0u, \w+, ctx\);|InvokeDirectCpu<0x805400A0u>\(ctx\);)(\r?)$', re.M)
HEADER = '#include "archive_wait_checkpoint.h"\n'

def transform(text):
    count = 0
    functions = []
    for match in reversed(list(HEAD.finditer(text))):
        opening = text.index('{', match.start())
        end, depth = opening+1, 1
        while depth:
            depth += (text[end]=='{')-(text[end]=='}')
            end += 1
        body = text[opening:end]
        if not CALL.search(body): continue
        locals_ = [int(n) for n in re.findall(r'uint32_t r(\d+) = ctx->gpr\[\d+\];', body)]
        assert locals_ and len(locals_) == len(set(locals_)), match.group(1)
        values = ', '.join('{'+str(n)+', r'+str(n)+'}' for n in locals_)
        def replace(call):
            return (call.group(1)+'kartpad::PublishArchiveWaitCheckpoint(ctx->gpr, {'+values+
                    '}, ctx->lr); '+call.group(2)+call.group(3))
        updated, n = CALL.subn(replace, body)
        assert updated.count('\n') == body.count('\n')
        text = text[:opening]+updated+text[end:]
        functions.append(dict(function=match.group(1), calls=n, locals=locals_))
        count += n
    if count and HEADER not in text:
        assert '#line 1 ' in text
        pos = text.index('#line 1 ')
        text = text[:pos]+HEADER+text[pos:]
    return text, functions

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shards',type=Path)
    parser.add_argument('--archive',type=Path)
    args = parser.parse_args()
    results = []
    for path in sorted(args.shards.rglob('*.cpp')):
        raw = path.read_bytes()
        if b'0x805400A0u' not in raw: continue
        updated, functions = transform(raw.decode('utf-8'))
        if not functions: continue
        if args.archive:
            backup = args.archive/path.relative_to(args.shards)
            backup.parent.mkdir(parents=True,exist_ok=True)
            assert not backup.exists(),backup
            backup.write_bytes(raw)
        data = updated.encode('utf-8')
        path.write_bytes(data)
        assert transform(updated)==(updated,[])
        results.append(dict(path=str(path),functions=functions,
            before_sha256=hashlib.sha256(raw).hexdigest(),sha256=hashlib.sha256(data).hexdigest()))
    print(json.dumps(results,indent=2))

if __name__=='__main__': main()
