#!/usr/bin/env python3
"""Compile the game-independent part of the game NROs into a toolkit (run on the developer machine).

  python aio/toolkit/make-prebuilt.py --toolkit aio/out/toolkit --data C:\\KPNX-dev
                                      [--bundle KartPadNX-Builder.data] [--shim nxshim.exe] [--jobs N]

The AIO builder then compiles only the translated game on the player's PC. For each profile
(base, retro-rewind) this configures the toolkit's switch/ project exactly as the AIO pipeline
does (kpnx_aio.cmake_configure_cmd), with the same pinned tools, but against a stub
translation, so no game code exists in the build at all. It builds the targets listed in
switch/cmake/Prebuilt.cmake (runtime, Switch shim, Aurora, deko3d renderer, uam, cryptopp,
pugixml and the FetchContent libraries) and writes into the toolkit:

  aio-prebuilt/libs/<profile>/prebuilt.cmake   per target: settings fingerprint, objects
  aio-prebuilt/libs/<profile>/<target>/*.o     the objects; a target with the same fingerprint
                                               in a later profile uses the first profile's copy
  aio-prebuilt/cmake/                         configure seed: compiler-check results and CMake's
                                               compiler identification (@TOOLS@ for the tools path)

Objects are stored without debug sections (they are not part of an NRO). An object that the
compiler's dependency file shows reading the stub translation or the build's generated/
headers is not prebuilt: the runtime's two RuntimeConfig.h users are compiled by the player as
part of the same target. The run fails if any static-library object depends on them or if
any object defines a func_8xxxxxxx symbol that is not one of the runtime's hand-written
PPC_NATIVE_OVERRIDE replacements (a translated function).

--data is an AIO work folder for the tools (installed from --bundle, or downloaded, as the
builder does). The toolkit's Aurora sources get builder/integrate-deko.py's edits, as on the
player's PC.
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

AIO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(AIO / 'pipeline'))
import kpnx_aio  # noqa: E402
from kpnx_aio import fwd  # noqa: E402

PROFILES = ('base', 'retro-rewind')
COMPILER_FILES = ('CMakeCCompiler.cmake', 'CMakeCXXCompiler.cmake', 'CMakeASMCompiler.cmake')
TRANSLATED_SYMBOL = re.compile(r'^func_8[0-9A-Fa-f]{7}$')
NATIVE_OVERRIDE = re.compile(r'PPC_NATIVE_OVERRIDE(?:_VOID)?\(\s*([0-9A-Fa-f]{8})\s*,')

STUB_RUNTIME_CONFIG = """// Stub for aio/toolkit/make-prebuilt.py: objects that include this are never prebuilt.
#pragma once
#include <cstdint>
namespace RuntimeConfig {
constexpr uint32_t SDA1_BASE = 0u;
constexpr uint32_t SDA2_BASE = 0u;
}
"""


def write_stub_translation(root, profile):
    """A translation with no game code: one empty shard per list FullApp.cmake requires."""
    if root.exists():
        shutil.rmtree(root)
    shards = root / 'build_shards'
    shards.mkdir(parents=True)
    stub = shards / 'stub.cpp'
    stub.write_text('// Stub for aio/toolkit/make-prebuilt.py; no game code.\n', encoding='utf-8')
    (root / 'RuntimeConfig.h').write_text(STUB_RUNTIME_CONFIG, encoding='utf-8')
    rr = profile == 'retro-rewind'
    lists = {'MKW_BASE_COMMON_SHARDS': [stub], 'MKW_BASE_PORTABLE_SENSITIVE_SHARDS': [],
             'MKW_RETRO_PORTABLE_SENSITIVE_SHARDS': [], 'MKW_RETRO_MOD_SHARDS': [stub] if rr else [],
             'MKW_BASE_REGISTRATION_SOURCES': [stub], 'MKW_RETRO_REGISTRATION_SOURCES': [stub] if rr else [],
             'MKW_RETRO_EXTRA_SOURCES': []}
    text = [f'set(MKW_TRANSLATED_SHARD_ROOT "{fwd(shards)}")', 'set(MKW_BASE_FUNCTION_COUNT 0)',
            'set(MKW_RETRO_REWIND_FUNCTION_COUNT 0)']
    for name, files in lists.items():
        text.append(f'set({name}\n' + ''.join(f'    "{fwd(f)}"\n' for f in files) + ')')
    text.append(f'set(MKW_HAVE_RETRO_REWIND_SHARDS {"ON" if rr else "OFF"})')
    (shards / 'shards.cmake').write_text('\n'.join(text) + '\n', encoding='utf-8')


# ------------------------------------------------------------------------------ ninja ---

def ninja_unescape(s):
    return re.sub(r'\$(.)', r'\1', s)


def ninja_split(s):
    """Split a build line's path list on unescaped spaces."""
    return [ninja_unescape(p) for p in re.split(r'(?<!\$) ', s) if p]


