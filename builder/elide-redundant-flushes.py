#!/usr/bin/env python3
"""Blank GPR flush stores that write the value ctx already holds (b73, docs/PERF-B73-2026-10-14.md).

    python builder/elide-redundant-flushes.py <build_shards> --exemptions <nonvolatile_reload_exemptions.json>

Why: since b73 the translator no longer reloads a nonvolatile GPR after a call that provably returns it
unchanged (GuestNonvolatilePreservationAnalyzer). The old reload also told clang that ctx->gpr[N] equals
rN, which let it delete the flush store before the next call. Without the reload that store comes back
(hot shards grew 1-2%). This pass restores the knowledge at source level: a forward must-dataflow per
translated function tracks which GPR locals equal their ctx slot, and replaces a flush
`ctx->gpr[N] = rN;` with a blank line when N is already in sync on every path. Blank lines keep every
line number, so the sample PGO profile still maps.

A local is in sync after its entry load, a reload or a flush, and across a call that preserves it
(same exemption table and EABI assumption the reload elision uses). Any write to the local, any
other write to ctx->gpr[N], and any statement that passes ctx to an unrecognized function drop it.
A function with a construct the pass does not model is left untouched. Idempotent. Set
KARTPAD_FLUSH_ELISION=0 to skip.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import sys

ALL = (1 << 32) - 1
EABI_PRESERVED = 0xFFFFE006  # r1, r2, r13-r31 (GuestNonvolatilePreservationAnalyzer.EabiPreservedGprMask)

FUNC_HEAD = re.compile(r'^(?:extern "C" )?(?:static )?(?:inline )?void (\w+)\(CpuContext\* MKW_RESTRICT ctx\)$')
FLUSH = re.compile(r'^ctx->gpr\[(\d+)\] = r(\d+);$')
RELOAD = re.compile(r'^(?:uint32_t )?r(\d+) = ctx->gpr\[(\d+)\];$')
STATIC_CALL = re.compile(r'^MKW_STATIC_TRANSLATED_CALL\(0x([0-9A-Fa-f]+)u, \w+, ctx\);$')
DIRECT_CALL = re.compile(r'^InvokeDirectCpu<0x([0-9A-Fa-f]+)u>\(ctx\);$')
INDIRECT_CALL = re.compile(r'^InvokeIndirectCpu\((?:r\d+|ctr|ctx->lr|0x[0-9A-Fa-f]+u), ctx\);$')
INDIRECT_JUMP = re.compile(r'^InvokeIndirectJump\((?:r\d+|ctr|ctx->lr|0x[0-9A-Fa-f]+u), ctx\);$')
LABEL = re.compile(r'^(?:\[\[maybe_unused\]\] )?(\w+):$')
CASE = re.compile(r'^(?:case [^:]+|default):$')
GOTO = re.compile(r'^goto (\w+);$')
IF_OPEN = re.compile(r'^if \(.*\) \{$')
SWITCH_OPEN = re.compile(r'^switch \(.*\) \{$')
# Assignment or compound assignment to a GPR local anywhere in a statement.
ASSIGN = re.compile(r'(?<![\w.>])r(\d+)\s*(?:<<|>>|[-+*/%&|^])?=(?!=)')
INCDEC = re.compile(r'(?<![\w.>])r(\d+)\s*(?:\+\+|--)|(?:\+\+|--)\s*r(\d+)(?!\w)')
BARE_ARG = re.compile(r'(?<![\w.>])r(\d+)(?!\w)')
ADDR_OF = re.compile(r'(?:^|[(,=!?:])\s*&\s*r\d+(?!\w)')
REF_DECL = re.compile(r'\b\w+\s*&&?\s*\w+\s*=(?!=)|\[&|\[=')
# ctx uses that cannot change ctx->gpr: field reads/writes other than gpr, and the psq helpers, which
# only read ctx->gqr (runtime/include/isa/ppc_isa_quantized.h).
CTX_FIELD = re.compile(r'ctx->(?:lr|ctr|cr|xer|msr|srr0|srr1|fpscr|gqr\[\d+\]|fpr\[\d+\](?:\.\w+)?)')
CTX_GPR = re.compile(r'ctx->gpr\[(\d+)\]')
PSQ_CTX = re.compile(r'\bPPC_Psq\w*Inline(?:<[^<>]*>)?\(ctx,')
# Callees verified to take every argument by value.
BY_VALUE = re.compile(
    r'^(?:static_cast|reinterpret_cast|MemoryInline::(?:Flat|Read|Write|Resolve)\w*|func_\w+_statefree(?:_v\d+)?|'
    r'PPC_Psq\w*Inline|PPC_WriteSpr|PPC_TrapWord|PPC_Store(?:Halfword|Word)ByteReverse|PPC_CntlzwInline|'
    r'PpcRotl32Inline|KPad_RkgFixture_Force\w+|InvokeIndirectCpu|InvokeIndirectJump)$')


class Unsupported(Exception):
    pass


def enclosing_callee(s: str, pos: int) -> str | None:
    """Name of the function whose argument list contains position pos; '' for a grouping paren."""
    depth = 0
    i = pos - 1
    while i >= 0:
        ch = s[i]
        if ch == ')':
            depth += 1
        elif ch == '(':
            if depth == 0:
                break
            depth -= 1
        i -= 1
    if i < 0:
        return None
    j = i - 1
    while j >= 0 and s[j] == ' ':
        j -= 1
    if j >= 0 and s[j] == '>':
        depth = 0
        while j >= 0:
            if s[j] == '>':
                depth += 1
            elif s[j] == '<':
                depth -= 1
                if depth == 0:
                    break
            j -= 1
        j -= 1
    end = j + 1
    while j >= 0 and (s[j].isalnum() or s[j] in '_:'):
        j -= 1
    return s[j + 1:end]


def bit(n: int) -> int:
    return 1 << n


def local_writes(t: str) -> int:
    """GPR locals a statement may write."""
    if ADDR_OF.search(t) or REF_DECL.search(t):
        raise Unsupported('address or reference of a local')
    mask = 0
    for m in ASSIGN.finditer(t):
        mask |= bit(int(m.group(1)))
    for m in INCDEC.finditer(t):
        mask |= bit(int(m.group(1) or m.group(2)))
    for m in BARE_ARG.finditer(t):
        a, b = m.start(), m.end()
        before = t[:a].rstrip()
        after = t[b:].lstrip()
        if before.endswith(('(', ',')) and after.startswith((',', ')')):
            name = enclosing_callee(t, a)
            if name is None:
                raise Unsupported('unbalanced parentheses')
            if name and not BY_VALUE.match(name):
                mask |= bit(int(m.group(1)))
    return mask


def statement_effect(t: str, exemptions: dict[int, int]):
    """('flush', n) | ('reload', n) | ('call', preserved) | ('clear',) | ('kill', mask)."""
    m = FLUSH.match(t)
    if m and m.group(1) == m.group(2):
        return ('flush', int(m.group(1)))
    m = RELOAD.match(t)
    if m and m.group(1) == m.group(2):
        return ('reload', int(m.group(1)))
    m = STATIC_CALL.match(t) or DIRECT_CALL.match(t)
    if m:
        return ('call', exemptions.get(int(m.group(1), 16), 0))
    if INDIRECT_CALL.match(t):
        return ('call', EABI_PRESERVED)
    if INDIRECT_JUMP.match(t):
        return ('call', 0)
    kill = local_writes(t)
    if 'ctx' in t:
        rest = PSQ_CTX.sub('', t)
        rest = CTX_FIELD.sub('', rest)
        lvalue = re.match(r'^ctx->gpr\[(\d+)\]\s*=(?!=)', rest)
        if lvalue:
            kill |= bit(int(lvalue.group(1)))
            rest = rest[lvalue.end():]
        rest = CTX_GPR.sub('', rest)  # remaining ctx->gpr[N] uses are reads
        if re.search(r'\bctx\b', rest):
            return ('clear',)
    return ('kill', kill)


def analyze(body: list[str], exemptions: dict[int, int]) -> list[int]:
    """Line indices (into body) of redundant flushes. Raises Unsupported."""
    n = len(body)
    kinds = [None] * n
    succ: list[list[int]] = [[] for _ in range(n)]
    labels: dict[str, int] = {}
    stack: list[list] = []
    pending_gotos: list[tuple[int, str]] = []
    pending_breaks: list[tuple[int, list]] = []
    for i, raw in enumerate(body):
        t = raw.strip()
        if '/*' in t or 'R"' in t:
            raise Unsupported('block comment or raw string')
        if not t or t.startswith('//') or t.startswith('#'):
            kinds[i] = ('kill', 0)
            succ[i].append(i + 1)
            continue
        m = LABEL.match(t)
        if m and not t.startswith(('case ', 'default')):
            if m.group(1) in labels:
                raise Unsupported('duplicate label')
            labels[m.group(1)] = i
            kinds[i] = ('kill', 0)
            succ[i].append(i + 1)
            continue
        if CASE.match(t):
            sw = next((f for f in reversed(stack) if f[0] == 'switch'), None)
            if sw is None or stack[-1] is not sw:
                raise Unsupported('case outside a switch body')
            sw[2].append(i)
            if t.startswith('default'):
                sw[3] = True
            kinds[i] = ('kill', 0)
            succ[i].append(i + 1)
            continue
        if t == '{':
            stack.append(['block', i])
            kinds[i] = ('kill', 0)
            succ[i].append(i + 1)
            continue
        if t == '}':
            if not stack:
                raise Unsupported('unbalanced braces')
            frame = stack.pop()
            if frame[0] == 'if':
                succ[frame[1]].append(i)  # condition false: skip the then-block
            elif frame[0] == 'else':
                succ[frame[1]].append(i)  # end of the then-block jumps over the else-block
            elif frame[0] == 'switch':
                for case in frame[2]:
                    succ[frame[1]].append(case)
                if not frame[3]:
                    succ[frame[1]].append(i)
                frame[4].append(i)
            kinds[i] = ('kill', 0)
            succ[i].append(i + 1)
            continue
        if t == '} else {':
            if not stack or stack[-1][0] != 'if':
                raise Unsupported('else without if')
            frame = stack.pop()
            succ[frame[1]].append(i + 1)  # condition false: enter the else-block
            stack.append(['else', i])
            kinds[i] = ('kill', 0)
            continue  # successor (end of the else-block) is added when it closes
        if IF_OPEN.match(t):
            kinds[i] = statement_effect(t[:-1].strip(), exemptions)
            if kinds[i][0] in ('flush', 'reload', 'call'):
                raise Unsupported('sync inside a condition')
            stack.append(['if', i])
            succ[i].append(i + 1)
            continue
        if SWITCH_OPEN.match(t):
            kinds[i] = statement_effect(t[:-1].strip(), exemptions)
            if kinds[i][0] in ('flush', 'reload', 'call'):
                raise Unsupported('sync inside a switch')
            stack.append(['switch', i, [], False, []])
            continue
        m = GOTO.match(t)
        if m:
            kinds[i] = ('kill', 0)
            pending_gotos.append((i, m.group(1)))
            continue
        if t == 'return;' or t.startswith('return '):
            kinds[i] = statement_effect(t, exemptions) if t != 'return;' else ('kill', 0)
            continue
        if t == 'break;':
            sw = next((f for f in reversed(stack) if f[0] == 'switch'), None)
            if sw is None:
                raise Unsupported('break outside a switch')
            kinds[i] = ('kill', 0)
            pending_breaks.append((i, sw[4]))
            continue
        if re.match(r'^(?:for|while|do|else)\b', t) or t.startswith('}') or t.endswith('{'):
            raise Unsupported('construct: ' + t[:40])
        if not t.endswith(';'):
            raise Unsupported('multi-line statement: ' + t[:40])
        kinds[i] = statement_effect(t, exemptions)
        succ[i].append(i + 1)
    if stack:
        raise Unsupported('unbalanced braces at end')
    for i, name in pending_gotos:
        if name not in labels:
            raise Unsupported('goto to unknown label ' + name)
        succ[i].append(labels[name])
    for i, closes in pending_breaks:
        succ[i].append(closes[0])
    for s in succ:
        s[:] = [x for x in s if x < n]

    preds: list[list[int]] = [[] for _ in range(n)]
    for i, s in enumerate(succ):
        for x in s:
            preds[x].append(i)
    state_in = [ALL] * n
    state_in[0] = 0
    out = [ALL] * n

    def transfer(i: int, s: int) -> int:
        k = kinds[i]
        if k[0] == 'flush' or k[0] == 'reload':
            return s | bit(k[1])
        if k[0] == 'call':
            return s & k[1]
        if k[0] == 'clear':
            return 0
        return s & ~k[1]

    work = list(range(n - 1, -1, -1))
    queued = [True] * n
    while work:
        i = work.pop()
        queued[i] = False
        if i == 0:
            s = 0
        else:
            s = ALL
            for p in preds[i]:
                s &= out[p]
            if not preds[i]:
                s = ALL  # unreachable
        state_in[i] = s
        o = transfer(i, s)
        if o != out[i]:
            out[i] = o
            for x in succ[i]:
                if not queued[x]:
                    queued[x] = True
                    work.append(x)
    return [i for i in range(n) if kinds[i] and kinds[i][0] == 'flush' and state_in[i] & bit(kinds[i][1])
            and preds[i]]


def process_file(path: pathlib.Path, exemptions: dict[int, int], stats: dict, write: bool) -> None:
    raw = path.read_bytes()
    text = raw.decode('utf-8')
    lines = text.split('\n')
    changed = False
    i = 0
    n = len(lines)
    while i < n:
        m = FUNC_HEAD.match(lines[i].rstrip('\r'))
        if not m or i + 1 >= n or lines[i + 1].rstrip('\r') != '{':
            i += 1
            continue
        start = i + 2
        depth = 1
        j = start
        while j < n and depth:
            t = lines[j].strip()
            depth += t.count('{') - t.count('}')
            j += 1
        if depth:
            stats['unterminated'] += 1
            break
        body = [l.rstrip('\r') for l in lines[start:j - 1]]
        stats['functions'] += 1
        stats['flushes'] += sum(1 for l in body if FLUSH.match(l.strip()))
        try:
            redundant = analyze(body, exemptions)
        except Unsupported as error:
            stats['skipped'] += 1
            reason = str(error).split(':')[0]
            stats['reasons'][reason] = stats['reasons'].get(reason, 0) + 1
            i = j
            continue
        for k in redundant:
            line = lines[start + k]
            lines[start + k] = '\r' if line.endswith('\r') else ''
            changed = True
        stats['elided'] += len(redundant)
        i = j
    if changed and write:
        path.write_bytes('\n'.join(lines).encode('utf-8'))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('shards', type=pathlib.Path)
    parser.add_argument('--exemptions', type=pathlib.Path, required=True)
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    if os.environ.get('KARTPAD_FLUSH_ELISION') == '0':
        print('flush elision: disabled (KARTPAD_FLUSH_ELISION=0)')
        return 0
    if os.environ.get('KARTPAD_RELOAD_ELISION') == '0':
        print('flush elision: skipped, reload elision is off (KARTPAD_RELOAD_ELISION=0)')
        return 0
    document = json.loads(args.exemptions.read_text(encoding='utf-8'))
    if document.get('format') != 'kartpad-nonvolatile-reload-exemptions':
        raise SystemExit(f'{args.exemptions}: not a reload exemption table')
    exemptions = {int(k, 16): int(v) for k, v in document['entries'].items()}
    stats = {'functions': 0, 'skipped': 0, 'flushes': 0, 'elided': 0, 'unterminated': 0, 'reasons': {}}
    files = sorted(args.shards.rglob('*.cpp'))
    for path in files:
        process_file(path, exemptions, stats, not args.dry_run)
    print('flush elision: %d of %d GPR flush lines blanked in %d functions (%d files, %d functions skipped)' % (
        stats['elided'], stats['flushes'], stats['functions'], len(files), stats['skipped']))
    for reason, count in sorted(stats['reasons'].items(), key=lambda kv: -kv[1]):
        print('  skipped: %-40s %d' % (reason, count))
    if stats['unterminated']:
        raise SystemExit('flush elision: unterminated function body')
    return 0


if __name__ == '__main__':
    sys.exit(main())
