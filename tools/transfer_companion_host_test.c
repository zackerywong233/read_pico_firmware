/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：配套客户端 JSON 协议、图片头校验与便签文件名校验的宿主测试。
 *
 * English: Host tests for companion JSON codecs, image header validation and sketch filename rules.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "transfer_companion_image.h"
#include "transfer_companion_policy.h"

static void write_bytes(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    assert(f);
    assert(fwrite(data, 1, len, f) == len);
    assert(fclose(f) == 0);
}

static void make_minimal_png(uint8_t out[45], uint32_t w, uint32_t h) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    memset(out, 0, 45);
    memcpy(out, sig, 8);
    out[11] = 13;
    memcpy(out + 12, "IHDR", 4);
    out[16] = (uint8_t)(w >> 24);
    out[17] = (uint8_t)(w >> 16);
    out[18] = (uint8_t)(w >> 8);
    out[19] = (uint8_t)w;
    out[20] = (uint8_t)(h >> 24);
    out[21] = (uint8_t)(h >> 16);
    out[22] = (uint8_t)(h >> 8);
    out[23] = (uint8_t)h;
    out[24] = 8; // bit depth
    memcpy(out + 37, "IEND", 4);
}

static void test_settings_codec(void) {
    read_pico_device_settings_t s = {
        .sleep_mode = 1,
        .auto_sleep_sec = 120,
        .pickup_wake = false,
        .book_px = 48,
        .book_refresh = 1,
        .book_line_height = 1,
        .book_margin = 1,
        .book_weight = 0,
        .book_rotation = 0,
        .book_gesture = 0,
        .saver_style = 0,
        .dashboard_items = 15,
        .timezone_min = 480,
        .charge_led = true,
        .low_soc_notify_percent = 10,
        .muted = false,
        .font_path = "/sdcard/fonts/ChillDuanSansVF.ttf",
    };

    cJSON *json = companion_settings_to_json(&s);
    assert(json);
    assert(cJSON_GetObjectItemCaseSensitive(json, "book_px")->valueint == 48);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(json, "font_path")->valuestring,
                   "/sdcard/fonts/ChillDuanSansVF.ttf"));
    assert(cJSON_GetObjectItemCaseSensitive(json, "vcom") == NULL);
    cJSON_Delete(json);

    const char *err = NULL;
    const char *valid_patch =
        "{\"sleep_mode\":0,\"auto_sleep_sec\":300,\"pickup_wake\":true,"
        "\"book_px\":56,\"book_refresh\":2,\"book_line_height\":0,\"book_margin\":2,"
        "\"book_weight\":500,\"book_rotation\":1,\"book_gesture\":2,\"saver_style\":2,"
        "\"dashboard_items\":5,\"timezone_min\":-300,\"charge_led\":false,"
        "\"low_soc_notify_percent\":20,\"muted\":true,\"transfer_pin_required\":true,"
        "\"ble_enabled\":true,\"ble_pin_required\":true}";
    assert(companion_settings_patch_json(&s, valid_patch, strlen(valid_patch), &err));
    assert(s.sleep_mode == 0 && s.auto_sleep_sec == 300 && s.pickup_wake);
    assert(s.book_px == 56 && s.book_refresh == 2 && s.book_line_height == 0 && s.book_margin == 2);
    assert(s.book_weight == 500 && s.book_rotation == 1 && s.book_gesture == 2 && s.saver_style == 2);
    assert(s.dashboard_items == 5 && s.timezone_min == -300 && !s.charge_led);
    assert(s.low_soc_notify_percent == 20 && s.muted && s.transfer_pin_required &&
           s.ble_enabled && s.ble_pin_required);
    assert(!strcmp(s.font_path, "/sdcard/fonts/ChillDuanSansVF.ttf"));

    // 任意字段失败时不得发生部分修改。/ Rejections must leave the struct untouched.
    read_pico_device_settings_t snapshot = s;
    const char *bad_cases[] = {
        "{}",
        "[]",
        "not json",
        "{\"muted\":true,\"vcom\":1500}",
        "{\"muted\":false,\"font_path\":\"\"}",
        "{\"book_px\":50}",
        "{\"book_px\":48.5}",
        "{\"auto_sleep_sec\":45}",
        "{\"book_weight\":250}",
        "{\"low_soc_notify_percent\":7}",
        "{\"timezone_min\":900}",
        "{\"sleep_mode\":3}",
        "{\"dashboard_items\":16}",
        "{\"muted\":1}",
        "{\"pickup_wake\":\"true\"}",
        "{\"unknown_key\":1}",
        "{\"muted\":false}trailing",
    };
    for (size_t i = 0; i < sizeof(bad_cases) / sizeof(bad_cases[0]); ++i) {
        err = NULL;
        assert(!companion_settings_patch_json(&s, bad_cases[i], strlen(bad_cases[i]), &err));
        assert(err && err[0]);
        assert(memcmp(&s, &snapshot, sizeof(s)) == 0);
    }
}

