#!/usr/bin/env bash
# Build and run every launcher host test.
#
#   bash launcher/tests/run-host-tests.sh [work-dir]
#
# The tests use relative "sdmc:/..." and "test_romfs/" paths. Each test is
# built and run in its own fresh directory under work-dir (default: a new temp
# directory), so the source tree is never written to.
#
# Needs g++ plus libsodium, OpenSSL (libcrypto), libcurl, libzstd and zlib headers.
set -euo pipefail

tests="$(cd "$(dirname "$0")" && pwd)"
launcher="$(dirname "$tests")"
src="$launcher/source"
work="${1:-$(mktemp -d)}"
mkdir -p "$work"
work="$(cd "$work" && pwd)"

cxx=(g++ -std=c++20 -Wno-deprecated-declarations -I"$launcher/include" -I"$src" -I"$tests")
failed=()

# run_test NAME [setup-function] -- compiler arguments...
run_test() {
  local name=$1 setup=$2
  shift 3
  local dir
  dir="$(mktemp -d "$work/$name.XXXXXX")"
  echo "==> $name ($dir)"
  if ! "${cxx[@]}" "$@" -o "$dir/$name"; then
    failed+=("$name (build)")
    return
  fi
  [[ -z "$setup" ]] || (cd "$dir" && "$setup")
  if ! (cd "$dir" && "./$name"); then
    failed+=("$name")
  fi
}

# payload_test reads its manifest from the fake RomFS; it writes the matching
# test.nro.zst itself.
payload_fixture() {
  mkdir -p test_romfs/payload
  cat > test_romfs/payload/payloads.json <<'EOF'
{"schema":1,"payloads":[{"variant":"vanilla","file":"test.nro.zst","raw_size":13,"raw_sha256":"096c0a72c31f9a2d65126d8e8a401a2ab2f2e21d0a282a6ffe6642bbef65ffd9","zst_size":26,"build_stamp":"test"}]}
EOF
}

run_test payload_test payload_fixture -- -DROMFS_PREFIX='"test_romfs/"' \
  "$tests/payload_test.cpp" "$src/payload/payload_service.cpp" "$src/app/self_path.cpp" \
  "$tests/switch_mock.cpp" -lzstd -lcrypto -Wl,--wrap=fwrite

run_test manifest_test "" -- -DMKW_UPDATER_TEST_MOCKS \
  "$tests/manifest_test.cpp" "$src/net/update_service.cpp" "$src/app/self_path.cpp" "$tests/switch_mock.cpp" \
  -lsodium -lcrypto

run_test disc_service_test "" -- \
  "$tests/disc_service_test.cpp" "$src/disc/disc_service.cpp" "$src/data/game_paths.cpp" "$tests/stubs.cpp" \
  "$tests/switch_mock.cpp" -lcrypto

run_test save_service_test "" -- \
  "$tests/save_service_test.cpp" "$src/data/save_service.cpp" "$src/data/game_paths.cpp"

data_sources=("$src/data/license_service.cpp" "$src/data/save_service.cpp" "$src/data/mii_service.cpp" "$src/data/mii_record.cpp"
  "$src/data/game_paths.cpp" "$src/data/wiinand/miidb.cpp" "$src/data/switch_mii.cpp")

run_test profiles_test "" -- "$tests/profiles_test.cpp" "${data_sources[@]}" -lz

# On the console "sdmc:" is a mounted root that always exists; fsx::MakeDirs
# never creates the device component, so give the host run the same root.
sd_root() { mkdir -p "sdmc:"; }

run_test mii_create_test sd_root -- "$tests/mii_create_test.cpp" "${data_sources[@]}" -lz

run_test switch_mii_test "" -- "$tests/switch_mii_test.cpp" "$src/data/switch_mii.cpp" "$src/data/mii_record.cpp"

if ((${#failed[@]})); then
  echo "FAILED: ${failed[*]}" >&2
  exit 1
fi
echo "All launcher host tests passed."
