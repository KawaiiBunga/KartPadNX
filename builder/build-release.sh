#!/usr/bin/env bash
# Build and sign a public MKW Launcher release.
#
#   builder/build-release.sh <tag> [--channel stable|beta] [--notes-file FILE]
#
# A public release is the game-free launcher only (nopayload mode). It never
# bundles locally compiled game NROs: those are derived from the user's own
# disc and stay local (see docs/DISTRIBUTION.md). For a local launcher with
# your own games packed in, run `builder/build-launcher.sh payload build`; that
# output is not a release artifact.
#
# Steps:
#   1. Build the launcher in nopayload mode (overwrites build/launcher/mkw-launcher.nro).
#   2. Read the packaged NRO's RomFS and fail if it contains any game payload.
#   3. Copy it to build/release/<tag>/, write manifest.json, sign it
#      (manifest.json.sig) and verify against launcher/source/net/release_key.h.
#   4. Print the `gh release create` command. Nothing is uploaded.
#
# Runs on the host and starts the kartpad-dawn container for the launcher build.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"

usage() {
  echo "Usage: $0 <tag> [--channel stable|beta] [--notes-file FILE]" >&2
  echo "Example: $0 v1.0.0" >&2
  exit 64
}

[[ $# -ge 1 ]] || usage
tag="$1"
shift
channel="stable"
notes_file=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --channel) channel="${2:?missing channel argument}"; shift 2 ;;
    --notes-file) notes_file="${2:?missing notes file argument}"; shift 2 ;;
    *) echo "ERROR: Unknown option '$1'" >&2; usage ;;
  esac
done

version="${tag#v}"
release_dir="$repo/build/release/$tag"
private_key="$repo/private/release/mkw-release-ed25519.key"

if [[ ! -f "$private_key" ]]; then
  echo "ERROR: Private release key missing: $private_key" >&2
  exit 1
fi
mkdir -p "$release_dir"

if [[ -z "$notes_file" ]]; then
  notes_file="$release_dir/release-notes.txt"
  cat > "$notes_file" <<EOF
MKW Launcher $tag

- Vanilla MKW and Retro Rewind launcher
- On-device disc extractor (WBFS / ISO)
- Ed25519-signed update manifest

The games are built locally from your own disc; this release contains no game code.
EOF
fi

echo "==> [1/4] Building the game-free launcher (nopayload)..."
MSYS_NO_PATHCONV=1 docker run --rm -v "$repo:/project" -w /project -e DEVKITPRO=/opt/devkitpro \
  kartpad-dawn bash builder/build-launcher.sh nopayload build

echo "==> [2/4] Checking the packaged RomFS for game payloads..."
python3 "$repo/builder/list-nro-romfs.py" "$repo/build/launcher/mkw-launcher.nro" --game-free

launcher_nro="$release_dir/mkw-launcher.nro"
manifest_json="$release_dir/manifest.json"
manifest_sig="$release_dir/manifest.json.sig"
cp "$repo/build/launcher/mkw-launcher.nro" "$launcher_nro"

echo "==> [3/4] Writing and signing $manifest_json..."
python3 "$repo/builder/make-manifest.py" "$launcher_nro" "$version" \
  --tag "$tag" --channel "$channel" --changelog "$notes_file" --output "$manifest_json"
python3 "$repo/builder/sign-manifest.py" "$manifest_json" --key "$private_key" --output "$manifest_sig"
python3 "$repo/builder/sign-manifest.py" "$manifest_json" --output "$manifest_sig" --verify-only

echo "==> [4/4] Release files for $tag are in $release_dir:"
echo "    Launcher NRO: $launcher_nro ($(stat -c %s "$launcher_nro") bytes)"
echo "    Manifest:     $manifest_json"
echo "    Signature:    $manifest_sig"
echo ""
echo "To publish (not run by this script):"
echo "  gh release create $tag \"$launcher_nro\" \"$manifest_json\" \"$manifest_sig\" \\"
echo "    --title \"MKW Launcher $tag\" --notes-file \"$notes_file\""
