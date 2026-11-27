#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
tmp=${TMPDIR:-/tmp}/read-pico-ambient-test-$$
trap 'rm -f "$tmp"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I"$repo/main" \
  "$repo/tools/ambient_sync_host_test.c" "$repo/main/ambient_sync.c" -o "$tmp"
"$tmp"
