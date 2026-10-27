from pathlib import Path
import argparse
import hashlib
import json
import re
import struct

REL_BASE = 0x805102E0  # StaticR.rel's load address on RMCP01 (ref/kartpad-main/tools/mkwii-rmcp01-*.yml)
MARK = '// KARTPAD_LR_SKIP'
HEAD = re.compile(r'^extern "C" void (\w+)\(CpuContext\* MKW_RESTRICT ctx\)\r?\n\{', re.M)
LABEL = re.compile(r'\bloc_([0-9A-F]{8}):')
RELOAD = re.compile(r'^    (?:[rf]\d+ = ctx->(?:gpr|fpr)\[\d+\]|cr = ctx->cr|ctr = ctx->ctr|xer = ctx->xer);'
                    r'(?: if \(ctx->lr == 0x[0-9A-F]{8}u\) \{ .*?goto loc_[0-9A-F]{8}; \} // KARTPAD_LR_SKIP)*\r?$')


def slot(base, disp):
    """A 32-bit guest access of base + disp, in each spelling (before and after flat-guest-displacement.py)."""
    if disp == 0:
        return r'(?:\w+At\(%s, 0\)|\w+\(%s\)|\w+\(guest_range_\d+, \d+u, %s\))' % (base, base, base)
    return (r'(?:\w+At\(%s, %d\)|\w+\(\(%s \+ %d\)\)|\w+\(guest_range_\d+, \d+u, \(%s \+ %d\)\))'
            % (base, disp, base, disp, base, disp))


def functions(text):
    heads = list(HEAD.finditer(text))
    for i, m in enumerate(heads):
        end = heads[i + 1].start() if i + 1 < len(heads) else len(text)
        guard = text.find('\n// RECOMP_GUEST_ABI', m.end(), end)
        yield m.group(1), m.start(), guard if guard >= 0 else end


def skip_offsets(body):
    """N for every `ctx->lr = rX` whose rX holds this function's incoming LR plus N."""
    found = set()
    for m in re.finditer(r'^    (r\d+) = \(\1 \+ (\d+)\);\r?\n    ctx->lr = \1;', body, re.M):
        reg, before, n = m.group(1), body[:m.start()], int(m.group(2))
        origin = re.findall(r'^    %s = (.+?);\r?$' % reg, before, re.M)
        if not origin:
            continue
        if origin[-1] == 'ctx->lr':
            found.add(n)
            continue
        # Reloaded from the LR save slot: that slot was stored from a copy of ctx->lr.
        k = re.fullmatch(r'MemoryInline::\w*Read\w*32\w*\((?:r1, (\d+)|\(r1 \+ (\d+)\)|guest_range_\d+, \d+u, \(r1 \+ (\d+)\))\)',
                         origin[-1])
        if not k:
            continue
        disp = int(next(g for g in k.groups() if g))
        for s in re.findall(r'^    (r\d+) = ctx->lr;', before, re.M):
            if re.search(r'Write\w*32\w*\((?:r1, %d|\(r1 \+ %d\)|guest_range_\d+, \d+u, \(r1 \+ %d\)), %s\)'
                         % (disp, disp, disp, s), before):
                found.add(n)
                break
    return found


def decode(word):
    op = word >> 26
    if op == 32:  # lwz rD, d(rA)
        d = word & 0xFFFF
        return 'lwz', (word >> 21) & 31, (word >> 16) & 31, d - 0x10000 if d & 0x8000 else d
    if word & 0xFC1FFFFF == 0x7C0803A6:  # mtlr rS
        return 'mtlr', (word >> 21) & 31, None, None
    if op == 14:  # addi rD, rA, simm
        d = word & 0xFFFF
        return 'addi', (word >> 21) & 31, (word >> 16) & 31, d - 0x10000 if d & 0x8000 else d
    return None, None, None, None


def statement_pattern(insn):
    kind, a, b, d = insn
    if kind == 'lwz':
        return re.compile(r'^    r%d = MemoryInline::%s;\r?$' % (a, slot('r%d' % b, d)), re.M)
    if kind == 'mtlr':
        return re.compile(r'^    ctx->lr = r%d;\r?$' % a, re.M)
    if kind == 'addi':
        return re.compile(r'^    r%d = \(r%d \+ %d\);\r?$' % (a, b, d), re.M)
    return None


def place_label(body, target, rel):
    """Label guest instruction `target`'s statement in its block: (body, setup for the jump) or a reason."""
    labels = sorted(int(x, 16) for x in LABEL.findall(body))
    start = max((l for l in labels if l < target), default=None)
    if start is None:
        return 'no block before the target'
    a = body.index('loc_%08X:' % start)
    nxt = body.find('[[maybe_unused]] loc_', a + 1)
    block_end = nxt if nxt >= 0 else len(body)
    block = body[a:block_end]
    off = target - REL_BASE
    if not rel or not 0 <= off <= len(rel) - 4:
        return 'target outside StaticR'
    insn = decode(struct.unpack('>I', rel[off:off + 4])[0])
    pattern = statement_pattern(insn)
    if pattern is None:
        return 'unhandled instruction at the target'
    hits = list(pattern.finditer(block))
    if len(hits) != 1:
        return f'{insn[0]} statement matches {len(hits)} times in its block'
    at = a + hits[0].start()
    skipped, rest = body[a:at], body[at:block_end]
    if re.search(r'^    (?:const |uint\d+_t |uint8_t\* |auto |PPC_FPR )', skipped, re.M):
        return 'the label would jump over a declaration'
    setup = ''
    for var in sorted(set(re.findall(r'\b(guest_range_\d+|r\d+_\w+?_\d+)\b', rest))):
        assigns = re.findall(r'^    %s = (.+);\r?$' % var, skipped, re.M)
        if not assigns:
            continue
        if not var.startswith('guest_range_') or not assigns[-1].startswith('MemoryInline::ResolveRangeHost('):
            return f'{var} is computed by a skipped statement'
        # Pure address resolution of the frame; r1 is the same at the call and in this epilogue.
        setup += f'{var} = {assigns[-1]}; '
    return body[:at] + '    loc_%08X: ' % target + body[at + 4:], setup


