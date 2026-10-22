#!/usr/bin/env python3
"""Protect returning archive waits and their internal SDK/virtual boundaries.

Same-line insertion preserves translated source locations for sample PGO.
Run after flush elision and the existing archive checkpoint injection.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re

HEAD=re.compile(r'extern "C" void (\w+)\(CpuContext\* MKW_RESTRICT ctx\)\s*\{')
CALL=re.compile(r'^(\s*)(?:kartpad::PublishArchiveWaitCheckpoint\(ctx->gpr, \{.*?\}, ctx->lr\); )?'
    r'(MKW_STATIC_TRANSLATED_CALL\(0x([0-9A-Fa-f]+)u, \w+, ctx\);|'
    r'InvokeDirectCpu<0x([0-9A-Fa-f]+)u>\(ctx\);|InvokeIndirectCpu\((ctr|r\d+), ctx\);)(\r?)$',re.M)

def transform(text):
    changed=[]
    for match in reversed(list(HEAD.finditer(text))):
        opening=text.index('{',match.start())
        end,depth=opening+1,1
        while depth:
            depth+=(text[end]=='{')-(text[end]=='}');end+=1
        body=text[opening:end]
        internal=match.group(1) in ('func_805400A0','func_805400A0_retro_rewind','rr_overlay_805400A0')
        locals_=[int(n) for n in re.findall(r'uint32_t r(\d+) = ctx->gpr\[\d+\];',body)]
        values=', '.join('{'+str(n)+', r'+str(n)+'}' for n in locals_)
        count=0
        def replace(call):
            nonlocal count
            address=call.group(3) or call.group(4)
            if not internal and address!='805400A0':return call.group(0)
            target='0x'+address+'u' if address else call.group(5)
            assert address!='801A1F58', 'Never guard rfi'
            count+=1
            return (call.group(1)+'{ kartpad::ArchiveCallBoundary archiveBoundary(ctx->gpr, {'+values+
                '}, '+target+', ctx->lr); '+call.group(2)+' }'+call.group(6))
        updated=CALL.sub(replace,body)
        if count:
            assert updated.count('\n')==body.count('\n')
            text=text[:opening]+updated+text[end:]
            changed.append({'function':match.group(1),'calls':count,'internal_wait':internal})
    header='#include "archive_call_boundary.h"\n'
    if changed and header not in text:
        pos=text.index('#line 1 ');text=text[:pos]+header+text[pos:]
    return text,changed

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('shards',type=Path);p.add_argument('--archive',type=Path)
    args=p.parse_args();results=[]
    for path in sorted(args.shards.rglob('*.cpp')):
        raw=path.read_bytes()
        if b'805400A0' not in raw:continue
        text,changes=transform(raw.decode())
        if not changes:continue
        if args.archive:
            dst=args.archive/path.relative_to(args.shards);dst.parent.mkdir(parents=True,exist_ok=True)
            assert not dst.exists();dst.write_bytes(raw)
        data=text.encode();path.write_bytes(data)
        assert transform(text)==(text,[])
        results.append(dict(path=str(path),changes=changes,before_sha256=hashlib.sha256(raw).hexdigest(),sha256=hashlib.sha256(data).hexdigest()))
    print(json.dumps(results,indent=2))
if __name__=='__main__':main()
