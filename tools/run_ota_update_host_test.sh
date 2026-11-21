#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out="${TMPDIR:-/tmp}/read-pico-ota-policy-test"
cc -std=c11 -Wall -Wextra -Werror -I"$root/components/read_pico_transfer/include" \
  "$root/tools/ota_update_host_test.c" -o "$out"
"$out"
python3 "$root/tools/test_ota_release_metadata.py"
