#!/usr/bin/env python3
"""Publish/retain GlobeScene::OnEnter's resident registers across returning calls.

Archive loading can run guest jobs while it waits. The caller must publish its
live registers and recover its own EABI-preserved state, rather than importing
the last job's scratch registers. Same-line insertion retains PGO locations.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re

HEAD=re.compile(r'extern "C" void (?:func_805538EC(?:_retro_rewind)?|rr_overlay_805538EC)\(CpuContext\* MKW_RESTRICT ctx\)\s*\{')
CALL=re.compile(r'^(\s*)(MKW_STATIC_TRANSLATED_CALL\(0x([0-9A-F]+)u, \w+, ctx\);|InvokeDirectCpu<0x([0-9A-F]+)u>\(ctx\);|InvokeIndirectCpu\((ctr|r\d+), ctx\);)(\r?)$',re.M)

def transform(text):
    count=0
    for match in reversed(list(HEAD.finditer(text))):
        a=text.index('{',match.start());b=a+1;depth=1
        while depth:depth+=(text[b]=='{')-(text[b]=='}');b+=1
        body=text[a:b]
        if 'kartpad::GlobeSceneCallBoundary' in body:continue
        regs=re.findall(r'uint32_t r(\d+) = ctx->gpr\[\d+\];',body)
        values=', '.join('{'+r+', r'+r+'}' for r in regs)
        def replace(m):
            target='0x'+(m[3] or m[4])+'u' if m[3] or m[4] else m[5]
            assert target!='0x801A1F58u'
            return m[1]+'{ kartpad::GlobeSceneCallBoundary globeBoundary(ctx->gpr, {'+values+'}, '+target+', ctx->lr); '+m[2]+' }'+m[6]
        new,n=CALL.subn(replace,body)
        assert n==8,n
        assert new.count('\n')==body.count('\n')
        text=text[:a]+new+text[b:];count+=n
    include='#include "globe_scene_call_boundary.h"\n'
    if count and include not in text:
        a=text.index('#line 1 ');text=text[:a]+include+text[a:]
    return text,count

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('shards',type=Path);p.add_argument('--archive',type=Path)
    args=p.parse_args();rows=[]
    for path in args.shards.rglob('*.cpp'):
        raw=path.read_bytes()
        if b'805538EC(CpuContext* MKW_RESTRICT ctx)' not in raw and b'805538EC_retro_rewind(CpuContext* MKW_RESTRICT ctx)' not in raw:continue
        new,n=transform(raw.decode('utf-8'))
        if not n:continue
        if args.archive:
            dest=args.archive/path.relative_to(args.shards);dest.parent.mkdir(parents=True,exist_ok=True)
            assert not dest.exists();dest.write_bytes(raw)
        data=new.encode('utf-8');path.write_bytes(data);assert transform(new)==(new,0)
        rows.append(dict(path=str(path),calls=n,before_sha256=hashlib.sha256(raw).hexdigest(),sha256=hashlib.sha256(data).hexdigest()))
    print(json.dumps(rows,indent=2))
if __name__=='__main__':main()
