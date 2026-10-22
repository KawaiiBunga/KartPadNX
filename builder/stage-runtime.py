#!/usr/bin/env python3
"""Stage the complete Switch source patch onto the pinned public WiiCompiled.

The output must be new. Existing source/translation trees are never reset or
deleted. The patch contains runtime/translator/graphics source, not game output.
"""
import argparse
import hashlib
import shutil
import subprocess
from pathlib import Path
from urllib.request import urlopen

ROOT=Path(__file__).resolve().parents[1]
SSE_COMMIT='13a42df35dc7fcc94f987568e7274a998bb6cc86'
SSE_SHA='44b9fa3dec3a52ea473246e04b9f692a4e5b0ed654299eef7fe7ec3049e223e0'

def stage(source,output):
    source=source.resolve();output=output.resolve()
    if source==output or source in output.parents or output in source.parents:
        raise ValueError('Source/output directories must be separate')
    if output.exists():raise FileExistsError('Preserve or move the existing stage before staging: '+str(output))
    if not (source/'translator/src/Translator.Cli/Translator.Cli.csproj').is_file():
        raise FileNotFoundError('Pinned WiiCompiled source is incomplete')
    shutil.copytree(source,output,ignore=shutil.ignore_patterns('.git','bin','obj','test_output'))
    patch=ROOT/'switch/patches/wiicompiled-switch-complete.patch'
    command=['patch','--batch','-p1','-d',str(output),'-i',str(patch)]
    subprocess.run(command+['--dry-run'],check=True)
    subprocess.run(command,check=True)
    header=output/'runtime/third_party/sse2neon/sse2neon.h'
    header.parent.mkdir(parents=True,exist_ok=True)
    if header.is_file():data=header.read_bytes()
    else:
        with urlopen('https://raw.githubusercontent.com/DLTcollab/sse2neon/'+SSE_COMMIT+'/sse2neon.h',timeout=60) as response:
            data=response.read()
    if hashlib.sha256(data).hexdigest()!=SSE_SHA:raise ValueError('sse2neon digest mismatch')
    header.write_bytes(data)
    (output/'.kartpad-switch-complete-stage').write_text(
        'upstream=1912292c804ff9b1b79938de89369ec4496f9fff\npatch-sha256='+hashlib.sha256(patch.read_bytes()).hexdigest()+'\n')
    print('Complete Switch sources staged at',output)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=ROOT/'ref/upstream/Wiicompiled')
    p.add_argument('--output',type=Path,default=ROOT/'build/wiicompiled-fpscr')
    args=p.parse_args();stage(args.source,args.output)

if __name__=='__main__':main()
