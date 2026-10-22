#!/usr/bin/env bash
# KartPad-NX dependency provisioner.
#
#   builder/fetch-deps.sh
#
# Clones the vendored upstream source trees into ref/ at their PINNED commits so
# a fresh clone can be built. ref/ is gitignored (large third-party trees, plus
# GPL/asset entanglement), so this script is the tracked source-of-truth for the
# exact revisions. Idempotent: re-running fetches/verifies without re-cloning.
#
# After this, follow the build steps in README.md (build the kartpad-dawn Docker
# image from switch/docker/Dockerfile.dawn, then builder/build-switch.sh and
# builder/build-fullapp.sh).
#
# The parts of KartPad (GPLv3) this port uses (its WiiCompiled translator patches, recompiler
# configs, profile metadata and translation hooks) are kept in this repository under
# ref/kartpad-main. sse2neon is fetched by build-switch.sh's translate stage at its own pinned
# commit, not here.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
ref="$repo/ref"
mkdir -p "$ref"

# ── Pinned git dependencies: dir | url | commit ─────────────────────────────
# Keep these in sync with the actual working checkout; bump deliberately.
DEPS=(
  "upstream/Wiicompiled|https://github.com/patchzyy/Wiicompiled|1912292c804ff9b1b79938de89369ec4496f9fff"
  "dawn|https://github.com/google/dawn.git|80ee0043018a51532ea0fa2e77496cc66634157e"
  "SDL|https://github.com/libsdl-org/SDL.git|5848e584a1b606de26e3dbd1c7e4ecbc34f807a6"
  "sdl3-switch|https://github.com/neomody77/sdl3-switch.git|182e511214d7600e4bdab8606d7caf0ef744afd6"
)

clone_pin() {
  local dir="$ref/$1" url="$2" commit="$3"
  echo "── $1  ($commit)"
  if [[ ! -d "$dir/.git" ]]; then
    mkdir -p "$(dirname "$dir")"
    git clone "$url" "$dir"
  fi
  if [[ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$commit" ]]; then
    git -C "$dir" fetch --tags origin "$commit" 2>/dev/null || git -C "$dir" fetch --tags origin
    git -C "$dir" checkout --detach "$commit"
  fi
  [[ "$(git -C "$dir" rev-parse HEAD)" == "$commit" ]] \
    || { echo "ERROR: $1 is not pinned at $commit" >&2; exit 1; }
  echo "   ok: $dir"
}

for entry in "${DEPS[@]}"; do
  IFS='|' read -r dir url commit <<<"$entry"
  clone_pin "$dir" "$url" "$commit"
done

# Dawn resolves its own third-party dependencies during the graphics build
# (inside the kartpad-dawn container); this script only pins the top checkout.

# The deko3d renderer needs no Mesa/NVK tree: its shader compiler (UAM) is in switch/uam.
if [[ ! -d "$ref/kartpad-main/patches" ]]; then
  echo "MISSING: ref/kartpad-main (part of this repository; restore it from git)" >&2
  exit 2
fi

echo
echo "All dependencies present and pinned. Next: build the Docker image and run"
echo "builder/build-switch.sh, then builder/build-fullapp.sh (see README.md)."
