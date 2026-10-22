#!/usr/bin/env bash
# Renders launcher screens on the host and writes PNGs to build/preview/.
# Run inside the kartpad-dawn container from the repo root:
#   docker run --rm -v "$PWD:/project" -w /project kartpad-dawn bash launcher/tools/preview/run-preview.sh
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
L="$repo/launcher"
out="$repo/build/preview"
work=/tmp/mkw-preview

apt-get update -qq >/dev/null 2>&1 || true
apt-get install -y -qq g++ libsdl2-dev libsdl2-ttf-dev libsdl2-image-dev libsdl2-mixer-dev libssl-dev \
  libzstd-dev libcurl4-openssl-dev libsodium-dev zlib1g-dev nlohmann-json3-dev >/dev/null 2>&1

mkdir -p "$work" "$out"
srcs=$(find "$L/source" -name '*.cpp' ! -path '*/app/main.cpp' ! -path '*/app/input.cpp' ! -path '*/stubs/*')
g++ -std=c++20 -O1 -DMKW_PREVIEW -w \
  -I"$L/tools/preview" -I"$L/include" -I"$L/source" $(sdl2-config --cflags) \
  $srcs "$repo/switch/src/runtime/disc_extractor.cpp" "$repo/switch/src/runtime/disc_reader.cpp" \
  "$repo/switch/src/runtime/wii_cluster.cpp" "$repo/switch/src/runtime/wii_crypto.cpp" \
  "$repo/switch/src/runtime/wii_common_key.cpp" "$repo/switch/src/runtime/wii_fst.cpp" \
  "$L/tools/preview/preview_main.cpp" "$L/tools/preview/preview_aes.cpp" \
  -o "$work/launcher-preview" \
  $(sdl2-config --libs) -lSDL2_ttf -lSDL2_image -lSDL2_mixer -lssl -lcrypto -lzstd -lcurl -lsodium -lz -lpthread

cd "$work"
python3 "$L/tools/preview/make_fixture.py" "$L"
mkdir -p build/preview

shoot() {  # name script
  MKW_PREVIEW_SCRIPT="$2" ./launcher-preview >/dev/null
}
shoot play "shot:play"
shoot play-rr "right,shot:play-rr"
shoot saves "R,shot:saves"
shoot licenses "R,R,shot:licenses"
shoot license-edit "R,R,down,A,shot:license-edit"
shoot miis "R,R,R,shot:miis"
shoot mii-edit "R,R,R,down,A,shot:mii-edit"
shoot mii-face "R,R,R,down,A,right,A,shot:mii-face"
shoot mii-eyes "R,R,R,down,A,right,right,right,right,A,shot:mii-eyes"
shoot mii-glasses "R,R,R,down,A,down,right,right,A,down,A,right,right,A,shot:mii-glasses"
shoot files "R,R,R,R,shot:files"
shoot settings "L,L,shot:settings"
shoot updates "L,shot:updates"
MKW_PREVIEW_APPLET=1 shoot applet "shot:applet"
cp build/preview/*.png "$out/"
ls "$out"
