#!/usr/bin/env python3
"""Translator post-hooks shared by the base and profile translate drivers.

    python builder/translator-post-hooks.py functions <functions dir> [--print]
    python builder/translator-post-hooks.py shards <build_shards dir> [--print]

`builder/build-switch.sh` (base) and `builder/translate-profile.py` (Retro Rewind) both call this
file, so a translator-level hook added here reaches every profile. Order is the base order:
  functions: applied to each emitted function file after translate-recursive, before data-init
             and shard emission;
  shards:    applied to the emitted build_shards after emit-build-shards (inject, then verify).
`--print` lists the commands instead of running them (used to prove the sequence is unchanged).
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPTS = ROOT / "ref/kartpad-main/scripts"

# (script, function file) in application order. Base output depends on this exact sequence.
FUNCTION_HOOKS = [
    ("inject-g10-rkg-fixture-hook.py", "func_8051FC84.cpp"),
    *[("inject-online-rkg-selection-hooks.py", "func_%s.cpp" % fn)
      for fn in ("8083DFA8", "80846C1C", "8084E388", "80643F48")],
    ("inject-g10-camera-lifecycle-guard.py", "func_805A1A8C.cpp"),
    ("inject-retro-rel-report-guard.py", "func_8000A440.cpp"),
]

# (script, mode flag) applied to the shard directory, in order.
SHARD_HOOKS = [
    ("inject-retro-rel-report-guard.py", "--inject-shards"),
    ("inject-retro-rel-report-guard.py", "--verify-shards"),
]


def commands(phase, target):
    if phase == "functions":
        return [[sys.executable, str(SCRIPTS / script), str(target / name)]
                for script, name in FUNCTION_HOOKS]
    if phase == "shards":
        return [[sys.executable, str(SCRIPTS / script), flag, str(target)]
                for script, flag in SHARD_HOOKS]
    raise SystemExit("unknown phase: " + phase)


def main(argv):
    dry = "--print" in argv
    argv = [a for a in argv if a != "--print"]
    if len(argv) != 2:
        raise SystemExit(__doc__)
    phase, target = argv[0], Path(argv[1]).resolve()
    for cmd in commands(phase, target):
        if dry:
            print(" ".join(str(x) for x in cmd))
        else:
            subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main(sys.argv[1:])
