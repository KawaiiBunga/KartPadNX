#!/usr/bin/env bash
# Build the graphics dependencies the game NROs link: Dawn (Null backend + Tint's GLSL writer)
# and SDL3 for Switch. Run inside the kartpad-dawn image with this checkout at /project.
#
#   builder/build-graphics.sh [dawn|sdl|all]     (default all)
#
#   dawn  patch ref/dawn (dawn-*.patch, prepare-deko.py) and build build/dawn-switch-deko
#   sdl   patch ref/SDL (external graphics, AUDOUT queue) and build build/sdl-switch-vulkan
#
# KARTPAD_BUILD_JOBS sets the compile jobs (default 2). Never contacts the console.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
stage="${1:-all}"
jobs="${KARTPAD_BUILD_JOBS:-2}"
case "$stage" in dawn|sdl|all) ;; *) echo "usage: $0 [dawn|sdl|all]" >&2; exit 64;; esac
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "KARTPAD_BUILD_JOBS must be positive" >&2; exit 64; }
: "${DEVKITPRO:?Run in the kartpad-dawn image with DEVKITPRO set}"

dawn="$repo/ref/dawn"
sdl="$repo/ref/SDL"
dawn_build="$repo/build/dawn-switch-deko"
sdl_build="$repo/build/sdl-switch-vulkan"  # name kept: FullApp.cmake and existing caches use it

require_file() {
  [[ -f "$1" ]] || { echo "Required source/artifact missing: $1" >&2; exit 66; }
}

# GNU patch, reverse-checked first so a patched tree is left alone. It also works for source
# trees without .git, where `git apply` would find this repository and skip the paths.
# --ignore-whitespace tolerates CRLF on the Windows bind mount.
apply_patch_once() {
  local tree="$1" patch_file="$2"
  require_file "$patch_file"
  if patch --batch --forward -p1 -R --dry-run --ignore-whitespace -d "$tree" -i "$patch_file" >/dev/null 2>&1; then
    echo "Already applied: ${patch_file##*/}"
  elif patch --batch --forward -p1 --dry-run --ignore-whitespace -d "$tree" -i "$patch_file" >/dev/null 2>&1; then
    patch --batch --forward -p1 --ignore-whitespace -d "$tree" -i "$patch_file"
  else
    echo "Patch conflicts with $tree: $patch_file (nothing changed)" >&2
    exit 65
  fi
}

build_dawn() {
  require_file "$dawn/CMakeLists.txt"
  apply_patch_once "$dawn" "$repo/switch/patches/dawn-switch-complete.patch"
  apply_patch_once "$dawn/third_party/abseil-cpp" "$repo/switch/patches/dawn-abseil-switch.patch"
  python3 "$repo/builder/prepare-deko.py"
  cmake -S "$dawn" -B "$dawn_build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$repo/switch/cmake/SwitchClang.cmake" \
    -C "$repo/switch/cmake/DawnOptions.cmake" \
    -DDAWN_ENABLE_VULKAN=OFF -DDAWN_ENABLE_NULL=ON \
    -DTINT_BUILD_GLSL_WRITER=ON -DTINT_BUILD_NULL_WRITER=ON \
    -DTINT_BUILD_SPV_READER=OFF -DTINT_BUILD_SPV_WRITER=OFF \
    -DTINT_BUILD_GLSL_VALIDATOR=OFF -DDAWN_FETCH_DEPENDENCIES=ON
  cmake --build "$dawn_build" --target webgpu_dawn tint_lang_glsl_writer \
    tint_lang_wgsl_reader tint_lang_wgsl_inspector --parallel "$jobs"
}

build_sdl() {
  require_file "$sdl/CMakeLists.txt"
  apply_patch_once "$sdl" "$repo/switch/patches/sdl-switch-external-graphics.patch"
  apply_patch_once "$sdl" "$repo/switch/patches/sdl-switch-audio-queue.patch"
  # SDL is portable C: it keeps GCC's libnx soft-TLS convention. The executable's pthread wrapper
  # synchronizes Clang TLS before SDL worker start routines too.
  cmake -S "$sdl" -B "$sdl_build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF \
    -DSDL_EXAMPLES=OFF -DSDL_SWITCH_EXTERNAL_GRAPHICS=ON \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_RENDER_GPU=OFF
  cmake --build "$sdl_build" --target SDL3-static --parallel "$jobs"
}

case "$stage" in
  dawn) build_dawn ;;
  sdl) build_sdl ;;
  all) build_dawn; build_sdl ;;
esac
echo "Graphics dependencies built. Next: builder/build-fullapp.sh <base|retro-rewind> all"