def parse_build_ninja(bdir):
    """(archive inputs per static target with their rule language, explicit inputs per output)."""
    archives, inputs = {}, {}
    text = (bdir / 'build.ninja').read_text(encoding='utf-8')
    text = text.replace('$\n', '')  # line continuations
    for line in text.splitlines():
        if not line.startswith('build '):
            continue
        m = re.match(r'build ((?:\$.|[^:$])+): (\S+)(.*)$', line)
        if not m:
            continue
        outs = ninja_split(m.group(1))
        explicit = re.split(r' \|\|? ', m.group(3), maxsplit=1)[0]
        ins = ninja_split(explicit.strip())
        rule = m.group(2)
        a = re.match(r'(\w+?)_STATIC_LIBRARY_LINKER__(.+)_\w+$', rule)
        if a:
            archives[a.group(2)] = (a.group(1), ins)
        for o in outs:
            inputs[o] = ins
    return archives, inputs


def ninja_deps(ninja, bdir):
    deps, cur = {}, None
    out = subprocess.run([str(ninja), '-C', str(bdir), '-t', 'deps'], capture_output=True, text=True,
                         encoding='utf-8', errors='replace', check=True).stdout
    for line in out.splitlines():
        if not line.strip():
            cur = None
        elif not line.startswith(' '):
            cur = line.split(': #deps', 1)[0]
            deps[cur] = []
        elif cur is not None:
            deps[cur].append(line.strip().replace('\\', '/'))
    return deps


# ------------------------------------------------------------------------------ build ---

def export_profile(run, profile, work, jobs):
    """Configure + build one profile's prebuilt targets. Returns {target: info}."""
    stub = work / profile / 'translation'
    bdir = work / profile / 'build'
    write_stub_translation(stub, profile)
    # A fresh configure (every check and the compiler identification run again, as the seed
    # needs); objects already built with the same commands are kept.
    (bdir / 'CMakeCache.txt').unlink(missing_ok=True)
    for d in (bdir / 'CMakeFiles').glob('3.*'):
        shutil.rmtree(d)
    run.run(kpnx_aio.cmake_configure_cmd(run, profile, bdir, stub) + ['-DKARTPAD_PREBUILT_EXPORT=ON'])
    exported = {}
    for m in re.finditer(r'set\(KARTPAD_PREBUILT_(\w+?)_(TYPE|FINGERPRINT) (\S+)\)',
                         (bdir / 'prebuilt-export.cmake').read_text(encoding='utf-8')):
        exported.setdefault(m.group(1), {})[m.group(2).lower()] = m.group(3)
    targets = sorted(exported)
    run.run([run.t.cmake, '--build', bdir, '--target', *targets, '--parallel', jobs],
            progress=kpnx_aio.ninja_progress(f'prebuilt {profile}'), quiet=True)

    archives, inputs = parse_build_ninja(bdir)
    deps = ninja_deps(run.t.ninja, bdir)
    tainted_roots = [fwd(stub).lower() + '/', fwd(bdir / 'gen').lower() + '/', 'gen/']

    def reads_translation(obj):
        if obj not in deps:
            raise SystemExit(f'{profile}: no dependency record for {obj}; cannot check it')
        return any(d.lower().startswith(r) for d in deps[obj] for r in tainted_roots)

    tree = fwd(run.ws)
    result = {}
    for t in targets:
        info = {'fingerprint': exported[t]['fingerprint'], 'type': exported[t]['type'], 'folded': [],
                'objects': [], 'local': []}
        if info['type'] == 'STATIC_LIBRARY':
            if t not in archives:
                raise SystemExit(f'{profile}: no archive rule for {t} in build.ninja')
            info['language'], objs = archives[t]
            for o in objs:
                owner = re.search(r'CMakeFiles/([^/]+)\.dir/', o)
                if owner and owner.group(1) != t and owner.group(1) not in info['folded']:
                    info['folded'].append(owner.group(1))
                if reads_translation(o):
                    raise SystemExit(f'{profile}: {t} object {o} depends on translator output')
            info['objects'] = objs
        elif info['type'] == 'OBJECT_LIBRARY':
            link = next((ins for out, ins in inputs.items() if out.endswith('kartpad_full.elf')), None)
            if link is None:
                raise SystemExit(f'{profile}: no kartpad_full.elf link line in build.ninja')
            for o in (x for x in link if f'/{t}.dir/' in x or x.startswith(f'CMakeFiles/{t}.dir/')):
                if reads_translation(o):
                    src = fwd(inputs[o][0])
                    if not src.startswith(tree + '/'):
                        raise SystemExit(f'{profile}: local source outside the toolkit: {src}')
                    info['local'].append('@TREE@' + src[len(tree):])
                else:
                    info['objects'].append(o)
        else:
            raise SystemExit(f'{profile}: {t} is a {info["type"]}; only static and object libraries can be prebuilt')
        result[t] = info
    return bdir, result


