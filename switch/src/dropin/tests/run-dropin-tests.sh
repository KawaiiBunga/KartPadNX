#!/usr/bin/env bash
# Host tests for switch/src/dropin (docs/RR-DROP-IN.md). Needs clang++ and python3 on x86-64 Linux
# (WSL is fine) and a staged runtime (an AIO work folder's ws/).
#
#   bash switch/src/dropin/tests/run-dropin-tests.sh <work>/ws [main.dol] [Code.pul translate-mod-dir]
#
# Without main.dol the interpreter runs only its hand-assembled programs; without Code.pul and the
# translate-mod output (<ws>/private/self-build/retro-rewind/mod) the loader check is skipped.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$(cd "$here/../.." && pwd)"
ws="$1"
out="${TMPDIR:-/tmp}/kpnx-dropin"
mkdir -p "$out"
rt="$ws/build/wiicompiled-fpscr/runtime"

clang++ -std=gnu++20 -O1 -march=haswell -w -DTARGET_PC -DNOMINMAX \
  -I"$rt/include" -I"$ws/switch/include" -I"$src" \
  "$src/dropin/ppc_interpreter.cpp" "$here/interpreter_test.cpp" "$here/runtime_stubs.cpp" "$rt/src/fpu_helpers.cpp" \
  -o "$out/interpreter_test"
"$out/interpreter_test" ${2:+"$2"}

if [[ $# -ge 4 ]]; then
  clang++ -std=gnu++20 -O2 -Wall -Wextra -I"$src" "$src/dropin/kamek_loader.cpp" "$here/kamek_loader_dump.cpp" -o "$out/kamek_loader_dump"
  python3 "$here/test_kamek_loader.py" "$out/kamek_loader_dump" "$3" "$4"
fi