static void test_storage_target(void) {
    bool flash = false;
    assert(companion_parse_storage_target("{\"target\":\"flash\"}", 18, &flash) && flash);
    assert(companion_parse_storage_target("{\"target\":\"sd\"}", 15, &flash) && !flash);
    assert(!companion_parse_storage_target("{\"target\":\"usb\"}", 16, &flash));
    assert(!companion_parse_storage_target("{\"target\":\"sd\",\"extra\":1}", 25, &flash));
    assert(!companion_parse_storage_target("{}", 2, &flash));
    assert(!companion_parse_storage_target("[]", 2, &flash));
}

static void test_image_and_sketch_validation(void) {
    char dir[] = "/tmp/pico-companion-img-XXXXXX";
    assert(mkdtemp(dir));
    char path[256];
    snprintf(path, sizeof(path), "%s/sample.bin", dir);

    uint8_t png[45];
    make_minimal_png(png, 684, 1216);
    write_bytes(path, png, sizeof(png));
    transfer_image_format_t fmt = TRANSFER_IMAGE_INVALID;
    assert(valid_image_file(path, &fmt) && fmt == TRANSFER_IMAGE_PNG);

    // 宽为 0 或超过 8192 须拒绝。/ Zero or oversized PNG dimensions must fail.
    make_minimal_png(png, 0, 100);
    write_bytes(path, png, sizeof(png));
    assert(!valid_image_file(path, &fmt));
    make_minimal_png(png, 8193, 100);
    write_bytes(path, png, sizeof(png));
    assert(!valid_image_file(path, &fmt));

    // 截断 PNG 须拒绝。/ Truncated PNG must fail.
    make_minimal_png(png, 100, 100);
    write_bytes(path, png, 20);
    assert(!valid_image_file(path, &fmt));

    // 合法 JPEG 与非法 JPEG 头。/ Valid JPEG vs invalid JPEG header.
    uint8_t jpg[32] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F'};
    write_bytes(path, jpg, sizeof(jpg));
    assert(valid_image_file(path, &fmt) && fmt == TRANSFER_IMAGE_JPEG);
    jpg[3] = 0x00;
    write_bytes(path, jpg, sizeof(jpg));
    assert(!valid_image_file(path, &fmt));
    unlink(path);
    rmdir(dir);

    char name[121];
    assert(decode_sketch_name("sketch_12345_1.png", name) && !strcmp(name, "sketch_12345_1.png"));
    assert(decode_sketch_name("%E4%BE%BF%E7%AD%BE.PNG", name) && !strcmp(name, "便签.PNG"));
    assert(!decode_sketch_name("sketch.jpg", name));
    assert(!decode_sketch_name("..%2Fsketch.png", name));
    assert(!decode_sketch_name("sub%2Fsketch.png", name));
    assert(!decode_sketch_name(".hidden.png", name));
    assert(!decode_sketch_name(".png", name));
    assert(!decode_sketch_name("bad%00.png", name));
}

