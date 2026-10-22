#!/usr/bin/env bash
# KartPad-NX end-user builder.
#
#   builder/build-switch.sh /path/to/RMCP01.wbfs --stage extract
#
# Prepares the user's dumped disc and translation for the Switch port.
# The complete game build remains unimplemented. Disc-derived code/assets stay
# local; the separate graphics stage builds a synthetic fixture without them.
#
# Stages (explicit selection):
#   1. extract   disc image -> main.dol + StaticR.rel, validate profile
#   2. translate vendor+patch WiiCompiled, run the static recompiler
#   graphics     build the Dawn and SDL3 dependencies (Docker; builder/build-graphics.sh)
#   build/package/all are not this script's job: the game NRO is built by
#   builder/build-fullapp.sh (docs/BUILDING-SWITCH.md).
#
# Requirements: bash, cargo (for nodtool), .NET 8 SDK, devkitPro (devkitA64,
# libnx, cmake, make) and aarch64 Clang (LLVM).
#
# NOTE (Windows): graphics runs in Docker. The native platform-shim recipe and
# host translation prerequisites are documented in the top-level README.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
ref="$repo/ref/kartpad-main"
work="$repo/private/self-build"

image=""
stage=all
profile=base
payload=""
payload_provenance=""
retro_root=""
usage() {
  cat <<'TXT'
usage: build-switch.sh [IMAGE] --stage extract|translate|graphics|build|package|all
       [--profile base|online|retro-rewind] [--payload FILE]
       [--payload-provenance JSON] [--retro-root DIRECTORY]

The complete game build is not implemented yet; all/build/package stop explicitly.
graphics builds the Dawn and SDL3 dependencies inside the kartpad-dawn Docker image.
extract needs IMAGE. translate uses the already extracted private disc tree.
Existing staged runtime trees are retained as timestamped backups when restaged.
TXT
}
while [[ $# -gt 0 ]]; do
  case "$1" in
    --stage|--profile|--payload|--payload-provenance|--retro-root)
      [[ $# -ge 2 && -n "$2" && "$2" != --* ]] || { usage >&2; exit 64; }
      case "$1" in
        --stage) stage="$2";; --profile) profile="$2";; --payload) payload="$2";;
        --payload-provenance) payload_provenance="$2";; --retro-root) retro_root="$2";;
      esac
      shift 2;;
    -h|--help) usage; exit 0;;
    -*) usage >&2; exit 64;;
    *) [[ -z "$image" ]] || { usage >&2; exit 64; }; image="$1"; shift;;
  esac
done
case "$stage" in extract|translate|graphics|build|package|all) ;; *) usage >&2; exit 64;; esac
case "$profile" in base|online|retro-rewind) ;; *) usage >&2; exit 64;; esac
if [[ "$stage" == all || "$stage" == build || "$stage" == package ]]; then
  echo "build-switch.sh runs the extract, translate and graphics stages only." >&2
  echo "Build the game NRO with builder/build-fullapp.sh <base|retro-rewind> all (docs/BUILDING-SWITCH.md)." >&2
  exit 69
fi
if [[ "$stage" == graphics ]]; then
  exec bash "$here/build-graphics.sh" all
fi
if [[ "$stage" == extract && ( -z "$image" || ! -f "$image" ) ]]; then
  echo "extract requires an existing disc image" >&2; exit 64
fi
if [[ "$stage" == translate && "$profile" != base ]]; then
  [[ -f "$payload" ]] || { echo "This profile requires --payload FILE" >&2; exit 64; }
  if [[ "$profile" == retro-rewind && ! -d "$retro_root" ]]; then
    echo "retro-rewind requires --retro-root DIRECTORY" >&2; exit 64
  fi
fi

# Supported PAL RMCP01 rev0 profile (matches KartPad's prepare-disc.sh).
EXPECT_DOL=80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05
EXPECT_REL=16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d

log() { printf '\n=== %s ===\n' "$*"; }
need() { command -v "$1" >/dev/null 2>&1 || { echo "missing tool: $1" >&2; exit 69; }; }