def strip_copy(objcopy, src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(objcopy), '--strip-debug', str(src), str(dst)], check=True)


def place_objects(objcopy, bdir, objects, dest_dir):
    """Copy (without debug sections) under dest_dir, keeping archive member names where unique."""
    rels, used = [], set()
    for o in objects:
        name = Path(o).name
        rel, n = name, 1
        while rel.lower() in used:
            rel = f'{n}/{name}'
            n += 1
        used.add(rel.lower())
        src = Path(o) if Path(o).is_absolute() else bdir / o
        strip_copy(objcopy, src, dest_dir / rel)
        rels.append(rel)
    return rels


def cmake_list(name, items):
    return f'set({name}\n' + ''.join(f'    "{i}"\n' for i in items) + ')\n'


def check_results(cache):
    """Compiler-check results from a CMakeCache.txt (entries whose help string is "Test <VAR>")."""
    out, help_ = {}, None
    for line in cache.read_text(encoding='utf-8').splitlines():
        if line.startswith('//'):
            help_ = line[2:]
            continue
        m = re.match(r'([A-Za-z0-9_]+):INTERNAL=(.*)$', line)
        if m and help_ == f'Test {m.group(1)}':
            out[m.group(1)] = m.group(2)
        help_ = None
    return out


def write_seed(run, bdir, dest, checks):
    dest.mkdir(parents=True)
    tools_root, data = fwd(run.t.root), fwd(run.t.data)
    files_dir = next((bdir / 'CMakeFiles').glob('3.*/CMakeCXXCompiler.cmake')).parent
    for name in COMPILER_FILES:
        text = (files_dir / name).read_text(encoding='utf-8').replace(tools_root, '@TOOLS@')
        if data.lower() in text.lower():
            raise SystemExit(f'{name} names {data} outside the tools folder; cannot reuse it elsewhere')
        (dest / name).write_text(text, encoding='utf-8')
    (dest / 'checks.cmake').write_text(
        '# Compiler-check results recorded by aio/toolkit/make-prebuilt.py (same pinned tools).\n'
        + ''.join(f'set({k} "{v}" CACHE INTERNAL "Test {k}")\n' for k, v in sorted(checks.items())),
        encoding='utf-8')
    meta = {'cmake_files': files_dir.name, 'compiler_files': list(COMPILER_FILES),
            'tools': {n: run.t.key(n) for n in ('cmake', 'llvm', 'devkitpro')}}
    (dest / 'seed.json').write_text(json.dumps(meta, indent=2), encoding='utf-8')


