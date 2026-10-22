#!/usr/bin/env bash
# Keep the linker's random-access ELF writes on the container filesystem.
# Copy the completed image to the Windows bind mount only after a successful link.
set -euo pipefail
driver="$1"
shift
args=("$@")
output=""
index=-1
for ((i=0;i<${#args[@]};++i)); do
  if [[ "${args[i]}" == -o ]]; then
    index=$((i+1))
    output="${args[index]}"
    break
  fi
done
if [[ -z "$output" ]]; then
  exec "$driver" "${args[@]}"
fi
link_tmp="$(mktemp -d /tmp/kartpad-link.XXXXXX)"
trap 'rm -rf "$link_tmp"' EXIT
args[index]="$link_tmp/image.elf"
"$driver" "${args[@]}"
cp "$link_tmp/image.elf" "$output"
