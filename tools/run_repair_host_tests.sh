#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 本次修复的独立主机回归。/ Standalone host regressions for this repair set.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/book-tests
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Itools/html_text_stubs -Imain/book \
    tools/html_text_host_test.c main/book/html_text.c -o build/book-tests/html
build/book-tests/html
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Itools/reading_stats_stubs -Itools/agent_config_stubs -Imain \
    tools/reading_stats_host_test.c main/reading_stats.c -o build/book-tests/reading_stats
build/book-tests/reading_stats
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Icomponents/read_pico_transfer tools/clip_text_host_test.c -o build/book-tests/clip_text
build/book-tests/clip_text
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Itools/agent_config_stubs -Icomponents/read_pico_transfer/include \
    -Icomponents/read_pico_transfer tools/agent_config_store_host_test.c \
    -o build/book-tests/agent_config
build/book-tests/agent_config
node --check web/localize-installer.js
node tools/localize_installer_host_test.js
node tools/transfer_web_host_test.js
