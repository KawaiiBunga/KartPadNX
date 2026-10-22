#!/usr/bin/env bash
# Build the standalone MKW Launcher Switch NRO.
# Run inside the kartpad-dawn Docker container with this checkout mounted at /project:
#
#   builder/build-launcher.sh [nopayload|payload] [configure|build|clean]
#
#   nopayload: RomFS contains only launcher/romfs/ assets (fonts/sfx/ui).
#   payload:   RomFS also bundles zstd-compressed game NROs + payloads.json.
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"

mode="nopayload"
stage="build"

for arg in "$@"; do
  case "$arg" in
    nopayload|payload)
      mode="$arg"
      ;;
    configure|build|clean)
      stage="$arg"
      ;;
    *)
      echo "ERROR: Unknown argument '$arg'" >&2
      echo "Usage: $0 [nopayload|payload] [configure|build|clean]" >&2
      exit 64
      ;;
  esac
done

build_dir="$repo/build/launcher"
src_dir="$repo/launcher"
romfs_stage="$build_dir/romfs"
jobs="${KARTPAD_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"

if [[ "$stage" == "clean" ]]; then
  echo "==> Cleaning MKW Launcher build directory: $build_dir"
  rm -rf "$build_dir"
  exit 0
fi

: "${DEVKITPRO:?DEVKITPRO must be set (ensure script runs inside kartpad-dawn container)}"

mkdir -p "$build_dir"
mkdir -p "$romfs_stage"

# Prepare RomFS contents
if [[ -d "$src_dir/romfs" ]]; then
  cp -a "$src_dir/romfs/." "$romfs_stage/" 2>/dev/null || true
fi

if [[ "$mode" == "payload" ]]; then
  echo "==> Mode: PAYLOAD (packing zstd game NROs into RomFS staging)..."
  vanilla_nro="$repo/build/switch-fullapp/kartpad.nro"
  rr_nro="$repo/build/switch-fullapp-rr/kartpad-retro-rewind.nro"
  if [[ ! -f "$vanilla_nro" ]]; then
    echo "ERROR: Missing vanilla game NRO: $vanilla_nro" >&2
    exit 1
  fi
  if [[ ! -f "$rr_nro" ]]; then
    echo "ERROR: Missing Retro Rewind game NRO: $rr_nro" >&2
    exit 1
  fi
  python3 "$repo/builder/pack-payloads.py" \
    --vanilla "$vanilla_nro" \
    --rr "$rr_nro" \
    --out-dir "$romfs_stage/payload"
else
  echo "==> Mode: NOPAYLOAD (no game binaries in RomFS)..."
  rm -rf "$romfs_stage/payload"
fi

prev_mode="$(cat "$build_dir/.mode" 2>/dev/null || true)"
if [[ "$mode" != "$prev_mode" || "$mode" == "payload" ]]; then
  echo "==> Re-triggering NRO packaging for $mode..."
  rm -f "$build_dir/mkw-launcher.nro"
  if [[ -f "$build_dir/mkw_launcher.elf" ]]; then
    touch "$build_dir/mkw_launcher.elf"
  fi
  echo "$mode" > "$build_dir/.mode"
fi

do_configure() {
  echo "==> Configuring MKW Launcher ($mode)..."
  cmake -S "$src_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$repo/switch/cmake/SwitchClang.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_EXE_LINKER_FLAGS=-L$DEVKITPRO/libnx/lib -L$DEVKITPRO/portlibs/switch/lib -fPIE -specs=$DEVKITPRO/libnx/switch.specs" \
    -DLAUNCHER_ROMFS_DIR="$romfs_stage"
}

if [[ ! -f "$build_dir/build.ninja" || "$stage" == "configure" ]]; then
  do_configure
fi

if [[ "$stage" == "configure" ]]; then
  echo "==> Configuration complete."
  exit 0
fi

echo "==> Building MKW Launcher ($mode mode, $jobs jobs)..."
ninja -C "$build_dir" -j "$jobs" mkw_launcher_nro

nro="$build_dir/mkw-launcher.nro"
if [[ -f "$nro" ]]; then
  echo "==> MKW Launcher built successfully:"
  size="$(stat -c %s "$nro")"
  sha="$(sha256sum "$nro" | cut -d' ' -f1)"
  echo "Output: $nro ($size bytes, SHA-256: $sha)"
else
  echo "ERROR: Expected NRO artifact missing: $nro" >&2
  exit 1
fi
