#!/usr/bin/env python3
"""Stitch RR's two non-link menu-model hooks into their containing guest CFG.

These branches are not EABI calls: their continuations finish the containing
function and restore its stack. Keeping them as host calls followed by LR
redispatch allows a returned continuation to execute the epilogue again, using
already-restored caller registers. Materialize the known branch graph instead.
Only MenuDriverModel::SetPlayerCharacter changes; other #line units stay intact.
"""
from pathlib import Path
import argparse
import hashlib
import json
import re

# The two Kamek hooks live in the RR module, whose layout moves between RR
# releases (6.13.1: 81806FF0/81806F8C; 6.13.3: 818073E8/81807384, identical
# code). main() reads them from the branch sites before transforming.
NAMES = ['rr_overlay_80830D00', 'rr_kamek_81806FF0', 'rr_kamek_81806F8C', 'rr_continue_80830F18']

def hook_addresses(overlay):
    found = []
    for site in ('loc_80830D7C:', 'loc_80830EDC:'):
        a = overlay.index(site)
        end = overlay.find('[[maybe_unused]] loc_', a)
        block = overlay[a:end if end >= 0 else len(overlay)]
        m = re.search(r'InvokeDirectCpu<0x(818[0-9A-F]{5})u>|goto loc_(818[0-9A-F]{5});', block)
        assert m, site
        found.append(m[1] or m[2])
    return found

MARKER = '// KARTPAD_MENU_MODEL_CFG: non-link hooks share locals and one epilogue'

def function(s, name):
    a = s.index('extern "C" void '+name+'(')
    b = s.index('{', a)
    e, depth = b+1, 1
    while depth:
        depth += (s[e] == '{') - (s[e] == '}')
        e += 1
    return a, e, s[a:e]

def hook_body(s, continuation):
    a = s.index('    goto loc_')
    b = s.index('    MKW_STATIC_TRANSLATED_CALL(0x'+continuation+'u,')
    # The final block only publishes the branch inputs, invokes the rest of
    # this same function, then reloads/re-publishes its already returned state.
    block = s.rfind('{', a, b)
    return s[a:block+1]+'\n    goto loc_'+continuation+';\n}\n'