def reload_end(body, pos):
    """End of the last register reload line after the call line ending at pos (blank lines between
    reloads are the translator's padding for registers it did not reload)."""
    end = pos
    i = body.find('\n', pos)
    while i >= 0:
        j = body.find('\n', i + 1)
        line = body[i + 1:j if j >= 0 else len(body)].rstrip('\r')
        if RELOAD.match(line):
            end = i + 1 + len(line)
        elif line.strip():
            break
        i = j
    return end


def adjusters_of(texts):
    found = {}
    for text in texts.values():
        for name, a, b in functions(text):
            m = re.fullmatch(r'rr_kamek_([0-9A-F]{8})', name)
            if m:
                for n in skip_offsets(text[a:b]):
                    found.setdefault(m.group(1), set()).add(n)
    return found


def transform(texts, rel):
    """texts: {path: text}. Returns ({path: new text}, report rows)."""
    adjusters = adjusters_of(texts)
    rows, out = [], dict(texts)
    for path in sorted(texts):
        text = out[path]
        edits = []
        for name, a, b in functions(text):
            body = changed = text[a:b]
            for addr, offsets in sorted(adjusters.items()):
                call = re.compile(r'^    (?:InvokeDirectCpu<0x%su>\(ctx\);|MKW_STATIC_TRANSLATED_CALL\(0x%su, \w+, ctx\);)\r?$'
                                  % (addr, addr), re.M)
                search = 0
                while True:
                    m = call.search(changed, search)
                    if not m:
                        break
                    search = m.end()
                    lrs = re.findall(r'ctx->lr = 0x([0-9A-F]{8})u;', changed[:m.start()])
                    if not lrs:
                        continue
                    ret = int(lrs[-1], 16)
                    block = changed[m.end():]
                    stop = block.find('[[maybe_unused]] loc_')
                    if ('if (ctx->lr != 0x%08Xu)' % ret) in block[:stop if stop >= 0 else len(block)]:
                        continue  # the translator already resumes this call through ctx->lr
                    eol = reload_end(changed, m.end() - (1 if changed[m.end() - 1] == '\r' else 0))
                    last_line = changed[changed.rfind('\n', 0, eol) + 1:eol]
                    for n in sorted(offsets):
                        target = ret + n
                        if 'goto loc_%08X; } %s' % (target, MARK) in last_line:
                            continue  # already resumed
                        setup = ''
                        if 'loc_%08X:' % target not in changed:
                            placed = place_label(changed, target, rel)
                            if isinstance(placed, str):
                                rows.append(dict(function=name, call=f'0x{ret - 4:08X}', helper=f'0x{addr}',
                                                 target=f'0x{target:08X}', result='not resumed: ' + placed))
                                continue
                            labelled, setup = placed
                            if labelled.find('loc_%08X: ' % target) < eol:
                                eol += len(labelled) - len(changed)
                            changed = labelled
                        jump = ' if (ctx->lr == 0x%08Xu) { %sgoto loc_%08X; } %s' % (target, setup, target, MARK)
                        changed = changed[:eol] + jump + changed[eol:]
                        eol += len(jump)
                        search = eol
                        rows.append(dict(function=name, call=f'0x{ret - 4:08X}', helper=f'0x{addr}',
                                         target=f'0x{target:08X}', result='resumed'))
            if changed != body:
                assert changed.count('\n') == body.count('\n')
                edits.append((a, b, changed))
        for a, b, changed in reversed(edits):
            text = text[:a] + changed + text[b:]
        out[path] = text
    return out, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shards', type=Path)
    parser.add_argument('--rel', type=Path, help='StaticR.rel (default: the staged disc above the shards)')
    parser.add_argument('--archive', type=Path)
    args = parser.parse_args()
    if not any((args.shards / 'retro_mod').glob('*.cpp')):
        print('No Retro Rewind module in this graph'); return
    rel_path = args.rel
    if rel_path is None:
        for up in args.shards.resolve().parents:
            if (up / 'disc/files/rel/StaticR.rel').is_file():
                rel_path = up / 'disc/files/rel/StaticR.rel'
                break
    rel = rel_path.read_bytes() if rel_path and rel_path.is_file() else b''
    paths = sorted(args.shards.rglob('*.cpp'))
    texts = {p: p.read_bytes().decode('utf-8') for p in paths}
    out, rows = transform(texts, rel)
    files = []
    for p in paths:
        if out[p] == texts[p]:
            continue
        if args.archive:
            dest = args.archive / p.relative_to(args.shards)
            dest.parent.mkdir(parents=True, exist_ok=True)
            assert not dest.exists()
            dest.write_bytes(texts[p].encode('utf-8'))
        data = out[p].encode('utf-8')
        p.write_bytes(data)
        files.append(dict(path=str(p), sha256=hashlib.sha256(data).hexdigest()))
    assert transform(out, rel)[0] == out, 'not idempotent'
    print(json.dumps(dict(staticr=str(rel_path) if rel else None, sites=rows, files=files), indent=2))


if __name__ == '__main__':
    main()
