#!/usr/bin/env python3
"""Generate a PRIVATE pilot shard graph that captures the immutable flat base.

Input/output are shard directories, not checked-in game code. Originals remain
untouched. Captures the reservation base only; guest data, stores, addresses,
faults, call dispatch and instruction ordering remain on the original paths.
"""
import argparse
import json
import re
import shutil
from pathlib import Path

ACCESS = re.compile(r'MemoryInline::Flat(Read|Write(?:Ram)?)(8|16|32|Float32|Float64)\(')
FUNCTION = re.compile(r'extern "C" void func_([0-9A-Fa-f]{8})\([^\n]*\)\s*\n\{')
PILOT = {'80562888', '8006E6F0', '80074770', '8055D23C', '80068A80', '801277A8'}
NON_CODE = re.compile(r'//[^\n]*|/\*.*?\*/|R"([^ ()\\\t\r\n]{0,16})\(.*?\)\1"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.DOTALL)

def code_mask(text):
    # Preserve character offsets and line breaks while excluding braces and
    # helper-like text in literals/comments from the transformation.
    return NON_CODE.sub(lambda m: ''.join('\n' if c=='\n' else ' ' for c in m[0]), text)

def rewrite(text, addresses, read_only=False):
    mask = code_mask(text)
    functions = [m for m in FUNCTION.finditer(text) if mask[m.start():m.start()+6]=='extern']
    changed = []
    for index in range(len(functions)-1, -1, -1):
        match = functions[index]
        address = match[1].upper()
        if addresses is not None and address not in addresses: continue
        end = functions[index+1].start() if index+1 < len(functions) else len(text)
        # End the body at its own closing brace, so following declarations or
        # registration helpers are never part of the transformation.
        depth = 1
        body_start = match.end()
        body_end = None
        for pos in range(body_start, end):
            if mask[pos] == '{': depth += 1
            elif mask[pos] == '}':
                depth -= 1
                if depth == 0:
                    body_end = pos
                    break
        if body_end is None: raise ValueError('Unclosed generated function '+address)
        body = text[body_start:body_end]
        reads = [access for access in ACCESS.finditer(mask[body_start:body_end])
                 if not read_only or access[1] == 'Read']
        count = len(reads)
        if not count: continue
        for read in reversed(reads):
            body = body[:read.start()]+'mkw_captured_guest_memory.'+read[1]+read[2]+'('+body[read.end():]
        # No extra source line: original #line locations and sample-PGO line
        # offsets stay valid for the guest instructions.
        body = ' const MemoryInline::CapturedGuestMemory mkw_captured_guest_memory;'+body
        text = text[:body_start]+body+text[body_end:]
        changed.append({'address':address, 'reads':sum(r[1]=='Read' for r in reads),
                        'writes':sum(r[1]!='Read' for r in reads)})
    if changed:
        text = '#include "captured_guest_memory.h"\n'+text
    return text, changed

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('output', type=Path)
    selection = p.add_mutually_exclusive_group()
    selection.add_argument('--all', action='store_true', help='Experimental full-graph version')
    selection.add_argument('--functions', nargs='+', metavar='ADDRESS',
                           help='Explicit eight-digit guest function addresses')
    p.add_argument('--read-only', action='store_true', help='Leave every write on its original helper')
    args = p.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    if source == output or source in output.parents or output in source.parents:
        p.error('Output must be a separate directory outside the source graph')
    if output.exists(): p.error('Output exists; choose a fresh private output')
    addresses = None if args.all else PILOT
    if args.functions:
        if any(not re.fullmatch(r'[0-9A-Fa-f]{8}', address) for address in args.functions):
            p.error('Each function address must contain exactly eight hexadecimal digits')
        addresses = {address.upper() for address in args.functions}
    shutil.copytree(source, output)
    changes = []
    for path in sorted(output.rglob('*.cpp')):
        text = path.read_text(encoding='utf-8')
        text, modified = rewrite(text, addresses, args.read_only)
        if modified:
            path.write_text(text,encoding='utf-8')
            changes.extend({'file':str(path.relative_to(output)), **row} for row in modified)
    graph = output/'shards.cmake'
    text = graph.read_text(encoding='utf-8').replace(str(source).replace('\\','/'),str(output).replace('\\','/'))
    graph.write_text(text,encoding='utf-8')
    (output/'captured-base-receipt.json').write_text(json.dumps(changes,indent=2))
    print(f'{len(changes)} functions, {sum(row["reads"] for row in changes)} reads and '
          f'{sum(row["writes"] for row in changes)} writes at original sites; original graph preserved')

if __name__ == '__main__': main()
