#!/usr/bin/env bash
# Build the real translated game runtime as a Switch NRO.
# Run inside the kartpad-dawn image with this checkout mounted at /project.
#
#   builder/build-fullapp.sh [base|retro-rewind] [patch|configure|transform|build|all]
#
#   patch      apply the runtime/Aurora patches to the staged tree (build/wiicompiled-fpscr)
#   configure  patch, then configure CMake
#   transform  rewrite the private translation in place (see do_transform); idempotent
#   build      compile only: never changes the translation or the staged runtime
#   all        patch, configure, transform, build
#
# Needs the Dawn Null and SDL3 builds (build-graphics.sh), the staged runtime and
# Aurora (stage-runtime.py), and the private translation (build-switch.sh --stage translate).
# KARTPAD_BUILD_JOBS sets the compile jobs (default 2: translated shards need a lot of RAM).
# Never contacts the console or launches a test.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
if [[ "${1:-}" =~ ^(base|retro-rewind)$ ]]; then
  profile="$1"
  stage="${2:-all}"
else
  profile="${KARTPAD_PROFILE:-base}"
  stage="${1:-all}"
fi
jobs="${KARTPAD_BUILD_JOBS:-2}"
case "$stage" in patch|configure|transform|build|all) ;; *)
  echo "usage: $0 [base|retro-rewind] [patch|configure|transform|build|all]" >&2; exit 64;; esac
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "KARTPAD_BUILD_JOBS must be positive" >&2; exit 64; }

dawn="$repo/ref/dawn"
sdl="$repo/ref/SDL"
aurora="$repo/build/wiicompiled-fpscr/aurora-main"
dawn_build="$repo/build/dawn-switch-deko"
sdl_build="$repo/build/sdl-switch-vulkan"
runtime="$repo/build/wiicompiled-fpscr/runtime"

if [[ "$profile" == "retro-rewind" ]]; then
  build_dir="${KARTPAD_BUILD_DIR:-$repo/build/switch-fullapp-rr}"
  translation_dir="${KARTPAD_TRANSLATION_DIR:-$repo/private/self-build/retro-rewind/translation}"
  shards="${KARTPAD_SHARDS:-$translation_dir/build_shards/shards.cmake}"
  app_name="${KARTPAD_APP_NAME:-KartPad Retro Rewind}"
  output_nro_name="${KARTPAD_NRO_NAME:-kartpad-retro-rewind.nro}"
  pgo_pref="$repo/private/pgo/current-retro-rewind"
else
  build_dir="${KARTPAD_BUILD_DIR:-$repo/build/switch-fullapp}"
  translation_dir="${KARTPAD_TRANSLATION_DIR:-$repo/private/self-build/translation}"
  shards="${KARTPAD_SHARDS:-$translation_dir/build_shards/shards.cmake}"
  app_name="${KARTPAD_APP_NAME:-KartPad-NX}"
  output_nro_name="${KARTPAD_NRO_NAME:-kartpad.nro}"
  pgo_pref="$repo/private/pgo/current"
fi

# Sample PGO per profile: private/pgo/current-retro-rewind (falling back to private/pgo/current).
pgo_current_file=""
if [[ -f "$pgo_pref" ]]; then
  pgo_current_file="$pgo_pref"
elif [[ -f "$repo/private/pgo/current" ]]; then
  pgo_current_file="$repo/private/pgo/current"
fi

if [[ "${KARTPAD_PGO:-1}" != "0" && -z "${KARTPAD_PGO_PROFILE:-}" && -n "$pgo_current_file" ]]; then
  pgo_name="$(tr -d ' \r\n' < "$pgo_current_file")"
  if [[ -n "$pgo_name" ]]; then
    KARTPAD_PGO_PROFILE="$repo/private/pgo/$pgo_name.extbin"
    KARTPAD_SECTION_ORDER="${KARTPAD_SECTION_ORDER:-$repo/private/pgo/$pgo_name.order.ld}"
    KARTPAD_PGO_ALL="${KARTPAD_PGO_ALL:-ON}"
    require_pgo_file() { [[ -f "$1" ]] || { echo "$pgo_current_file names a missing file: $1" >&2; exit 66; }; }
    require_pgo_file "$KARTPAD_PGO_PROFILE"
    require_pgo_file "$KARTPAD_SECTION_ORDER"
  fi
fi