# Preserve previous local work, especially staged Aurora edits. Resolve both
# source and destination in this shell and limit moves to the named workspace
# build/private directories. Never recursively delete a staged tree.
backup_tree() {
  local target="$1" allowed="$2"
  [[ -e "$target" ]] || return 0
  [[ ! -L "$target" ]] || { echo "Refusing to move symlink: $target" >&2; exit 65; }
  local resolved parent destination
  resolved="$(realpath "$target")"
  parent="$(realpath "$allowed")"
  case "$resolved" in "$parent"/*) ;; *) echo "Backup outside expected directory: $resolved" >&2; exit 65;; esac
  destination="${resolved}.backup.$(date +%Y%m%d-%H%M%S).$$"
  case "$destination" in "$parent"/*) ;; *) exit 65;; esac
  [[ ! -e "$destination" ]] || { echo "Backup already exists: $destination" >&2; exit 65; }
  mv -- "$resolved" "$destination"
  printf 'Preserved previous tree: %s\n' "$destination"
}

# ---------------------------------------------------------------- 1. extract ---
stage_extract() {
  log "1/4 extract disc"
  if ! command -v nodtool >/dev/null 2>&1; then
    need cargo
    echo "installing nodtool..."; cargo install nodtool --version 2.0.0-alpha.9 --locked
  fi
  local out="$work/disc"
  if [[ ! -f "$out/sys/main.dol" ]]; then
    backup_tree "$out" "$work"
    mkdir -p "$(dirname "$out")"
    nodtool extract --quiet "$image" "$out"
  fi
  local dol rel
  dol=$(sha256sum "$out/sys/main.dol" | awk '{print $1}')
  rel=$(sha256sum "$out/files/rel/StaticR.rel" | awk '{print $1}')
  [[ "$dol" == "$EXPECT_DOL" && "$rel" == "$EXPECT_REL" ]] || {
    echo "ERROR: disc is not the supported PAL RMCP01 rev0 profile" >&2
    echo "  main.dol=$dol"; echo "  StaticR.rel=$rel"; exit 65; }
  echo "profile OK (PAL RMCP01 rev0)"
}

# -------------------------------------------------------------- 2. translate ---
# Verified working end-to-end on 2026-09-12 (base profile).
UPSTREAM_COMMIT=1912292c804ff9b1b79938de89369ec4496f9fff
# Same immutable sse2neon revision/hash used by KartPad's runtime preparation.
SSE2NEON_COMMIT=13a42df35dc7fcc94f987568e7274a998bb6cc86
SSE2NEON_SHA256=44b9fa3dec3a52ea473246e04b9f692a4e5b0ed654299eef7fe7ec3049e223e0
stage_translate() {
  log "2/4 static recompilation (translator)"
  need dotnet
  local py; py=$(command -v python || command -v py || true)
  [[ -n "$py" ]] || { echo "need a real python 3 (not the Store stub)"; exit 69; }

  local up="$repo/ref/upstream/Wiicompiled"
  local stg="$repo/build/wiicompiled-fpscr"
  local pat="$ref/patches"
  local trans="$work/translation"

  # Non-base profiles use the previously prepared translator and their own
  # output roots; never regenerate or overwrite the base graph as a side effect.
  if [[ "$profile" != base ]]; then
    local args=(--profile "$profile" --payload "$payload")
    [[ -z "$payload_provenance" ]] || args+=(--payload-provenance "$payload_provenance")
    [[ -z "$retro_root" ]] || args+=(--retro-root "$retro_root")
    "$py" "$here/translate-profile.py" "${args[@]}"
    return
  fi
  [[ -f "$work/disc/sys/main.dol" && -f "$work/disc/files/rel/StaticR.rel" ]] || {
    echo "Run --stage extract first" >&2; exit 66; }
  [[ "$(sha256sum "$work/disc/sys/main.dol" | awk '{print $1}')" == "$EXPECT_DOL" &&
     "$(sha256sum "$work/disc/files/rel/StaticR.rel" | awk '{print $1}')" == "$EXPECT_REL" ]] || {
    echo "Unsupported or changed extracted PAL disc files" >&2; exit 65; }

  # 2a. vendor upstream at the pinned commit
  if [[ ! -d "$up/.git" ]]; then
    git clone "https://github.com/patchzyy/Wiicompiled" "$up"
    git -C "$up" checkout "$UPSTREAM_COMMIT"
  fi
  [[ "$(git -C "$up" rev-parse HEAD)" == "$UPSTREAM_COMMIT" ]] || {
    echo "WiiCompiled checkout is not at the pinned revision; leaving it unchanged" >&2; exit 65; }

  # 2b. stage + apply KartPad patches (order matters)
  backup_tree "$stg" "$repo/build"
  mkdir -p "$stg"
  (cd "$up" && tar --exclude=.git --exclude=bin --exclude=obj -cf - .) | (cd "$stg" && tar -xf -)
  git apply --recount --unidiff-zero --unsafe-paths --directory="$stg" "$pat/wiicompiled-fpscr-state.patch"
  git apply --recount --unsafe-paths --directory="$stg" "$pat/wiicompiled-dual-profile-translator.patch"
  git apply --recount --unsafe-paths --directory="$stg" "$pat/wiicompiled-kamek-v2.patch"
  patch --batch -p1 -d "$stg" < "$pat/wiicompiled-dual-profile-symbols.patch"
  patch --batch -p1 -d "$stg" < "$pat/wiicompiled-dual-profile-closure.patch"
  patch --batch -p1 -d "$stg" < "$pat/wiicompiled-dynamic-overrides.patch"

  # aarch64/Switch runtime portability: vendor sse2neon + gate the x86/MSVC ISA
  # layer to Clang+aarch64. Required for the translated shards to compile with
  # Clang against the devkitA64 sysroot. See switch/patches/README.md.
  mkdir -p "$stg/runtime/third_party/sse2neon"
  if [[ ! -f "$stg/runtime/third_party/sse2neon/sse2neon.h" ]]; then
    curl -fsSL "https://raw.githubusercontent.com/DLTcollab/sse2neon/$SSE2NEON_COMMIT/sse2neon.h" \
      -o "$stg/runtime/third_party/sse2neon/sse2neon.h"
  fi
  [[ "$(sha256sum "$stg/runtime/third_party/sse2neon/sse2neon.h" | awk '{print $1}')" == "$SSE2NEON_SHA256" ]] || {
    echo "sse2neon digest mismatch" >&2; exit 65; }
  patch -p1 -d "$stg/runtime" < "$here/../switch/patches/switch-runtime-portability.patch"

  # REL-report guard: adds RecompMod::TryGetRelReportSectionTable (decl in
  # recomp_mod_loader.h, def in recomp_mod_loader.cpp). KartPad applies this in
  # its per-platform runtime prep (prepare-g7-game-runtime.sh), not in the
  # translator prep — the injected call in func_8000A440 is unresolved without it.
  patch -p1 -d "$stg" < "$ref/patches/wiicompiled-retro-rel-report-guard.patch"

  # b73 post-call reload elision for nonvolatile GPRs (docs/PERF-B73-2026-10-14.md). Translate with
  # KARTPAD_RELOAD_ELISION=0 to reproduce the pre-b73 output.
  "$py" "$here/apply-translator-reload-elision.py" "$stg"

  # Keep the graphics port reproducible after restaging upstream. These are
  # applied to Aurora, never to generated disc-derived code.
  for p in aurora-switch-dawn-api aurora-switch-surface aurora-switch-window; do
    patch --batch -p1 -d "$stg/aurora-main" < "$repo/switch/patches/$p.patch"
  done

  # 2c. build the translator
  local dll="$stg/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll"
  dotnet build "$stg/translator/src/Translator.Cli/Translator.Cli.csproj" -c Release

  # 2d. manifest with workspace_root pointing at the repo root
  mkdir -p "$repo/build"
  cp "$ref/tools/mkwii-rmcp01-base.yml" "$repo/build/mkwii-rmcp01-base.yml"  # workspace_root: .. == repo root
  local proj="$repo/build/mkwii-rmcp01-base.yml"

  # 2e. translate (~30k functions)
  local jobs="${KARTPAD_TRANSLATION_JOBS:-$(nproc 2>/dev/null || echo 4)}"
  [[ "$jobs" -gt 8 ]] && jobs=8
  dotnet "$dll" translate-recursive 0x800060A4 --project "$proj" --threads "$jobs" \
    --prune-stale --output-metadata "$trans/base_translation_output.json"

  # 2f. gameplay hook injections (shared list: builder/translator-post-hooks.py)
  local F="$trans/functions"
  "$py" "$here/translator-post-hooks.py" functions "$F"

  # 2g. data-init + build shards
  dotnet "$dll" generate-data-init --project "$proj"
  dotnet "$dll" emit-build-shards --project "$proj" \
    --base-metadata "$trans/base_translation_output.json" \
    --base-functions-dir "$F" \
    --native-source-dir "$stg/runtime/src" \
    --out "$trans/build_shards"
  grep -q '^set(MKW_BASE_FUNCTION_COUNT 29065)$' "$trans/build_shards/shards.cmake"

  # 2h. finalize + verify the REL-report guard in the emitted shard graph
  "$py" "$here/translator-post-hooks.py" shards "$trans/build_shards"
  echo "translation OK: $(find "$F" -name 'func_*.cpp' | wc -l) functions, shards.cmake ready"
}

# Full game build/package stages are rejected in preflight until the actual
# Switch runtime exists. Do not restore the old copy-only packaging scaffold:
# it referred to a nonexistent kartpad.nro and promised unimplemented extraction.
case "$stage" in
  extract) stage_extract;;
  translate) stage_translate;;
esac
log "done"
