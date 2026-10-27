#!/usr/bin/env python3
"""Let clang inline small translated functions into callers in other shards.

A static translated call (MKW_STATIC_TRANSLATED_CALL, or InvokeDirectCpu through a known trait)
to a function defined in another shard is a real call: the callee's host prologue/epilogue, and
the caller's CpuContext flush/reload around it that the compiler cannot see through. For a small
callee those dominate its cost (profiles: 20-60% of such functions' samples are frame code). Each
shard calling one gets an `extern "C" inline` definition of it, copied from its own shard, ahead
of its first function. Same name, same body and source lines: clang's profile-guided inliner
inlines it at the hot call sites, the copy carries the callee's own sample profile, and the
original shard's definition stays the one the linker keeps (registration, indirect calls).

A candidate has at most --max-lines lines (--hot-max-lines if it is one of the --hot-functions
first translated functions of --hot-order, the build's section order file: the profile's hottest
by samples per byte, i.e. the most frequently called for their size, where the call costs most),
makes no call except static translated calls (whose
targets get a declaration ahead of the copy) and no ctx->lr dispatch, has no static or
thread_local state or call boundary from another pass (their headers are only in that shard), does
not call itself, and neither is nor calls a native-kernel --wrap target
(switch/cmake/GraphicsLink.cmake): a copied body or call would bypass the wrapper. Functions of
the generated native regions are not copied either (their generators read them from the shards by
name). Idempotent;
lines of existing functions are unchanged (PGO line offsets hold).
"""
from pathlib import Path
import argparse
import hashlib
import json
import re

MARK = '// KARTPAD_LEAF_INLINE'
LINE1 = re.compile(r'^#line 1 "(\w+)\.cpp"\r?$', re.M)
SIG = re.compile(r'^extern "C" void (\w+)\(CpuContext\* MKW_RESTRICT ctx\)\r?$', re.M)
STATIC_CALL = re.compile(r'MKW_STATIC_TRANSLATED_CALL\(0x[0-9A-F]{8}u, (\w+), ctx\);')
CALL = re.compile(r'MKW_STATIC_TRANSLATED_CALL\(0x[0-9A-F]{8}u, (\w+), ctx\)|InvokeDirectCpu<0x([0-9A-F]{8})u>\(ctx\)')
NOT_SIMPLE = re.compile(r'MKW_STATIC_TRANSLATED_CALL|InvokeDirectCpu|InvokeIndirect|\bfunc_[0-9A-F]{8}\w*\(|\brr_\w+\(|'
                        r'ctx->lr != |TranslatedFunctionRegistry|KARTPAD_LR_SKIP|\bstatic\b|\bthread_local\b|'
                        r'state_free_result|\basm\b|__asm__|kartpad::|Boundary')  # other passes' boundaries need their headers
TRAIT = re.compile(r'^MKW_TRANSLATED_TRAIT\(([0-9A-F]{8}), (\w+), 0x[0-9A-F]{8}u\);', re.M)


def wrap_symbols(repo):
    p = repo / 'switch/cmake/GraphicsLink.cmake'
    return set(re.findall(r'\bfunc_[0-9A-F]{8}\b', p.read_text(encoding='utf-8'))) if p.is_file() else None


def native_region_functions(builder):
    """Functions generate-native-model.py / generate-native-scene.py read from the shards by name;
    they expect exactly one definition, so they are not copied."""
    import importlib.util
    names = set()
    for script in ('generate-native-model.py', 'generate-native-scene.py'):
        spec = importlib.util.spec_from_file_location(script.replace('-', '_')[:-3], builder / script)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        names |= {'func_' + a.upper() for a in module.REGION}
    return names


def functions(text):
    """{name: (extern-"C" line number in its file, definition text)}."""
    out = {}
    marks = list(LINE1.finditer(text))
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        chunk = text[m.start():end]
        s = SIG.search(chunk)
        if not s or s.group(1) != m.group(1):
            continue
        guard = chunk.find('\n// RECOMP_GUEST_ABI')
        body = chunk[s.start():guard if guard >= 0 else len(chunk)].rstrip()
        line = chunk[:s.start()].count('\n')  # `#line 1` numbers the line after it 1
        out[s.group(1)] = (line, body)
    return out


def hot_functions(order, count):
    """The first `count` translated functions of a builder/section-order.py order file."""
    names = []
    for name in re.findall(r' \.text\.(func_[0-9A-F]{8}\w*) ', order.read_text(encoding='utf-8')):
        if name not in names:
            names.append(name)
    return set(names[:count])


