#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
"""Host regression tests for v1.0.3 LAN/AP connectivity and Wi-Fi page launch latency.

验证 HTTPD 栈帧上限、NimBLE 停服信号量顺序、Wi-Fi 进页延迟启动与内部 DRAM 预算。
Verify HTTPD stack-frame bounds, NimBLE stop semaphore order, deferred Wi-Fi page startup, and internal DRAM budget.
"""

from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent


def read_text(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def extract_function(source: str, name: str) -> str:
    pattern = re.compile(rf"(?:^|\n)[^\n;]*\b{re.escape(name)}\s*\([^;]*\)\s*\{{", re.MULTILINE)
    match = pattern.search(source)
    assert match, f"function {name} not found"
    start = match.start()
    brace = source.index("{", match.start())
    depth = 0
    for i in range(brace, len(source)):
        ch = source[i]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
    raise AssertionError(f"unterminated function {name}")


def test_httpd_callback_stack_frames() -> None:
    """Ensure HTTPD callbacks and NVS helpers keep stack frames <= 1024 bytes."""
    feeds_src = read_text("main/feeds_store.c")
    webmarks_src = read_text("main/web_bookmarks.c")
    transfer_src = read_text("main/apps/app_transfer.c")

    with tempfile.TemporaryDirectory(prefix="pico-wifi-frames-") as tmp:
        tmp_path = Path(tmp)

        # 1. feeds_store_upsert / feeds_store_delete / feeds_store_list_public
        feeds_unit = "\n".join([
            "#include <stdbool.h>",
            "#include <stddef.h>",
            "#include <stdint.h>",
            "#include <stdio.h>",
            "#include <stdlib.h>",
            "#include <string.h>",
            '#include "feeds_store.h"',
            "static void *feeds_alloc(size_t n) { return calloc(1, n); }",
            "void derive_source_name(const char *url, char out[FEEDS_NAME_MAX]);",
            extract_function(feeds_src, "feeds_store_upsert"),
            extract_function(feeds_src, "feeds_store_delete"),
            extract_function(feeds_src, "feeds_store_list_public"),
        ])
        feeds_c = tmp_path / "feeds_frames.c"
        feeds_c.write_text(feeds_unit, encoding="utf-8")
        subprocess.run(
            [
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wframe-larger-than=1024",
                f"-I{ROOT / 'main'}",
                "-c", str(feeds_c), "-o", str(tmp_path / "feeds_frames.o"),
            ],
            check=True,
        )

        # 2. web_bookmarks_upsert / web_bookmarks_delete
        webmarks_unit = "\n".join([
            "#include <stdbool.h>",
            "#include <stddef.h>",
            "#include <stdint.h>",
            "#include <stdio.h>",
            "#include <stdlib.h>",
            "#include <string.h>",
            "typedef int esp_err_t;",
            "#define ESP_OK 0",
            "#define ESP_ERR_NO_MEM 0x101",
            "#define ESP_ERR_INVALID_ARG 0x102",
            "#define ESP_ERR_NOT_FOUND 0x105",
            "#define WEB_BOOKMARK_MAX 20",
            "#define WEB_BOOKMARK_TITLE_MAX 64",
            "#define WEB_BOOKMARK_URL_MAX 256",
            "typedef struct { char title[WEB_BOOKMARK_TITLE_MAX]; char url[WEB_BOOKMARK_URL_MAX]; } web_bookmark_t;",
            "void web_bookmarks_load(web_bookmark_t out[WEB_BOOKMARK_MAX], size_t *count_out);",
            "esp_err_t web_bookmarks_save(const web_bookmark_t items[WEB_BOOKMARK_MAX], size_t count);",
            "size_t web_bookmark_find(const web_bookmark_t items[WEB_BOOKMARK_MAX], size_t count, const char *url);",
            "bool web_bookmark_add(web_bookmark_t items[WEB_BOOKMARK_MAX], size_t *count, const char *title, const char *url);",
            "bool web_bookmark_remove(web_bookmark_t items[WEB_BOOKMARK_MAX], size_t *count, const char *url);",
            "bool valid_bookmark_url(const char *url);",
            "void clean_bookmark_title(const char *raw, const char *url, char out[WEB_BOOKMARK_TITLE_MAX]);",
            "static void *web_bookmarks_alloc(size_t n) { return calloc(1, n); }",
            extract_function(webmarks_src, "web_bookmarks_upsert"),
            extract_function(webmarks_src, "web_bookmarks_delete"),
        ])
        webmarks_c = tmp_path / "webmarks_frames.c"
        webmarks_c.write_text(webmarks_unit, encoding="utf-8")
        subprocess.run(
            [
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wframe-larger-than=1024",
                "-c", str(webmarks_c), "-o", str(tmp_path / "webmarks_frames.o"),
            ],
            check=True,
        )

        # 3. transfer_webmarks_list / transfer_feeds_list (called on HTTPD task stack)
        transfer_unit = "\n".join([
            "#include <stdbool.h>",
            "#include <stddef.h>",
            "#include <stdint.h>",
            "#include <stdio.h>",
            "#include <stdlib.h>",
            "#define MALLOC_CAP_SPIRAM 1",
            "#define MALLOC_CAP_8BIT 2",
            "void *heap_caps_calloc(size_t n, size_t sz, int caps);",
            "#define WEB_BOOKMARK_MAX 20",
            "#define WEB_BOOKMARK_TITLE_MAX 64",
            "#define WEB_BOOKMARK_URL_MAX 256",
            "typedef struct { char title[WEB_BOOKMARK_TITLE_MAX]; char url[WEB_BOOKMARK_URL_MAX]; } web_bookmark_t;",
            "typedef struct { char title[64]; char url[256]; } read_pico_webmark_t;",
            "void web_bookmarks_load(web_bookmark_t out[WEB_BOOKMARK_MAX], size_t *count_out);",
            '#include "feeds_store.h"',
            "typedef struct { char name[64]; char url[256]; uint8_t kind; char auth_user[64]; bool has_password; } read_pico_feed_source_t;",
            extract_function(transfer_src, "transfer_webmarks_list"),
            extract_function(transfer_src, "transfer_feeds_list"),
            "size_t entry_w(read_pico_webmark_t *o, size_t c) { return transfer_webmarks_list(o, c, NULL); }",
            "size_t entry_f(read_pico_feed_source_t *o, size_t c) { return transfer_feeds_list(o, c, NULL); }",
        ])
        transfer_c = tmp_path / "transfer_cb_frames.c"
        transfer_c.write_text(transfer_unit, encoding="utf-8")
        subprocess.run(
            [
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wframe-larger-than=1024",
                f"-I{ROOT / 'main'}",
                "-c", str(transfer_c), "-o", str(tmp_path / "transfer_cb_frames.o"),
            ],
            check=True,
        )


def strip_line_comments(code: str) -> str:
    return "\n".join(line.split("//", 1)[0] for line in code.splitlines())


def test_ble_stop_and_wifi_handoff() -> None:
    ble_src = read_text("components/read_pico_ble/read_pico_ble.c")
    host_task = strip_line_comments(extract_function(ble_src, "nimble_host_task"))
    give_pos = host_task.find("xSemaphoreGive(s_host_stopped_sem)")
    deinit_pos = host_task.find("nimble_port_freertos_deinit()")
    assert give_pos != -1 and deinit_pos != -1 and give_pos < deinit_pos, (
        "s_host_stopped_sem must be signaled before nimble_port_freertos_deinit() calls vTaskDelete(NULL)"
    )

    ble_stop = strip_line_comments(extract_function(ble_src, "read_pico_ble_stop"))
    assert "synced && ble_gap_adv_active()" in ble_stop, "guard ble_gap_adv_active() on s_synced"
    assert "vTaskDelay(pdMS_TO_TICKS(20))" in ble_stop, "yield briefly so idle reclaims NimBLE host stack"

    svc_src = read_text("main/ble_service.c")
    is_wifi = strip_line_comments(extract_function(svc_src, "is_wifi_page"))
    assert "&app_feeds" in is_wifi, "app_feeds must be tracked as a Wi-Fi page"
    hold_fn = strip_line_comments(extract_function(svc_src, "app_ble_wifi_hold"))
    assert "start_ble_internal()" not in hold_fn, (
        "app_ble_wifi_hold(false) should defer BLE restart to app_ble_poll() to avoid page-exit stalls and thrash"
    )


def test_deferred_wifi_entry_and_psram_tables() -> None:
    for rel in ("main/apps/app_web.c", "main/apps/app_feeds.c", "main/apps/app_agent.c"):
        src = read_text(rel)
        on_enter = extract_function(src, "on_enter")
        on_tick = extract_function(src, "on_tick")
        assert "network_start();" not in on_enter, f"{rel}: on_enter must defer network_start() to on_tick()"
        assert "s_start_pending = true;" in on_enter, f"{rel}: on_enter must arm s_start_pending"
        assert "s_start_pending" in on_tick and "network_start();" in on_tick, (
            f"{rel}: on_tick must execute deferred network_start()"
        )

    web_src = read_text("main/apps/app_web.c")
    assert "static web_bookmark_t *s_bookmarks;" in web_src
    assert "static web_bookmark_t s_bookmarks[" not in web_src

    feeds_src = read_text("main/apps/app_feeds.c")
    assert "static feed_source_t *s_sources;" in feeds_src
    assert "static feed_source_t s_sources[" not in feeds_src
    assert "static char (*s_nav_urls)[FEEDS_URL_MAX];" in feeds_src
    assert "static char s_nav_urls[" not in feeds_src


def test_transfer_startup_and_sdkconfig() -> None:
    tr_src = read_text("components/read_pico_transfer/read_pico_transfer.c")
    choose_fn = extract_function(tr_src, "choose_saved_network")
    assert "saved->count <= 1" in choose_fn, "single-network STA startup must skip full Wi-Fi scan"

    start_fn = extract_function(tr_src, "read_pico_transfer_start")
    m = re.search(r"http\.stack_size\s*=\s*(\d+)\s*;", start_fn)
    assert m and int(m.group(1)) <= 12288, "HTTPD stack should be <= 12288 bytes once buffers are in PSRAM"
    assert "start failed mode=%s stage=%s" in start_fn, "read_pico_transfer_start must log failing stage and heap"

    for handler, forbidden in (
        ("ambient_config_handler", "char body[4097]"),
        ("ota_upload_handler", "uint8_t buffer[4096]"),
        ("settings_handler", "char body[2049]"),
        ("webmarks_handler", "char query[900]"),
        ("feeds_handler", "char query[900]"),
    ):
        fn = extract_function(tr_src, handler)
        assert forbidden not in fn, f"{handler} must not place {forbidden} on the HTTPD stack"

    for cfg in ("sdkconfig.defaults", "sdkconfig.ci"):
        text = read_text(cfg)
        assert "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512" in text, f"{cfg}: ALWAYSINTERNAL should be 512"
        assert "CONFIG_ESP_MAIN_TASK_STACK_SIZE=18432" in text, f"{cfg}: MAIN_TASK_STACK_SIZE should be 18432"


if __name__ == "__main__":
    test_httpd_callback_stack_frames()
    test_ble_stop_and_wifi_handoff()
    test_deferred_wifi_entry_and_psram_tables()
    test_transfer_startup_and_sdkconfig()
    print("test_wifi_lan_reliability: PASS")
