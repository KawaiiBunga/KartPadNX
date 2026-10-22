#!/usr/bin/env python3
"""Fuse compatible scene/animation regions from the user's local translation.

Generated game bodies remain private. Preserve every memory operation, callback
and floating-point operation; only eligible internal dispatch boundaries change.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re

ANIMATION = ('80055540','80055130','80055150','800551E0','80055250',
             '80055290','80055340','800553E0','80055480','80053D00',
             '80054460','80054ED0','80053FB0','800540F0','80054270')
GROUPS = {'animation': ANIMATION}
REGION = tuple(dict.fromkeys(a for group in GROUPS.values() for a in group))
ROOTS = {group[0]: name for name, group in GROUPS.items()}
TAILS = ANIMATION[1:9]
# Keep large evaluators shared: duplicating their FP slow paths at every tail
# increases instruction-cache pressure and register spills. Dispatch-only
# resource/tail routines inline into the root; internal helper calls stay native.
SHARED = frozenset(ANIMATION[9:])
def function_prefix(address):
    return '__attribute__((noinline)) void' if address in SHARED else '__attribute__((always_inline)) inline void'
DIRECT = re.compile(r'InvokeDirectCpu<0x([0-9A-Fa-f]{8})u>\(ctx\);')
STATIC = re.compile(r'MKW_STATIC_TRANSLATED_CALL\(0x([0-9A-Fa-f]{8})u,\s*(func_[0-9A-Fa-f]{8}),\s*ctx\);')

def load_sources(translation):
    spec = importlib.util.spec_from_file_location('model_recipe', Path(__file__).with_name('generate-native-model.py'))
    model = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(model)
    mask = model.load_mask()
    bodies, fpr_masks, hashes, traits = {}, {}, {}, {}
    for address in REGION:
        path = translation/'functions'/f'func_{address}.cpp'
        raw = path.read_bytes()
        text = raw.decode().replace('\r\n','\n')
        meta = re.search(r'// RECOMP_REGISTRATION base 0x'+address+r' func_'+address+r' preserves=(?:true|false) fpr_mask=(0x[0-9A-Fa-f]+)', text)
        if not meta:
            raise ValueError('Unsupported base registration: '+address)
        bodies[address] = model.body(text, address, mask)
        fpr_masks[address] = int(meta[1],16)
        hashes[address] = hashlib.sha256(raw).hexdigest()
    # Use the currently built shard bodies, including prior source transforms.
    for shard in sorted((translation/'build_shards/base_common').glob('*.cpp')):
        text = shard.read_text()
        if not any('void func_'+a+'(' in text for a in REGION):
            continue
        for address in REGION:
            if 'void func_'+address+'(' in text:
                bodies[address] = model.body(text,address,mask)
                hashes['shard:'+address] = hashlib.sha256(bodies[address].encode()).hexdigest()
        for include in re.findall(r'#include "([^"]+_traits.h)"',text):
            text += '\n'+(shard.parent/include).read_text()
        for address,winner,fpr in model.TRAIT.findall(text):
            item = (winner,fpr)
            if address in traits and traits[address] != item:
                raise ValueError('Conflicting trait '+address)
            traits[address] = item
    for address in REGION:
        winner, fpr = traits.setdefault(address, ('func_'+address,f'0x{fpr_masks[address]:08X}u'))
        if winner != 'func_'+address or int(fpr.rstrip('uU'),16) != fpr_masks[address]:
            raise ValueError('Unexpected shard ABI/winner '+address)
    return bodies, fpr_masks, hashes, traits

def generate(translation, output):
    bodies, fpr_masks, hashes, traits = load_sources(translation)
    lines = ['#include "ppc_runtime.h"', '#include "isa/ppc_isa_cr.h"', '#ifndef MKW_SCENE_FUSION_TEST',
             '#include "abi_bridge.h"',
             '#define MKW_STATIC_TRANSLATED_CALL(T,E,C) do { ApplyRuntimeCallOptions(T,C); E(C); } while (false)',
             '#endif', '#include <atomic>', '#include <cstdio>',
             'extern "C" int kartpad_opt(const char*, int) __attribute__((weak));']
    lines += ['#ifndef MKW_SCENE_FUSION_TEST']
    lines += [f'MKW_TRANSLATED_TRAIT({a}, {w}, {m});' for a,(w,m) in sorted(traits.items())]
    lines += ['#endif','namespace {']
    lines += [f'{function_prefix(a)} Scene_{a}(CpuContext* MKW_RESTRICT ctx);' for a in REGION]
    edges = 0
    def internal(target):
        nonlocal edges
        edges += 1
        call = f'Scene_{target}(ctx);'
        if fpr_masks[target]:
            call = f'{{ PpcNonvolatileFprGuard guard(ctx, 0x{fpr_masks[target]:08X}u); {call} }}'
        return call
    for address in REGION:
        def direct(match):
            target = match[1].upper()
            return internal(target) if target in bodies else match[0]
        code = DIRECT.sub(direct,bodies[address])
        code = STATIC.sub(direct,code)
        if address == ANIMATION[0]:
            tail = ['switch (ctr) {']
            for target in TAILS:
                tail += [f'case 0x{target}u: {internal(target)} break;']
            tail += ['default: InvokeIndirectJump(ctr, ctx); break;', '}']
            needle = 'InvokeIndirectJump(ctr, ctx);'
            if code.count(needle) != 1:
                raise ValueError('Unexpected animation tail dispatch')
            code = code.replace(needle,'\n'.join(tail))
        lines += [f'{function_prefix(address)} Scene_{address}(CpuContext* MKW_RESTRICT ctx)',code]
    for name,group in GROUPS.items():
        lines += [f'bool Available_{name}() {{', '    static std::atomic<uint8_t> cached{0};',
                  '    const auto verdict = cached.load(std::memory_order_acquire);',
                  '    if (verdict) return verdict == 2;',
                  '    const bool published = TranslatedFunctionRegistry::IsLookupPublished();',
                  '    const bool active =']
        lines += [f'        IsBaseTranslatedCpuTargetActive<0x{a}u>()'+(' &&' if i+1<len(group) else ';') for i,a in enumerate(group)]
        lines += ['    if (published) cached.store(active ? 2 : 1, std::memory_order_release);',
                  '    return active;', '}']
    lines += [f'uint64_t accepted[{len(GROUPS)}]{{}}, declined[{len(GROUPS)}]{{}};', '} // namespace']
    for index,(root,name) in enumerate(ROOTS.items()):
        lines += [f'extern "C" void __real_func_{root}(CpuContext*);',
                  f'extern "C" void __wrap_func_{root}(CpuContext* ctx) {{',
                  f'    static const bool on = kartpad_opt && kartpad_opt("native_{name}_region", 0) != 0;',
                  f'    if (on && Available_{name}()) {{ ++accepted[{index}]; Scene_{root}(ctx); return; }}',
                  f'    ++declined[{index}]; __real_func_{root}(ctx);', '}']
    lines += ['void MkwReportNativeSceneRegions() {',
              '    std::fprintf(stderr, "[HOS native regions] animation=%llu/%llu\\n",',
              '        (unsigned long long)accepted[0], (unsigned long long)declined[0]);',
              f'    for (unsigned i=0;i<{len(GROUPS)};++i) accepted[i]=declined[i]=0;', '}']
    output.parent.mkdir(parents=True,exist_ok=True)
    output.write_text('\n'.join(lines)+'\n')
    manifest = {'groups': GROUPS, 'internal_edges': edges, 'shared_evaluators': sorted(SHARED), 'animation_tail_targets': TAILS,
                'source_sha256': hashes, 'nonvolatile_fpr_masks': fpr_masks}
    output.with_suffix('.json').write_text(json.dumps(manifest,indent=2)+'\n')
    print(f'Generated private scene regions: {len(REGION)} functions, {edges} internal edges')

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('translation',type=Path)
    p.add_argument('output',type=Path)
    a=p.parse_args()
    generate(a.translation.resolve(),a.output.resolve())