def candidates(texts, wraps, max_lines, hot=frozenset(), hot_max_lines=0):
    """{name: (line, body, static callees)} and {name: defining path}."""
    owners, defs = {}, {}
    for path, text in texts.items():
        for name, (line, body) in functions(text).items():
            owners[name] = path
            defs[name] = (line, body)
    cands = {}
    for name, (line, body) in defs.items():
        limit = max(max_lines, hot_max_lines) if name in hot else max_lines
        if name in wraps or not re.match(r'^func_[0-9A-F]{8}', name) or body.count('\n') > limit:
            continue
        inner = body[body.index('{'):]
        callees = sorted(set(STATIC_CALL.findall(inner)))
        if NOT_SIMPLE.search(STATIC_CALL.sub('', inner)) or name in callees or wraps.intersection(callees):
            continue
        cands[name] = (line, body, callees)
    return cands, owners


def transform(text, path, cands, owners, traits):
    if MARK in text or 'MKW_STATIC_TRANSLATED_CALL' not in text:
        return text, []
    by_address = {a: n for a, n in traits}
    wanted = []
    for m in CALL.finditer(text):
        name = m.group(1) or by_address.get(m.group(2))
        if name in cands and owners.get(name) != path and name not in wanted:
            wanted.append(name)
    if not wanted:
        return text, []
    at = text.index('\n', text.index('#define MKW_STATIC_TRANSLATED_CALL')) + 1
    block = [MARK + ': copies of small callees from other shards, for inlining (builder/inline-leaf-calls.py)\n',
             '#include "ppc_runtime.h"\n', '#include "memory.h"\n', '#include "recomp_mod_loader.h"\n']
    declared = sorted({c for name in wanted for c in cands[name][2]})
    block += [f'extern "C" void {c}(CpuContext* ctx);\n' for c in declared]
    for name in sorted(wanted):
        line, body, _ = cands[name]
        sig = f'extern "C" void {name}(CpuContext* MKW_RESTRICT ctx)'
        copy = body.replace(sig, f'extern "C" inline void {name}(CpuContext* MKW_RESTRICT ctx)', 1)
        block.append(f'#line {line} "{name}.cpp"\n{copy}\n')
    return text[:at] + ''.join(block) + text[at:], wanted


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shards', type=Path)
    # 60: calls from hot code to small callees -22%, hot code +1.2%; 120 for every callee removed
    # twice the calls for +8.5% hot code (rr-e5 vs rr-e5b). 120 for the 300 hottest only (k1-rr-2cup,
    # offline): 83% of the hot calls 120 everywhere covers, with 57% of its copied code.
    parser.add_argument('--max-lines', type=int, default=60)
    parser.add_argument('--hot-order', type=Path)
    parser.add_argument('--hot-functions', type=int, default=300)
    parser.add_argument('--hot-max-lines', type=int, default=120)
    parser.add_argument('--archive', type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    wraps = wrap_symbols(repo)
    if wraps is None:
        print('inline-leaf-calls: no switch/cmake/GraphicsLink.cmake (wrap list); nothing done'); return
    wraps |= native_region_functions(Path(__file__).resolve().parent)
    paths = sorted(args.shards.rglob('shard_*.cpp'))
    texts = {p: p.read_bytes().decode('utf-8') for p in paths}
    hot = hot_functions(args.hot_order, args.hot_functions) if args.hot_order else frozenset()
    cands, owners = candidates(texts, wraps, args.max_lines, hot, args.hot_max_lines)
    rows = []
    for p in paths:
        traits_h = p.with_name(p.stem + '_traits.h')
        traits = TRAIT.findall(traits_h.read_text(encoding='utf-8')) if traits_h.is_file() else []
        new, wanted = transform(texts[p], p, cands, owners, traits)
        if not wanted:
            continue
        assert transform(new, p, cands, owners, traits)[1] == []
        if args.archive:
            dest = args.archive / p.relative_to(args.shards)
            dest.parent.mkdir(parents=True, exist_ok=True)
            assert not dest.exists()
            dest.write_bytes(texts[p].encode('utf-8'))
        data = new.encode('utf-8')
        p.write_bytes(data)
        rows.append(dict(path=str(p), copies=len(wanted), sha256=hashlib.sha256(data).hexdigest()))
    print(json.dumps(dict(candidates=len(cands), hot=len(hot), shards=len(rows),
                          copies=sum(r['copies'] for r in rows)), indent=2))


if __name__ == '__main__':
    main()