# CPU code-generation options. Explicit defaults for every profile (they used to be copied from
# whatever the base build directory had cached, so an RR build could silently change with it):
#   KARTPAD_OUTLINE_SCALAR_FP  ON   rare scalar-FP exception paths are outlined out of hot code
#   KARTPAD_THINLTO_SHARDS     OFF  ThinLTO over the translated shards (experimental)
KARTPAD_OUTLINE_SCALAR_FP="${KARTPAD_OUTLINE_SCALAR_FP:-ON}"
KARTPAD_THINLTO_SHARDS="${KARTPAD_THINLTO_SHARDS:-OFF}"

require_file() { [[ -f "$1" ]] || { echo "Required artifact missing: $1" >&2; exit 66; }; }

# Apply the libnx runtime port patch to the staged tree, idempotently. Reverse-
# check first so a re-run is a no-op. --ignore-whitespace tolerates the CRLF bind
# mount; --recount tolerates line drift from the earlier applied patches.
apply_runtime_patch() {
  local patch_file="$repo/switch/patches/wiicompiled-switch-runtime.patch"
  require_file "$patch_file"
  # From the repository top with --directory: inside the repo, `git -C "$runtime" apply` skips
  # every path. Once the runtime series has edited the same files, the reverse check no longer
  # matches, so the bridge's own symbols identify an already patched stage.
  git_apply_runtime() { git -C "$repo" apply --directory="${runtime#"$repo"/}" --recount --ignore-whitespace "$@"; }
  if git_apply_runtime --reverse --check "$patch_file" 2>/dev/null; then
    echo "Already applied: ${patch_file##*/}"
  elif grep -q "kartpad_fiber" "$runtime/src/fiber_manager.cpp" && \
       grep -q "KartPadRuntimeMain" "$runtime/src/main.cpp"; then
    echo "Already applied (compatible Switch runtime bridge detected): ${patch_file##*/}"
  elif git_apply_runtime --check "$patch_file" 2>/dev/null; then
    git_apply_runtime "$patch_file"
    echo "Applied: ${patch_file##*/}"
  else
    echo "ERROR: wiicompiled-switch-runtime.patch does not apply cleanly to $runtime" >&2
    git_apply_runtime --check "$patch_file" || true
    exit 65
  fi
}

# Switch full-app source arms (windows.h guards, aarch64 long/long-long, libnx
# Crypto++ RNG, profile-tagged registrar, pipe() guard, SD app-data / dvd_root
# defaults). GNU patch with --ignore-whitespace tolerates the CRLF bind mount;
# reverse-check first for idempotency.
apply_fullapp_patch() {
  local pf="$repo/switch/patches/wiicompiled-switch-fullapp.patch"
  require_file "$pf"
  if patch -p1 -R --dry-run --ignore-whitespace -d "$runtime" < "$pf" >/dev/null 2>&1; then
    echo "Already applied: ${pf##*/}"
  elif grep -E -q 'romfs:/(wii|wii_bootstrap)' "$runtime/include/nand_path.h" && \
       grep -q 'sdmc:/switch/kartpad/NAND' "$runtime/include/runtime_config.h"; then
    # The staged tree can contain the same port changes with regenerated hunk
    # context; do not reject a known-compatible working tree.
    echo "Already applied (compatible Switch config/NAND bridge detected): ${pf##*/}"
  elif patch -p1 --dry-run --ignore-whitespace -d "$runtime" < "$pf" >/dev/null 2>&1; then
    patch -p1 --ignore-whitespace -d "$runtime" < "$pf"
    echo "Applied: ${pf##*/}"
  else
    echo "ERROR: wiicompiled-switch-fullapp.patch does not apply cleanly to $runtime" >&2
    exit 65
  fi
}

# KartPad's runtime and Aurora changes: switch/patches/runtime, one patch per subsystem, applied
# in `series` order (builder/regen-runtime-patches.py writes them from the staged tree). They are
# diffs against the 2026-09-23 staged baseline, i.e. after the three patches above. A patch that
# neither reverse-applies (already in) nor applies stops the build, so a stage can never build
# with part of the runtime changes missing.
#
# The staged tree sits inside this repository, where `git -C <tree> apply` silently skips every
# path ("Skipped patch") and reports success. Applying from the repository top with --directory
# makes git check and patch the files for real.
apply_runtime_patches() {
  local dir="$repo/switch/patches/runtime"
  local tree="build/wiicompiled-fpscr"
  require_file "$dir/series"
  local name pf
  git_apply() { git -C "$repo" apply --directory="$tree" --recount --ignore-whitespace "$@"; }
  while read -r name; do
    [[ -n "$name" ]] || continue
    pf="$dir/$name"
    require_file "$pf"
    if git_apply --reverse --check "$pf" >/dev/null 2>&1; then
      echo "Already applied: runtime/$name"
    elif git_apply --check "$pf" >/dev/null 2>&1; then
      git_apply "$pf"
      echo "Applied: runtime/$name"
    else
      echo "ERROR: runtime/$name neither applies nor is already applied to $repo/$tree" >&2
      exit 65
    fi
  done < "$dir/series"
}