def load_make_toolkit():
    spec = importlib.util.spec_from_file_location('make_toolkit', AIO / 'toolkit/make-toolkit.py')
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--toolkit', type=Path, required=True, help='toolkit tree from make-toolkit.py (updated in place)')
    ap.add_argument('--data', type=Path, required=True, help='AIO work folder for the tools and the builds')
    ap.add_argument('--bundle', default=None, help='KartPadNX-Builder.data to take the tools from')
    ap.add_argument('--shim', type=Path, default=AIO / 'out/obj/nxshim.exe')
    ap.add_argument('--jobs', type=int, default=0)
    args = ap.parse_args()
    toolkit, data = args.toolkit.resolve(), args.data.resolve()
    if not (toolkit / 'aio-toolkit.json').is_file():
        sys.exit(f'{toolkit} is not a toolkit tree (run make-toolkit.py first)')
    t0 = time.time()
    manifest = json.loads((AIO / 'manifest/tools.json').read_text(encoding='utf-8'))
    state = kpnx_aio.State(data / 'state')
    tools = kpnx_aio.Tools(data, manifest, args.shim)
    tools.install_local(state, kpnx_aio.Bundle(args.bundle) if args.bundle else None)
    tools.install_devkitpro(state)
    tools.finish()
    run = kpnx_aio.Runner(tools, toolkit)
    jobs = args.jobs or kpnx_aio.default_jobs()
    run.python('integrate-deko.py')

    out = toolkit / 'aio-prebuilt/libs'
    seed = toolkit / 'aio-prebuilt/cmake'
    for d in (out, seed):
        if d.exists():
            shutil.rmtree(d)
    objcopy, nm = tools.llvm / 'bin/llvm-objcopy.exe', tools.llvm / 'bin/llvm-nm.exe'
    work = data / 'prebuilt'
    placed = {}            # (target, fingerprint) -> object paths relative to out
    first_checks = None
    for profile in PROFILES:
        bdir, targets = export_profile(run, profile, work, jobs)
        checks = check_results(bdir / 'CMakeCache.txt')
        if first_checks is None:
            first_checks = checks
            write_seed(run, bdir, seed, checks)
        elif checks != first_checks:
            sys.exit(f'{profile}: compiler checks differ from the first profile; the seed would be wrong')
        lines = [f'# Written by aio/toolkit/make-prebuilt.py ({profile}); read by switch/cmake/Prebuilt.cmake.\n',
                 cmake_list('KARTPAD_PREBUILT_TARGETS', sorted(targets))]
        for t, info in sorted(targets.items()):
            key = (t, info['fingerprint'])
            # A target compiled the same way for an earlier profile is stored once, there.
            if key not in placed:
                sub = Path(profile) / t
                rels = place_objects(objcopy, bdir, info['objects'], out / sub)
                placed[key] = [(sub / r).as_posix() for r in rels]
            lines.append(f'set(KARTPAD_PREBUILT_{t}_FINGERPRINT {info["fingerprint"]})\n')
            if info.get('language'):
                lines.append(f'set(KARTPAD_PREBUILT_{t}_LANGUAGE {info["language"]})\n')
            if info['folded']:
                lines.append(cmake_list(f'KARTPAD_PREBUILT_{t}_FOLDED', info['folded']))
            if info['local']:
                lines.append(cmake_list(f'KARTPAD_PREBUILT_{t}_LOCAL_SOURCES', info['local']))
            lines.append(cmake_list(f'KARTPAD_PREBUILT_{t}_OBJECTS',
                                    [f'${{CMAKE_CURRENT_LIST_DIR}}/../{p}' for p in placed[key]]))
            print(f'{profile}: {t}: {len(info["objects"])} objects'
                  + (f', {len(info["local"])} compiled by the player' if info['local'] else ''), flush=True)
        (out / profile).mkdir(parents=True, exist_ok=True)
        (out / profile / 'prebuilt.cmake').write_text(''.join(lines), encoding='utf-8')

    # Nothing translated may be in the toolkit. The runtime's hand-written replacements of guest
    # functions (PPC_NATIVE_OVERRIDE in its own sources) are named func_<address> too.
    native = set()
    for d in ('build/wiicompiled-fpscr/runtime', 'switch/src'):
        for p in (toolkit / d).rglob('*'):
            if p.suffix in ('.cpp', '.c', '.h', '.hpp', '.inc'):
                native.update('func_' + a for a in NATIVE_OVERRIDE.findall(p.read_text(encoding='utf-8', errors='replace')))
    objects = sorted(str(p) for p in out.rglob('*.o'))
    bad = []
    for i in range(0, len(objects), 200):
        r = subprocess.run([str(nm), '--defined-only', '--format=just-symbols', *objects[i:i + 200]],
                           capture_output=True, text=True, check=True)
        bad += [s for s in r.stdout.split() if TRANSLATED_SYMBOL.match(s) and s not in native]
    if bad:
        shutil.rmtree(out)
        sys.exit(f'prebuilt objects define translated functions ({", ".join(sorted(set(bad))[:10])}); removed')
    mt = load_make_toolkit()
    for d, _, files in os.walk(out):
        for f in files:
            if f in mt.FORBIDDEN_NAMES or f.startswith('func_8') or f.startswith('shard_'):
                shutil.rmtree(out)
                sys.exit(f'prebuilt output contains a disc-derived file name: {f}; removed')

    size = sum(p.stat().st_size for p in out.rglob('*') if p.is_file())
    tk_json = toolkit / 'aio-toolkit.json'
    meta = json.loads(tk_json.read_text(encoding='utf-8'))
    meta['prebuilt'] = {'created_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                        'profiles': list(PROFILES), 'objects': len(objects), 'bytes': size}
    tk_json.write_text(json.dumps(meta, indent=2), encoding='utf-8')
    print(f'prebuilt: {len(objects)} objects, {size / 1e6:.0f} MB in {out} ({time.time() - t0:.0f}s)')


if __name__ == '__main__':
    main()