static void test_time_font_dashboard_pin_sha(void) {
    uint32_t unix_sec = 0;
    bool has_tz = false;
    int16_t tz_min = 0;
    const char *t1 = "{\"unix_sec\":1763724000}";
    assert(companion_parse_time_sync(t1, strlen(t1), &unix_sec, &has_tz, &tz_min));
    assert(unix_sec == 1763724000U && !has_tz);
    const char *t2 = "{\"unix_sec\":1763724000,\"timezone_min\":480}";
    assert(companion_parse_time_sync(t2, strlen(t2), &unix_sec, &has_tz, &tz_min));
    assert(unix_sec == 1763724000U && has_tz && tz_min == 480);
    assert(!companion_parse_time_sync("{\"unix_sec\":100}", 16, &unix_sec, &has_tz, &tz_min));
    assert(!companion_parse_time_sync("{\"timezone_min\":480}", 20, &unix_sec, &has_tz, &tz_min));
    assert(!companion_parse_time_sync("{\"unix_sec\":1763724000,\"timezone_min\":999}", 43, &unix_sec, &has_tz, &tz_min));
    assert(!companion_parse_time_sync("{\"unix_sec\":1763724000,\"x\":1}", 29, &unix_sec, &has_tz, &tz_min));

    bool builtin = false;
    char font_name[121] = {0};
    assert(companion_parse_font_select("{\"builtin\":true}", 16, &builtin, font_name) && builtin);
    assert(companion_parse_font_select("{\"name\":\"\"}", 11, &builtin, font_name) && builtin);
    assert(companion_parse_font_select("{\"name\":\"MyFont.ttf\"}", 21, &builtin, font_name) && !builtin && !strcmp(font_name, "MyFont.ttf"));
    assert(!companion_parse_font_select("{\"builtin\":false}", 17, &builtin, font_name));
    assert(!companion_parse_font_select("{\"name\":\"a.ttf\",\"extra\":1}", 26, &builtin, font_name));

    char note[TRANSFER_DASHBOARD_NOTE_MAX + 1];
    const char *n1 = "{\"note\":\"第一行\\r\\n第二行\\n第三行\\n第四行\"}";
    assert(companion_parse_dashboard_note(n1, strlen(n1), note));
    assert(!strcmp(note, "第一行\n第二行\n第三行\n第四行"));
    const char *n_empty = "{\"note\":\"\"}";
    assert(companion_parse_dashboard_note(n_empty, strlen(n_empty), note) && note[0] == '\0');
    const char *n_5lines = "{\"note\":\"1\\n2\\n3\\n4\\n5\"}";
    assert(!companion_parse_dashboard_note(n_5lines, strlen(n_5lines), note));
    assert(!companion_parse_dashboard_note("{\"note\":\"a\",\"x\":1}", 18, note));

    char pin[7];
    assert(companion_pin_valid_format("012345"));
    assert(!companion_pin_valid_format("12345"));
    assert(!companion_pin_valid_format("1234567"));
    assert(!companion_pin_valid_format("12345a"));
    assert(companion_pin_equals("042918", "042918"));
    assert(!companion_pin_equals("042918", "042919"));
    assert(companion_parse_pin_body("{\"pin\":\"654321\"}", 16, pin) && !strcmp(pin, "654321"));
    assert(!companion_parse_pin_body("{\"pin\":\"65432a\"}", 16, pin));
    assert(!companion_parse_pin_body("{\"pin\":\"654321\",\"x\":1}", 22, pin));

    char sha[65];
    const char *upper = "AABBCCDDEEFF00112233445566778899AABBCCDDEEFF00112233445566778899";
    assert(companion_normalize_sha256(upper, sha));
    assert(!strcmp(sha, "aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899"));
    const char *prefixed = "sha256:0123456789ABCDEF0123456789abcdef0123456789ABCDEF0123456789abcdef";
    assert(companion_normalize_sha256(prefixed, sha));
    assert(!strcmp(sha, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    assert(!companion_normalize_sha256("short", sha));
    assert(!companion_normalize_sha256("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz", sha));
}

static void test_progress_notes_stats_restore_codecs(void) {
    // 1) 逐书阅读进度恢复解析 / Per-book progress restore parser.
    read_pico_book_progress_restore_t pr = {0};
    const char *err = NULL;
    const char *p1 = "{\"percent\":42,\"chapter\":5,\"byte_off\":12040,\"px\":52,\"file_size\":654321,\"set_last\":true}";
    assert(companion_parse_book_progress_restore(p1, strlen(p1), &pr, &err));
    assert(pr.percent == 42 && pr.chapter == 5 && pr.byte_off == 12040U);
    assert(pr.has_px && pr.px == 52 && pr.has_file_size && pr.file_size == 654321U && pr.set_last);

    const char *p2 = "{\"percent\":0,\"chapter\":0,\"byte_off\":0}";
    assert(companion_parse_book_progress_restore(p2, strlen(p2), &pr, &err));
    assert(pr.percent == 0 && !pr.has_px && !pr.has_file_size && !pr.set_last);

    const char *bad_progress[] = {
        "{}",
        "{\"percent\":42,\"chapter\":1}",
        "{\"percent\":101,\"chapter\":1,\"byte_off\":0}",
        "{\"percent\":10,\"chapter\":70000,\"byte_off\":0}",
        "{\"percent\":10,\"chapter\":1,\"byte_off\":-1}",
        "{\"percent\":10,\"chapter\":1,\"byte_off\":0,\"px\":50}",
        "{\"percent\":10,\"chapter\":1,\"byte_off\":0,\"set_last\":1}",
        "{\"percent\":10,\"chapter\":1,\"byte_off\":0,\"unknown\":1}",
        "{\"percent\":10,\"percent\":20,\"chapter\":1,\"byte_off\":0}",
    };
    for (size_t i = 0; i < sizeof(bad_progress) / sizeof(bad_progress[0]); ++i) {
        assert(!companion_parse_book_progress_restore(bad_progress[i], strlen(bad_progress[i]), &pr, &err));
        assert(err && err[0]);
    }

    // 2) 书签与摘录恢复解析（含 GET /notes?format=json 原样回传与空数组清空）
    // Bookmarks & excerpts restore parser (including GET round-trip fields and empty clear).
    bool has_size = false;
    uint32_t file_size = 0;
    cJSON *root = NULL;
    read_pico_book_note_item_t *items = NULL;
    size_t count = 0;
    const char *n_ok =
        "{\"book\":\"三体.epub\",\"file_size\":98765,\"count\":2,\"items\":["
        "{\"index\":0,\"kind\":\"bookmark\",\"chapter\":1,\"offset\":512,\"percent\":15},"
        "{\"index\":1,\"kind\":\"excerpt\",\"chapter\":2,\"offset\":1024,\"percent\":30,\"text\":\"给岁月以文明\\n而不是给文明以岁月\",\"truncated\":false}"
        "]}";
    assert(companion_parse_book_notes_restore(n_ok, strlen(n_ok), &has_size, &file_size, &root, &items, &count, &err));
    assert(has_size && file_size == 98765U && count == 2 && items != NULL && root != NULL);
    assert(!items[0].is_excerpt && items[0].chapter == 1 && items[0].byte_off == 512U && items[0].percent == 15 && !items[0].text);
    assert(items[1].is_excerpt && items[1].chapter == 2 && items[1].byte_off == 1024U && items[1].percent == 30);
    assert(items[1].text && !strcmp(items[1].text, "给岁月以文明\n而不是给文明以岁月"));
    free(items);
    cJSON_Delete(root);

    const char *n_empty = "{\"items\":[]}";
    assert(companion_parse_book_notes_restore(n_empty, strlen(n_empty), &has_size, &file_size, &root, &items, &count, &err));
    assert(!has_size && count == 0 && items == NULL && root != NULL);
    cJSON_Delete(root);

    const char *bad_notes[] = {
        "{}",
        "{\"items\":{},\"count\":0}",
        "{\"items\":[],\"count\":1}",
        "{\"items\":[{\"kind\":\"bookmark\",\"chapter\":0,\"offset\":0,\"percent\":10,\"text\":\"nope\"}]}",
        "{\"items\":[{\"kind\":\"excerpt\",\"chapter\":0,\"offset\":0,\"percent\":10}]}",
        "{\"items\":[{\"kind\":\"excerpt\",\"chapter\":0,\"offset\":0,\"percent\":10,\"text\":\"\"}]}",
        "{\"items\":[{\"kind\":\"note\",\"chapter\":0,\"offset\":0,\"percent\":10}]}",
        "{\"items\":[{\"kind\":\"bookmark\",\"chapter\":0,\"offset\":0,\"percent\":101}]}",
        "{\"items\":[{\"kind\":\"bookmark\",\"chapter\":0,\"offset\":0,\"percent\":10,\"extra\":1}]}",
        "{\"items\":[],\"extra\":1}",
    };
    for (size_t i = 0; i < sizeof(bad_notes) / sizeof(bad_notes[0]); ++i) {
        root = NULL; items = NULL; count = 0;
        assert(!companion_parse_book_notes_restore(bad_notes[i], strlen(bad_notes[i]), &has_size, &file_size, &root, &items, &count, &err));
        assert(root == NULL && items == NULL && err && err[0]);
    }

    // 3) 累计阅读统计恢复解析（replace / merge / null 清除 / 只读 last_book_path 兼容）
    // Cumulative reading stats restore parser (replace, merge, null clear, read-only last_book_path).
    read_pico_stats_restore_t sr = {0};
    const char *s1 =
        "{\"mode\":\"merge\",\"total_reading_sec\":7200,"
        "\"last_book\":{\"title\":\"三体.epub\",\"percent\":88},"
        "\"excerpt\":{\"title\":\"三体.epub\",\"text\":\"弱小和无知不是生存的障碍\"},"
        "\"last_book_path\":\"/sdcard/books/三体.epub\"}";
    assert(companion_parse_stats_restore(s1, strlen(s1), &sr, &err));
    assert(sr.merge && sr.has_total_reading_sec && sr.total_reading_sec == 7200U);
    assert(sr.has_last_book && !sr.clear_last_book && !strcmp(sr.last_book_title, "三体.epub") && sr.last_book_percent == 88);
    assert(sr.has_excerpt && !sr.clear_excerpt && !strcmp(sr.excerpt_title, "三体.epub") &&
           !strcmp(sr.excerpt_text, "弱小和无知不是生存的障碍"));

    const char *s_clear = "{\"last_book\":null,\"excerpt\":null}";
    assert(companion_parse_stats_restore(s_clear, strlen(s_clear), &sr, &err));
    assert(!sr.merge && !sr.has_total_reading_sec && sr.has_last_book && sr.clear_last_book &&
           sr.has_excerpt && sr.clear_excerpt);

    const char *bad_stats[] = {
        "{}",
        "{\"mode\":\"replace\"}",
        "{\"mode\":\"overwrite\",\"total_reading_sec\":10}",
        "{\"total_reading_sec\":-1}",
        "{\"last_book\":{\"title\":\"\",\"percent\":10}}",
        "{\"last_book\":{\"title\":\"a\\nb\",\"percent\":10}}",
        "{\"last_book\":{\"title\":\"a\",\"percent\":101}}",
        "{\"last_book\":{\"title\":\"a\",\"percent\":10,\"extra\":1}}",
        "{\"excerpt\":{\"title\":\"a\",\"text\":\"\"}}",
        "{\"excerpt\":{\"title\":\"a\"}}",
        "{\"unknown\":1,\"total_reading_sec\":10}",
    };
    for (size_t i = 0; i < sizeof(bad_stats) / sizeof(bad_stats[0]); ++i) {
        assert(!companion_parse_stats_restore(bad_stats[i], strlen(bad_stats[i]), &sr, &err));
        assert(err && err[0]);
    }
}

int main(void) {
    test_settings_codec();
    test_storage_target();
    test_image_and_sketch_validation();
    test_time_font_dashboard_pin_sha();
    test_progress_notes_stats_restore_codecs();
    puts("transfer_companion_host_test: PASS");
    return 0;
}
