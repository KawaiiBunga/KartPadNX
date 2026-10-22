#!/usr/bin/env python3
"""Generate the private model fusion from the user's translated function files.

No translated instruction bodies belong in Git. This recipe preserves guest
instructions, checked memory paths and dynamic external dispatch, inlining only
the listed base-region edges behind the publication/active-target guard.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re

REGION=('800605C0','800606E0','80060980','800609E0','80060A60','800619C0',
        '80061B80','80062050','800620C0','80062330','80062480','80062650',
        '80062860','80062910','800629C0','80062A80','80063380','80063410',
        '800636C0','80063870','80063EE0','80063FE0','80064450','80064B30','80064FD0')
CALL=re.compile(r'InvokeDirectCpu<0x([0-9A-Fa-f]{8})u>\(ctx\);')
TRAIT=re.compile(r'MKW_TRANSLATED_TRAIT\(([0-9A-Fa-f]{8}),\s*(func_[0-9A-Fa-f]{8}),\s*(0x[0-9A-Fa-f]+u)\);')
PURE=r'    r\d+(?:_[a-z0-9]+)* = [r0-9_a-z +*()|&~^<>\-]+;\n'
BP=re.compile(r'    GX_HLE_FIFO_Write8\(static_cast<uint8_t>\((?P<opcode>r\d+)\)\);\n'
              r'(?P<between>(?:'+PURE+r')*)'
              r'    GX_HLE_FIFO_Write32\(static_cast<uint32_t>\((?P<word>r\d+)\)\);')

def load_mask():
    spec=importlib.util.spec_from_file_location('capture_flat_base',Path(__file__).with_name('capture-flat-base.py'))
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module.code_mask

def body(text,address,mask):
    match=re.search(r'extern "C" void func_'+address+r'\([^\n]*\)\s*\n\{',text)
    if not match: raise ValueError('Missing translated entry '+address)
    code=mask(text);depth=1
    for end in range(match.end(),len(text)):
        if code[end]=='{':depth+=1
        elif code[end]=='}':
            depth-=1
            if depth==0:return text[match.end()-1:end+1]
    raise ValueError('Unclosed translated entry '+address)

def generate(translation,output):
    mask=load_mask();sources={};masks={};hashes={};traits={}
    for address in REGION:
        path=translation/'functions'/('func_'+address+'.cpp')
        raw=path.read_bytes();text=raw.decode().replace('\r\n','\n')
        metadata=re.search(r'// RECOMP_REGISTRATION base 0x'+address+r' func_'+address+r' preserves=(?:true|false) fpr_mask=(0x[0-9A-Fa-f]+)',text)
        if not metadata:raise ValueError('Unsupported registration/ABI '+address)
        masks[address]=int(metadata[1],16);sources[address]=body(text,address,mask)
        hashes[address]=hashlib.sha256(raw).hexdigest()
    # Use exactly the trait declarations emitted with the region's original
    # shards, including entry overrides. Never invent a static binding.
    for shard in sorted((translation/'build_shards/base_common').glob('*.cpp')):
        text=shard.read_text()
        if not any('void func_'+a+'(' in text for a in REGION):continue
        for address in REGION:
            if 'void func_'+address+'(' in text:sources[address]=body(text,address,mask)
        for include in re.findall(r'#include "([^"]+_traits.h)"',text):
            text+='\n'+(shard.parent/include).read_text()
        for address,winner,fpr in TRAIT.findall(text):
            item=(winner,fpr)
            if address in traits and traits[address]!=item:raise ValueError('Conflicting trait '+address)
            traits[address]=item
    for address in REGION:
        item=('func_'+address,f'0x{masks[address]:08X}u')
        if address not in traits:traits[address]=item
    if not set(REGION)<=traits.keys():raise ValueError('Incomplete shard trait graph')
    output.parent.mkdir(parents=True,exist_ok=True)
    logical=(Path(__file__).resolve().parents[1]/'switch/src/native_model_region.cpp').as_posix()
    lines=[f'#line 1 "{logical}"','#include "ppc_runtime.h"','#include "abi_bridge.h"']
    lines += [f'MKW_TRANSLATED_TRAIT({a}, {w}, {m});' for a,(w,m) in sorted(traits.items())]
    lines+=['#include "hle/native_model_submit.h"','#include "hle/native_model_cache.h"',
            '#include <atomic>','#include <cstdio>','extern "C" bool MkwNativeModelRecording();','namespace {']
    lines += [f'__attribute__((always_inline)) inline void Model_{a}(CpuContext* MKW_RESTRICT ctx);' for a in REGION]
    edges=sites=0
    for address in REGION:
        code=sources[address]
        def inline(match):
            nonlocal edges
            target=match[1].upper()
            if target not in sources:return match[0]
            if masks[target]==0:edges+=1
            call=f'Model_{target}(ctx);'
            if masks[target]:call=f'{{ PpcNonvolatileFprGuard guard(ctx, 0x{masks[target]:08x}u); {call} }}'
            return call
        code=CALL.sub(inline,code)
        static=re.compile(r'MKW_STATIC_TRANSLATED_CALL\(0x([0-9A-Fa-f]{8})u,\s*(func_[0-9A-Fa-f]{8}),\s*ctx\);')
        def direct(match):
            if match[1].upper() in sources:
                return inline(re.match(CALL,'InvokeDirectCpu<0x'+match[1]+'u>(ctx);'))
            return match[2]+'(ctx);'
        code=static.sub(direct,code)
        def submit(match):
            nonlocal sites
            prefix=code[:match.start()]
            block=prefix.rfind('[[maybe_unused]]')
            assigns=list(re.finditer(r'\b'+match['opcode']+r'\s*=\s*([^;]+);',prefix[block:]))
            if not assigns or assigns[-1][1]!='97':return match[0]
            since=prefix[block+assigns[-1].end():]
            if 'goto ' in since or 'if (' in since:return match[0]
            sites+=1
            return ('    // Complete BP register: bypass byte serialization/ring parsing.\n'+
                    match['between']+'    GX_HLE_SubmitBP(static_cast<uint32_t>('+match['word']+'));')
        code=BP.sub(submit,code)
        lines += [f'__attribute__((always_inline)) inline void Model_{address}(CpuContext* MKW_RESTRICT ctx)',code]
    lines += ['bool RegionAvailable() {','    static std::atomic<uint8_t> cached{0};',
              '    const auto verdict = cached.load(std::memory_order_acquire);',
              '    if (verdict) return verdict == 2;',
              '    const bool published = TranslatedFunctionRegistry::IsLookupPublished();','    const bool active =']
    lines += [f'        IsBaseTranslatedCpuTargetActive<0x{a}u>()'+(' &&' if i+1<len(REGION) else ';') for i,a in enumerate(REGION)]
    lines += ['    if (published) cached.store(active ? 2 : 1, std::memory_order_release);','    return active;','}',
              'uint64_t accepted = 0, declined = 0;','} // namespace','#include "native_model_region_footer.inc"']
    output.write_text('\n'.join(lines)+'\n')
    manifest={'functions':list(REGION),'inlined_static_edges':edges,'structured_bp_sites':sites,
              'native_texture_comparisons':False,'source_sha256':hashes}
    output.with_suffix('.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(f'Generated private model region: {len(REGION)} functions, {edges} inline edges, {sites} BP sites')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('translation',type=Path);p.add_argument('output',type=Path)
    args=p.parse_args();generate(args.translation.resolve(),args.output.resolve())

if __name__=='__main__':main()
