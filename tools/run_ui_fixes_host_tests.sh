#!/bin/sh
set -eu
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  -Dtime=test_time -Dsettimeofday=test_settimeofday \
  -Itools/ui_fixes_stubs \
  -Imain -Imain/app -Imain/ui -Imain/apps \
  -Icomponents/read_pico_transfer/include \
  tools/ui_fixes_host_test.c \
  main/time_sync.c \
  main/ui/ui_time_edit.c \
  main/ui/ui_gesture.c \
  main/app/app_loop.c \
  main/apps/app_settings.c \
  main/apps/app_focus.c \
  main/apps/app_personal.c \
  main/apps/app_sketch.c \
  -o "$out/ui_fixes_test"
"$out/ui_fixes_test"