# Switch-native Classic Controller HLE. Kept separate because it touches the
# upstream input stubs rather than the general runtime portability layer.
apply_input_patch() {
  local pf="$repo/switch/patches/wiicompiled-switch-input.patch"
  require_file "$pf"
  if patch -p1 -R --dry-run --ignore-whitespace -d "$runtime" < "$pf" >/dev/null 2>&1; then
    echo "Already applied: ${pf##*/}"
  elif grep -q "KartPadMobileReadClassicInputForPlayer" "$runtime/src/hle/input/kpad.cpp"; then
    # Accept a compatible locally evolved bridge as already applied. This keeps
    # existing staged work buildable while fresh staging receives the patch.
    echo "Already applied (compatible bridge detected): ${pf##*/}"
  elif patch -p1 --dry-run --ignore-whitespace -d "$runtime" < "$pf" >/dev/null 2>&1; then
    patch -p1 --ignore-whitespace -d "$runtime" < "$pf"
    echo "Applied: ${pf##*/}"
  else
    echo "ERROR: wiicompiled-switch-input.patch does not apply cleanly to $runtime" >&2
    exit 65
  fi
}

do_configure() {
  : "${DEVKITPRO:?Run in the kartpad-dawn image with DEVKITPRO set}"
  python3 "$repo/builder/integrate-deko.py"
  require_file "$shards"
  require_file "$runtime/src/main.cpp"
  require_file "$dawn_build/src/dawn/native/libwebgpu_dawn.a"
  require_file "$sdl_build/libSDL3.a"
  cmake -S "$repo/switch" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$repo/switch/cmake/SwitchClang.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_EXE_LINKER_FLAGS=-L$DEVKITPRO/libnx/lib -L$DEVKITPRO/portlibs/switch/lib -fPIE -specs=$DEVKITPRO/libnx/switch.specs" \
    -DPython3_EXECUTABLE="$(command -v python3)" \
    -DKARTPAD_PROFILE="$profile" \
    -DKARTPAD_BUILD_GRAPHICS=ON -DKARTPAD_ENABLE_FULL_APP=ON \
    -DKARTPAD_SHARDS="$shards" \
    -DKARTPAD_TRANSLATION_DIR="$translation_dir" \
    -DKARTPAD_RUNTIME_SRC="$runtime" \
    -DKARTPAD_DAWN_SOURCE="$dawn" -DKARTPAD_DAWN_BUILD="$dawn_build" \
    -DKARTPAD_SDL_SOURCE="$sdl" -DKARTPAD_SDL_BUILD="$sdl_build" \
    -DKARTPAD_AURORA_SOURCE="$aurora" \
    -DKARTPAD_PGO_PROFILE="${KARTPAD_PGO_PROFILE:-}" \
    -DKARTPAD_SECTION_ORDER="${KARTPAD_SECTION_ORDER:-}" \
    -DKARTPAD_PGO_ALL="${KARTPAD_PGO_ALL:-OFF}" \
    -DKARTPAD_OUTLINE_SCALAR_FP="${KARTPAD_OUTLINE_SCALAR_FP}" \
    -DKARTPAD_THINLTO_SHARDS="$KARTPAD_THINLTO_SHARDS" \
    -DKARTPAD_THINLTO_JOBS="${KARTPAD_THINLTO_JOBS:-1}" \
    -DKARTPAD_APP_NAME="$app_name" \
    -DKARTPAD_NRO_NAME="$output_nro_name"
}

# Reconfigures when the build directory was configured with different options than this run's.
reconfigure_if_options_changed() {
  if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
    echo "Build directory not configured yet; configuring..."
    do_configure
    return
  fi
  cached() { grep -E "^$1:" "$build_dir/CMakeCache.txt" 2>/dev/null | cut -d= -f2- || true; }
  local name want have
  for pair in "KARTPAD_DAWN_BUILD=$dawn_build" "KARTPAD_PROFILE=$profile" \
              "KARTPAD_PGO_PROFILE=${KARTPAD_PGO_PROFILE:-}" "KARTPAD_SECTION_ORDER=${KARTPAD_SECTION_ORDER:-}" \
              "KARTPAD_OUTLINE_SCALAR_FP=$KARTPAD_OUTLINE_SCALAR_FP" "KARTPAD_THINLTO_SHARDS=$KARTPAD_THINLTO_SHARDS"; do
    name="${pair%%=*}"
    want="${pair#*=}"
    have="$(cached "$name")"
    if [[ "$have" != "$want" ]]; then
      echo "$name changed ($have -> $want); reconfiguring..."
      do_configure
      return
    fi
  done
}