def transform(s, sources):
    if MARKER in s: return s
    a, e, body = function(s, NAMES[0])
    # The first hook returns to an existing local continuation, the second to
    # a translated tail chunk whose body belongs to this same guest frame.
    for label, following, hook, cont in [
        ('80830D7C', '80830DA4', NAMES[1], '80830DA4'),
        ('80830EDC', '8083102C', NAMES[2], '80830F18')]:
        start = body.index('[[maybe_unused]] loc_'+label+':')
        stop = body.index('[[maybe_unused]] loc_'+following+':', start)
        if label == '80830EDC':
            # Preserve instructions before the patched non-link branch.
            prefix = '''[[maybe_unused]] loc_80830EDC:
{
    r31 = (r3 + r29);
    MemoryInline::FlatWrite32((r31 + 16), r7);
    r31 += 16;
    goto loc_''' + NAMES[2][len('rr_kamek_'):] + ''';
}
'''
        else:
            prefix = '[[maybe_unused]] loc_80830D7C:\n{ goto loc_' + NAMES[1][len('rr_kamek_'):] + '; }\n'
        body = body[:start]+prefix+hook_body(sources[hook], cont)+'\n'+body[stop:]

    # Add the real continuation before the existing single epilogue. Its
    # final update is identical to loc_8083102C, which is already local.
    continuation = sources['rr_continue_80830F18']
    tail = continuation[continuation.index('    goto loc_80830F18;'):]
    ending = tail.index('    r3 = MemoryInline::FlatRead32(r30);', tail.index('loc_8083101C:'))
    tail = tail[:ending]+'    goto loc_8083102C;\n}\n'
    at = body.index('[[maybe_unused]] loc_8083102C:')
    body = body[:at]+tail+'\n'+body[at:]

    # Union of local registers/temporaries from the stitched blocks. Do not
    # reload these from a partially published CpuContext at an internal branch.
    declarations = []
    for name in NAMES[1:]:
        f = sources[name]
        prefix = f[f.index('{')+1:f.index('    goto loc_')]
        for declaration in re.findall(r'^    uint32_t (\w+) = ([^;]+);', prefix, re.M):
            var, value = declaration
            if not re.search(r'uint32_t '+var+r'\b', body) and var not in [v for v,_ in declarations]:
                declarations.append((var,value))
    at = body.index('    goto loc_80830D00;')
    body = body[:at]+MARKER+'\n'+''.join('    uint32_t '+v+' = '+value+';\n' for v,value in declarations)+body[at:]
    registers = sorted(set(map(int,re.findall(r'^    uint32_t r(\d+) =',body,re.M))))
    locals_list = ', '.join('{'+str(i)+', r'+str(i)+'}' for i in registers)
    call = re.compile(r'^(    )(InvokeIndirectCpu\(ctr, ctx\);|MKW_STATIC_TRANSLATED_CALL\((0x[0-9A-F]+u), \w+, ctx\);)$',re.M)
    def returning_call(m):
        target = m[3] or 'ctr'
        return ('    { kartpad::MenuModelCallBoundary boundary(ctx->gpr, {'+locals_list+'}, '+target+', ctx->lr); '+m[2]+' }')
    body, count = call.subn(returning_call,body)
    assert count == 7, count
    # These registers now change locally in the stitched hooks. Publish their
    # final values at the one normal return, as the standalone chunks did.
    at = body.rindex('    ctx->cr = cr;')
    body = body[:at]+''.join('    ctx->gpr['+str(i)+'] = r'+str(i)+';\n' for i in [5,11,12])+ '    ctx->ctr = ctr;\n'+body[at:]
    assert body.count('r1 = (r1 + 32);') == 1
    for name in NAMES[1:3]:
        assert 'InvokeDirectCpu<0x' + name[len('rr_kamek_'):] + 'u>' not in body
    assert 'switch (ctx->lr)' not in body
    assert body.count('loc_80831038:') == 1
    result = s[:a]+body+s[e:]
    include = '#include "menu_model_call_boundary.h"\n'+''.join(
        'extern "C" void '+name+'(CpuContext*);\n' for name in
        ['func_807DC2D8_retro_rewind','func_807DC2C0_retro_rewind','func_807DC3E8'])
    if include not in result:
        at = result.index('#line 1 ')
        result = result[:at]+include+result[at:]
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shards', type=Path)
    parser.add_argument('--archive', type=Path)
    args = parser.parse_args()
    sources, containing = {}, None
    files = sorted((args.shards/'retro_mod').glob('*.cpp'))
    for p in files:
        s = p.read_text()
        if 'extern "C" void '+NAMES[0]+'(' in s:
            near, far = hook_addresses(function(s, NAMES[0])[2])
            NAMES[1], NAMES[2] = 'rr_kamek_'+near, 'rr_kamek_'+far
    for p in files:
        s = p.read_text()
        for name in NAMES:
            if 'extern "C" void '+name+'(' in s:
                sources[name] = function(s,name)[2]
                if name == NAMES[0]: containing = p
    if not sources:
        print('No RR menu-model overlay in this graph'); return
    assert len(sources) == len(NAMES)
    raw = containing.read_text()
    updated = transform(raw, sources)
    if updated == raw:
        print('RR menu-model branch graph already stitched'); return
    if args.archive:
        dest = args.archive/containing.relative_to(args.shards)
        dest.parent.mkdir(parents=True, exist_ok=True)
        assert not dest.exists()
        dest.write_text(raw)
    containing.write_text(updated, newline='\n')
    assert transform(updated,sources) == updated
    print(json.dumps(dict(path=str(containing),sha256=hashlib.sha256(updated.encode()).hexdigest())))

if __name__ == '__main__': main()
