#!/usr/bin/env python3
"""Reuse FPCC for an adjacent CR update in privately generated game code.

Only identical, pure register/lane operands qualify. No guest instruction,
memory operation, callback, mod guard or FP exception handling is removed.
Keep both original source lines so sample-PGO locations remain stable.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil

REGISTER = r'f\d+(?:_[a-z0-9]+)*\.d'
OPERAND = rf'(?:{REGISTER}|(?:PPC_PsToScalarInline|PpcGetPs0Inline|PpcGetPs1Inline)\({REGISTER}\))'
PAIR = re.compile(
    rf'(?m)^(?P<pad>[ \t]*)\(void\)PpcCompareStateInline\((?P<ordered>true|false), '
    rf'(?P<a>{OPERAND}), (?P<b>{OPERAND})\);\n'
    rf'(?P=pad)SetCRFloatResident\((?P<cr>cr|cached_cr|ctx->cr), (?P<field>[0-7]), (?P=a), (?P=b)\);$')
NON_CODE = re.compile(r'//[^\n]*|/\*.*?\*/|R"([^ ()\\\t\r\n]{0,16})\(.*?\)\1"|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.DOTALL)

def transform(text):
    if not PAIR.search(text):
        return text,0
    mask = NON_CODE.sub(lambda m: ''.join('\n' if c=='\n' else ' ' for c in m[0]), text)
    count = 0
    def replace(m):
        nonlocal count
        if mask[m.start():m.end()] != m[0]:
            return m[0]
        count += 1
        return (f'{m["pad"]}PpcCompareCrStateInline({m["cr"]}, {m["field"]}, '
                f'{m["ordered"]}, {m["a"]}, {m["b"]});\n')
    result=PAIR.sub(replace,text)
    assert result.count('\n')==text.count('\n')
    return result,count

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('translation',type=Path)
    parser.add_argument('--apply',action='store_true')
    parser.add_argument('--backup-dir',type=Path)
    args=parser.parse_args()
    root=args.translation.resolve()
    if not (root/'functions').is_dir() or not (root/'build_shards').is_dir():
        parser.error('Expected a local translation with functions and build_shards')
    runtime=Path(__file__).resolve().parents[1]/'build/wiicompiled-fpscr/runtime/include/ppc_runtime.h'
    if args.apply and 'inline void PpcCompareCrStateInline(' not in runtime.read_text():
        raise RuntimeError('Apply the authored runtime performance patch before rewriting compares')
    # Only shards are compiled directly. The native region generators also
    # consume their small explicit raw-function sets. Reading 30,000 unused
    # raw functions on every incremental build adds avoidable filesystem I/O.
    native=set()
    for name in ('model','scene'):
        spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name('generate-native-'+name+'.py'))
        module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
        native.update(module.REGION)
    changes=[];totals={'functions':0,'build_shards':0};existing=0
    for folder in totals:
        paths=((root/folder)/('func_'+a+'.cpp') for a in sorted(native)) if folder=='functions' else (root/folder).rglob('*.cpp')
        for path in sorted(paths):
            if not path.is_file():raise RuntimeError('Missing generated input '+str(path))
            raw=path.read_bytes();text=raw.decode();newline='\r\n' if '\r\n' in text else '\n'
            text=text.replace('\r\n','\n')
            result,count=transform(text)
            existing+=result.count('PpcCompareCrStateInline(')
            if not count:continue
            totals[folder]+=count
            replacement=result.replace('\n',newline).encode()
            row={'file':str(path.relative_to(root)).replace('\\','/'),'sites':count,
                 'before_sha256':hashlib.sha256(raw).hexdigest(),
                 'after_sha256':hashlib.sha256(replacement).hexdigest()}
            changes.append(row)
            if args.apply:
                if args.backup_dir:
                    backup=args.backup_dir/Path(row['file'])
                    if backup.exists():raise RuntimeError('Refusing to replace a prior rollback file: '+str(backup))
                    backup.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(path,backup)
                path.write_bytes(replacement)
    if args.apply and changes:
        (root/'fcmp-reuse-manifest.json').write_text(json.dumps({'changes':changes,'totals':totals},indent=2)+'\n')
    print(('Applied' if args.apply else 'Eligible')+f': {len(changes)} private source files; '+
          f'{totals["functions"]} function sites; {totals["build_shards"]} compiled shard sites; '+
          f'{existing} resulting reuse sites',flush=True)

if __name__=='__main__':main()