# The NRO format needs libnx's crt0 `_start` (a branch over the MOD0/NRO header words) at image
# offset 0; elf2nro writes the NRO header over bytes 0x10+. pgo4-pgo6 broke this with a link order
# file that placed a hot function first, and crashed at boot with "Undefined Instruction at
# kartpad_full + 0x14" (docs/PERF-60FPS-ROADMAP.md, Finding 14). Refuse to leave such an NRO behind.
check_image_start() {
  local nm="${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin/aarch64-none-elf-nm"
  local elf="$build_dir/kartpad_full.elf"
  local start
  # awk reads to the end: exiting at the first match would SIGPIPE nm, and pipefail would then
  # abort this script with 141 (pgo7/b26 did, after building fine).
  start="$("$nm" "$elf" | awk '$3 == "_start" && !found { print $1; found = 1 }')"
  if [[ "$start" != "0000000000000000" ]]; then
    rm -f "$build_dir/$output_nro_name"
    echo "IMAGE CHECK FAILED: _start is at '${start:-missing}', not 0 (link order?); $output_nro_name deleted" >&2
    exit 1
  fi
  echo "image check: _start at offset 0"
}

# The staged runtime's batched DVD directory index (a source rewrite of upstream code).
apply_dvd_directory_index() {
  python3 "$repo/builder/inject-dvd-directory-index.py" "$runtime"
}

# Rewrites the private translation in place. Every pass is idempotent: rerunning it on an
# already-transformed tree changes nothing.
#
# Correctness passes (always): preserved-register boundaries around scene, archive and globe
# calls, and the RR menu-model control-flow stitch (a no-op on base).
#
# CPU passes (KARTPAD_CPU_REWRITES): FP compare reuse, and blanking GPR flush stores made
# redundant by the translator's reload elision (docs/PERF-B73-2026-10-14.md). They are on for
# retro-rewind, whose builds were hardware-tested with them, and off for base: the tested base
# NRO was built without them (docs/BASE-TRANSLATION-REGEN-2026-10-18.md).
do_transform() {
  local cpu_rewrites_default=0
  [[ "$profile" == "retro-rewind" ]] && cpu_rewrites_default=1
  if [[ "${KARTPAD_CPU_REWRITES:-$cpu_rewrites_default}" == "1" ]]; then
    python3 "$repo/builder/optimize-translated-compares.py" "$translation_dir" --apply
    if [[ -f "$translation_dir/nonvolatile_reload_exemptions.json" ]]; then
      python3 "$repo/builder/elide-redundant-flushes.py" "$translation_dir/build_shards" \
        --exemptions "$translation_dir/nonvolatile_reload_exemptions.json"
    else
      echo "flush elision: no reload exemption table in $translation_dir (pre-b73 translation); skipped"
    fi
  else
    echo "CPU rewrites: off for $profile (KARTPAD_CPU_REWRITES=1 enables them)"
  fi
  python3 "$repo/builder/inject-scene-register-boundary.py" "$translation_dir/build_shards"
  python3 "$repo/builder/inject-archive-wait-checkpoint.py" "$translation_dir/build_shards"
  python3 "$repo/builder/inject-archive-call-boundary.py" "$translation_dir/build_shards"
  python3 "$repo/builder/fix-rr-menu-model-flow.py" "$translation_dir/build_shards"
  python3 "$repo/builder/inject-globe-scene-boundary.py" "$translation_dir/build_shards"
}

do_build() {
  reconfigure_if_options_changed
  cmake --build "$build_dir" --target kartpad_full_nro --parallel "$jobs"
  check_image_start
  printf 'Built locally: %s\n' "$build_dir/$output_nro_name"
  echo "No runtime/hardware test was launched by this script."
}

do_patch() {
  apply_runtime_patch
  apply_fullapp_patch
  apply_input_patch
  apply_runtime_patches
  apply_dvd_directory_index
}

case "$stage" in
  patch)     do_patch ;;
  configure) do_patch; do_configure ;;
  transform) do_transform ;;
  build)     do_build ;;
  all)       do_patch; do_configure; do_transform; do_build ;;
esac
