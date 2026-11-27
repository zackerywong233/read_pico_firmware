/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：配套客户端接口的 JSON 编解码与参数边界校验，不含平台依赖（HTTP 与 BLE 共用）。
 *
 * English: JSON codecs and strict boundary validation for companion endpoints without platform dependencies (shared by HTTP and BLE).
 *
 * 冻结：设置更新采用整包原子校验；未知键、越界值与只读字段（如 font_path、vcom）一律拒绝。
 * Frozen: Settings patches validate atomically; unknown keys, out-of-range values and read-only fields (font_path, vcom) are rejected.
 */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "read_pico_transfer_companion.h"

#define TRANSFER_DASHBOARD_NOTE_MAX 1024U
#define TRANSFER_DASHBOARD_NOTE_LINES 4U
#define TRANSFER_TIME_MIN_VALID 946684800U
#define TRANSFER_TIME_MAX_VALID 4102444799U
#define TRANSFER_FONT_LIST_MAX 64U
#define TRANSFER_NOTES_BODY_MAX 131072U
#define TRANSFER_NOTES_ITEMS_MAX 512U
#define TRANSFER_NOTE_TEXT_MAX 4096U
#define TRANSFER_STATS_TITLE_MAX 127U
#define TRANSFER_STATS_EXCERPT_MAX 300U

static inline bool companion_json_int_range(const cJSON *item, int min_v, int max_v, int *out) {
    if (!item || !cJSON_IsNumber(item)) return false;
    double v = item->valuedouble;
    if (!isfinite(v) || floor(v) != v || v < (double)min_v || v > (double)max_v) return false;
    *out = (int)v;
    return true;
}

static inline bool companion_json_u32_range(const cJSON *item, uint32_t min_v, uint32_t max_v, uint32_t *out) {
    if (!item || !cJSON_IsNumber(item)) return false;
    double v = item->valuedouble;
    if (!isfinite(v) || floor(v) != v || v < (double)min_v || v > (double)max_v) return false;
    *out = (uint32_t)v;
    return true;
}

static inline bool companion_auto_sleep_valid(int seconds) {
    switch (seconds) {
        case 0:
        case 30:
        case 60:
        case 120:
        case 300:
        case 600:
        case 900:
        case 1800:
        case 3600:
            return true;
        default:
            return false;
    }
}

static inline cJSON *companion_settings_to_json(const read_pico_device_settings_t *s) {
    if (!s) return NULL;
    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    cJSON_AddNumberToObject(json, "sleep_mode", s->sleep_mode);
    cJSON_AddNumberToObject(json, "auto_sleep_sec", s->auto_sleep_sec);
    cJSON_AddBoolToObject(json, "pickup_wake", s->pickup_wake);
    cJSON_AddNumberToObject(json, "book_px", s->book_px);
    cJSON_AddNumberToObject(json, "book_refresh", s->book_refresh);
    cJSON_AddNumberToObject(json, "book_line_height", s->book_line_height);
    cJSON_AddNumberToObject(json, "book_margin", s->book_margin);
    cJSON_AddNumberToObject(json, "book_weight", s->book_weight);
    cJSON_AddNumberToObject(json, "book_rotation", s->book_rotation);
    cJSON_AddNumberToObject(json, "book_gesture", s->book_gesture);
    cJSON_AddBoolToObject(json, "book_top_bar", s->book_top_bar);
    cJSON_AddBoolToObject(json, "book_bottom_bar", s->book_bottom_bar);
    cJSON_AddNumberToObject(json, "book_para_gap", s->book_para_gap);
    cJSON_AddNumberToObject(json, "book_indent", s->book_indent);
    cJSON_AddNumberToObject(json, "book_shelf_view", s->book_shelf_view);
    cJSON_AddNumberToObject(json, "book_cbz_dir", s->book_cbz_dir);
    cJSON_AddBoolToObject(json, "book_cbz_dither", s->book_cbz_dither);
    cJSON_AddNumberToObject(json, "saver_style", s->saver_style);
    cJSON_AddNumberToObject(json, "dashboard_items", s->dashboard_items);
    cJSON_AddNumberToObject(json, "timezone_min", s->timezone_min);
    cJSON_AddBoolToObject(json, "charge_led", s->charge_led);
    cJSON_AddNumberToObject(json, "low_soc_notify_percent", s->low_soc_notify_percent);
    cJSON_AddBoolToObject(json, "muted", s->muted);
    cJSON_AddBoolToObject(json, "transfer_pin_required", s->transfer_pin_required);
    cJSON_AddBoolToObject(json, "ble_enabled", s->ble_enabled);
    cJSON_AddBoolToObject(json, "ble_pin_required", s->ble_pin_required);
    cJSON_AddStringToObject(json, "font_path", s->font_path);
    return json;
}

static inline cJSON *companion_parse_strict_json(const char *body, size_t len) {
    if (!body || !len) return NULL;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(body, len, &end, false);
    if (!root || !end) {
        cJSON_Delete(root);
        return NULL;
    }
    const char *limit = body + len;
    while (end < limit && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    if (end != limit) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static inline bool companion_settings_patch_object(read_pico_device_settings_t *target,
                                                   const cJSON *root,
                                                   const char **err_out) {
    if (err_out) *err_out = "无效的设置 JSON";
    if (!target || !root || !cJSON_IsObject(root) || !root->child) return false;

    read_pico_device_settings_t next = *target;
    bool ok = true;
    const char *err = "无效的设置参数";

    for (const cJSON *item = root->child; item && ok; item = item->next) {
        const char *key = item->string;
        if (!key) { ok = false; break; }
        int iv = 0;
        if (!strcmp(key, "sleep_mode")) {
            ok = companion_json_int_range(item, 0, 2, &iv);
            if (ok) next.sleep_mode = (uint8_t)iv;
            else err = "sleep_mode 须为 0..2";
        } else if (!strcmp(key, "auto_sleep_sec")) {
            ok = companion_json_int_range(item, 0, 3600, &iv) && companion_auto_sleep_valid(iv);
            if (ok) next.auto_sleep_sec = (uint16_t)iv;
            else err = "auto_sleep_sec 不在允许选项中";
        } else if (!strcmp(key, "pickup_wake")) {
            ok = cJSON_IsBool(item);
            if (ok) next.pickup_wake = cJSON_IsTrue(item);
            else err = "pickup_wake 须为布尔值";
        } else if (!strcmp(key, "book_px")) {
            ok = companion_json_int_range(item, 36, 72, &iv) && ((iv - 36) % 4 == 0);
            if (ok) next.book_px = (uint8_t)iv;
            else err = "book_px 须为 36..72 且步长为 4";
        } else if (!strcmp(key, "book_refresh")) {
            ok = companion_json_int_range(item, 0, 2, &iv);
            if (ok) next.book_refresh = (uint8_t)iv;
            else err = "book_refresh 须为 0..2";
        } else if (!strcmp(key, "book_line_height")) {
            ok = companion_json_int_range(item, 0, 2, &iv);
            if (ok) next.book_line_height = (uint8_t)iv;
            else err = "book_line_height 须为 0..2";
        } else if (!strcmp(key, "book_margin")) {
            ok = companion_json_int_range(item, 0, 2, &iv);
            if (ok) next.book_margin = (uint8_t)iv;
            else err = "book_margin 须为 0..2";
        } else if (!strcmp(key, "book_weight")) {
            ok = companion_json_int_range(item, 0, 800, &iv) &&
                 (iv == 0 || (iv >= 300 && iv <= 800 && (iv - 300) % 100 == 0));
            if (ok) next.book_weight = (uint16_t)iv;
            else err = "book_weight 须为 0 或 300..800（步长 100）";
        } else if (!strcmp(key, "book_rotation")) {
            ok = companion_json_int_range(item, 0, 2, &iv);
            if (ok) next.book_rotation = (uint8_t)iv;
            else err = "book_rotation 须为 0..2";
        } else if (!strcmp(key, "book_gesture")) {
            ok = companion_json_int_range(item, 0, 2, &iv);
            if (ok) next.book_gesture = (uint8_t)iv;
            else err = "book_gesture 须为 0..2";
        } else if (!strcmp(key, "book_top_bar")) {
            ok = cJSON_IsBool(item);
            if (ok) next.book_top_bar = cJSON_IsTrue(item);
            else err = "book_top_bar 须为布尔值";
        } else if (!strcmp(key, "book_bottom_bar")) {
            ok = cJSON_IsBool(item);
            if (ok) next.book_bottom_bar = cJSON_IsTrue(item);
            else err = "book_bottom_bar 须为布尔值";
        } else if (!strcmp(key, "book_para_gap")) {
            ok = companion_json_int_range(item, 0, 2, &iv);
            if (ok) next.book_para_gap = (uint8_t)iv;
            else err = "book_para_gap 须为 0..2";
        } else if (!strcmp(key, "book_indent")) {
            ok = companion_json_int_range(item, 0, 1, &iv);
            if (ok) next.book_indent = (uint8_t)iv;
            else err = "book_indent 须为 0..1";
        } else if (!strcmp(key, "book_shelf_view")) {
            ok = companion_json_int_range(item, 0, 1, &iv);
            if (ok) next.book_shelf_view = (uint8_t)iv;
            else err = "book_shelf_view 须为 0..1";
        } else if (!strcmp(key, "book_cbz_dir")) {
            ok = companion_json_int_range(item, 0, 1, &iv);
            if (ok) next.book_cbz_dir = (uint8_t)iv;
            else err = "book_cbz_dir 须为 0..1";
        } else if (!strcmp(key, "book_cbz_dither")) {
            ok = cJSON_IsBool(item);
            if (ok) next.book_cbz_dither = cJSON_IsTrue(item);
            else err = "book_cbz_dither 须为布尔值";
        } else if (!strcmp(key, "saver_style")) {
            ok = companion_json_int_range(item, 0, 4, &iv);
            if (ok) next.saver_style = (uint8_t)iv;
            else err = "saver_style 须为 0..4";
        } else if (!strcmp(key, "dashboard_items")) {
            ok = companion_json_int_range(item, 0, 31, &iv);
            if (ok) next.dashboard_items = (uint8_t)iv;
            else err = "dashboard_items 须为 0..31";
        } else if (!strcmp(key, "timezone_min")) {
            ok = companion_json_int_range(item, -720, 840, &iv);
            if (ok) next.timezone_min = (int16_t)iv;
            else err = "timezone_min 须在 -720..840 分钟内";
        } else if (!strcmp(key, "charge_led")) {
            ok = cJSON_IsBool(item);
            if (ok) next.charge_led = cJSON_IsTrue(item);
            else err = "charge_led 须为布尔值";
        } else if (!strcmp(key, "low_soc_notify_percent")) {
            ok = companion_json_int_range(item, 0, 30, &iv) &&
                 (iv == 0 || (iv >= 5 && iv <= 30 && iv % 5 == 0));
            if (ok) next.low_soc_notify_percent = (uint8_t)iv;
            else err = "low_soc_notify_percent 须为 0 或 5..30（步长 5）";
        } else if (!strcmp(key, "muted")) {
            ok = cJSON_IsBool(item);
            if (ok) next.muted = cJSON_IsTrue(item);
            else err = "muted 须为布尔值";
        } else if (!strcmp(key, "transfer_pin_required")) {
            ok = cJSON_IsBool(item);
            if (ok) next.transfer_pin_required = cJSON_IsTrue(item);
            else err = "transfer_pin_required 须为布尔值";
        } else if (!strcmp(key, "ble_enabled")) {
            ok = cJSON_IsBool(item);
            if (ok) next.ble_enabled = cJSON_IsTrue(item);
            else err = "ble_enabled 须为布尔值";
        } else if (!strcmp(key, "ble_pin_required")) {
            ok = cJSON_IsBool(item);
            if (ok) next.ble_pin_required = cJSON_IsTrue(item);
            else err = "ble_pin_required 须为布尔值";
        } else if (!strcmp(key, "font_path")) {
            ok = false;
            err = "font_path 为只读字段，请在设备字体页切换";
        } else {
            ok = false;
            err = "包含未知或不允许修改的设置字段";
        }
    }

    if (!ok) {
        if (err_out) *err_out = err;
        return false;
    }
    *target = next;
    return true;
}

static inline bool companion_settings_patch_json(read_pico_device_settings_t *target,
                                                 const char *body, size_t len,
                                                 const char **err_out) {
    if (err_out) *err_out = "无效的设置 JSON";
    if (!target || !body || !len) return false;
    cJSON *root = companion_parse_strict_json(body, len);
    if (!root) return false;
    bool ok = companion_settings_patch_object(target, root, err_out);
    cJSON_Delete(root);
    return ok;
}

static inline bool companion_parse_storage_target(const char *body, size_t len, bool *prefer_flash_out) {
    if (!body || !len || !prefer_flash_out) return false;
    cJSON *root = companion_parse_strict_json(body, len);
    if (!root || !cJSON_IsObject(root) || !root->child || root->child->next) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *target = cJSON_GetObjectItemCaseSensitive(root, "target");
    bool ok = false;
    if (cJSON_IsString(target) && target->valuestring) {
        if (!strcmp(target->valuestring, "flash")) {
            *prefer_flash_out = true;
            ok = true;
        } else if (!strcmp(target->valuestring, "sd")) {
            *prefer_flash_out = false;
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

static inline bool companion_parse_time_sync(const char *body, size_t len,
                                             uint32_t *unix_sec_out,
                                             bool *has_tz_out, int16_t *tz_min_out) {
    if (!body || !len || !unix_sec_out || !has_tz_out || !tz_min_out) return false;
    cJSON *root = companion_parse_strict_json(body, len);
    if (!root || !cJSON_IsObject(root) || !root->child) {
        cJSON_Delete(root);
        return false;
    }
    bool has_unix = false, has_tz = false, ok = true;
    uint32_t unix_sec = 0;
    int16_t tz_min = 0;
    for (const cJSON *item = root->child; item && ok; item = item->next) {
        if (!item->string) { ok = false; break; }
        if (!strcmp(item->string, "unix_sec")) {
            ok = !has_unix && companion_json_u32_range(item, TRANSFER_TIME_MIN_VALID, TRANSFER_TIME_MAX_VALID, &unix_sec);
            has_unix = true;
        } else if (!strcmp(item->string, "timezone_min")) {
            int iv = 0;
            ok = !has_tz && companion_json_int_range(item, -720, 840, &iv);
            tz_min = (int16_t)iv;
            has_tz = true;
        } else {
            ok = false;
        }
    }
    cJSON_Delete(root);
    if (!ok || !has_unix) return false;
    *unix_sec_out = unix_sec;
    *has_tz_out = has_tz;
    *tz_min_out = tz_min;
    return true;
}

static inline bool companion_parse_font_select(const char *body, size_t len,
                                               bool *builtin_out, char name_out[121]) {
    if (!body || !len || !builtin_out || !name_out) return false;
    *builtin_out = false;
    name_out[0] = 0;
    cJSON *root = companion_parse_strict_json(body, len);
    if (!root || !cJSON_IsObject(root) || !root->child || root->child->next) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *item = root->child;
    bool ok = false;
    if (item->string && !strcmp(item->string, "builtin") && cJSON_IsBool(item) && cJSON_IsTrue(item)) {
        *builtin_out = true;
        ok = true;
    } else if (item->string && !strcmp(item->string, "name") && cJSON_IsString(item) && item->valuestring) {
        size_t n = strlen(item->valuestring);
        if (n == 0) {
            *builtin_out = true;
            ok = true;
        } else if (n < 121) {
            memcpy(name_out, item->valuestring, n + 1);
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

static inline bool companion_valid_utf8_note(const char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i++];
        if (c < 128) {
            if (c < 32 && c != '\n' && c != '\t') return false;
            if (c == 127) return false;
            continue;
        }
        uint32_t cp;
        unsigned more;
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; more = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; more = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; more = 3; }
        else return false;
        unsigned width = more;
        while (more--) {
            if (i == n || ((unsigned char)s[i] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | ((unsigned char)s[i++] & 63);
        }
        if ((width == 2 && cp < 0x800) || (width == 3 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return false;
    }
    return true;
}

static inline bool companion_normalize_dashboard_note_text(const char *src,
                                                           char note_out[TRANSFER_DASHBOARD_NOTE_MAX + 1]) {
    if (!src || !note_out) return false;
    size_t out_n = 0;
    unsigned lines = 1;
    bool ok = true;
    for (size_t i = 0; src[i]; ++i) {
        char c = src[i];
        if (c == '\r') {
            if (src[i + 1] == '\n') ++i;
            c = '\n';
        }
        if (c == '\n') {
            if (++lines > TRANSFER_DASHBOARD_NOTE_LINES) { ok = false; break; }
        }
        if (out_n >= TRANSFER_DASHBOARD_NOTE_MAX) { ok = false; break; }
        note_out[out_n++] = c;
    }
    note_out[out_n] = 0;
    if (!ok || !companion_valid_utf8_note(note_out, out_n)) {
        note_out[0] = 0;
        return false;
    }
    return true;
}

static inline bool companion_parse_dashboard_note(const char *body, size_t len,
                                                  char note_out[TRANSFER_DASHBOARD_NOTE_MAX + 1]) {
    if (!body || !len || !note_out) return false;
    note_out[0] = 0;
    cJSON *root = companion_parse_strict_json(body, len);
    if (!root || !cJSON_IsObject(root) || !root->child || root->child->next) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "note");
    bool ok = cJSON_IsString(item) && item->valuestring &&
              companion_normalize_dashboard_note_text(item->valuestring, note_out);
    cJSON_Delete(root);
    return ok;
}

static inline bool companion_pin_valid_format(const char *pin) {
    if (!pin) return false;
    for (size_t i = 0; i < 6; ++i) {
        if (pin[i] < '0' || pin[i] > '9') return false;
    }
    return pin[6] == '\0';
}

static inline bool companion_pin_equals(const char expected[7], const char *candidate) {
    if (!companion_pin_valid_format(expected) || !companion_pin_valid_format(candidate))
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < 6; ++i)
        diff |= (unsigned char)(expected[i] ^ candidate[i]);
    return diff == 0;
}

static inline bool companion_parse_pin_body(const char *body, size_t len, char pin_out[7]) {
    if (!body || !len || !pin_out) return false;
    pin_out[0] = 0;
    cJSON *root = companion_parse_strict_json(body, len);
    if (!root || !cJSON_IsObject(root) || !root->child || root->child->next) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, "pin");
    bool ok = cJSON_IsString(item) && companion_pin_valid_format(item->valuestring);
    if (ok) memcpy(pin_out, item->valuestring, 7);
    cJSON_Delete(root);
    return ok;
}

static inline bool companion_normalize_sha256(const char *raw, char out[65]) {
    if (!raw || !out) return false;
    if (!strncmp(raw, "sha256:", 7) || !strncmp(raw, "SHA256:", 7)) raw += 7;
    for (size_t i = 0; i < 64; ++i) {
        unsigned char c = (unsigned char)raw[i];
        if (c >= '0' && c <= '9') out[i] = (char)c;
        else if (c >= 'a' && c <= 'f') out[i] = (char)c;
        else if (c >= 'A' && c <= 'F') out[i] = (char)(c - 'A' + 'a');
        else { out[0] = 0; return false; }
    }
    if (raw[64] != '\0') { out[0] = 0; return false; }
    out[64] = '\0';
    return true;
}

static inline bool companion_valid_utf8_text(const char *s, size_t n, bool allow_newlines) {
    if (!s) return false;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i++];
        if (c < 128) {
            if (c < 32) {
                if (!allow_newlines || (c != '\n' && c != '\r' && c != '\t')) return false;
            }
            if (c == 127) return false;
            continue;
        }
        uint32_t cp;
        unsigned more;
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; more = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; more = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; more = 3; }
        else return false;
        unsigned width = more;
        while (more--) {
            if (i == n || ((unsigned char)s[i] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | ((unsigned char)s[i++] & 63);
        }
        if ((width == 2 && cp < 0x800) || (width == 3 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return false;
    }
    return true;
}

static inline bool companion_parse_book_progress_restore(const char *body, size_t len,
                                                         read_pico_book_progress_restore_t *out,
                                                         const char **err_out) {
    if (err_out) *err_out = "无效的进度 JSON";
    if (!body || !len || !out) return false;
    memset(out, 0, sizeof(*out));
    cJSON *root = companion_parse_strict_json(body, len);
    if (!root || !cJSON_IsObject(root) || !root->child) {
        cJSON_Delete(root);
        return false;
    }

    read_pico_book_progress_restore_t parsed = {0};
    bool has_percent = false, has_chapter = false, has_byte_off = false, has_set_last = false;
    bool ok = true;
    const char *err = "无效的阅读进度参数";

    for (const cJSON *item = root->child; item && ok; item = item->next) {
        if (!item->string) { ok = false; break; }
        int iv = 0;
        uint32_t uv = 0;
        if (!strcmp(item->string, "percent")) {
            ok = !has_percent && companion_json_int_range(item, 0, 100, &iv);
            if (ok) { parsed.percent = (uint8_t)iv; has_percent = true; }
            else err = "percent 须为 0..100 的整数";
        } else if (!strcmp(item->string, "chapter")) {
            ok = !has_chapter && companion_json_int_range(item, 0, 65535, &iv);
            if (ok) { parsed.chapter = (uint16_t)iv; has_chapter = true; }
            else err = "chapter 须为 0..65535 的整数";
        } else if (!strcmp(item->string, "byte_off")) {
            ok = !has_byte_off && companion_json_u32_range(item, 0, UINT32_MAX, &uv);
            if (ok) { parsed.byte_off = uv; has_byte_off = true; }
            else err = "byte_off 须为无符号 32 位整数";
        } else if (!strcmp(item->string, "px")) {
            ok = !parsed.has_px && companion_json_int_range(item, 36, 72, &iv) && ((iv - 36) % 4 == 0);
            if (ok) { parsed.px = (uint8_t)iv; parsed.has_px = true; }
            else err = "px 须为 36..72 且步长为 4";
        } else if (!strcmp(item->string, "file_size")) {
            ok = !parsed.has_file_size && companion_json_u32_range(item, 0, UINT32_MAX, &uv);
            if (ok) { parsed.file_size = uv; parsed.has_file_size = true; }
            else err = "file_size 须为无符号 32 位整数";
        } else if (!strcmp(item->string, "set_last")) {
            ok = !has_set_last && cJSON_IsBool(item);
            if (ok) { parsed.set_last = cJSON_IsTrue(item); has_set_last = true; }
            else err = "set_last 须为布尔值";
        } else {
            ok = false;
            err = "包含未知的进度字段";
        }
    }
    cJSON_Delete(root);
    if (ok && (!has_percent || !has_chapter || !has_byte_off)) {
        ok = false;
        err = "缺少必填字段 percent / chapter / byte_off";
    }
    if (!ok) {
        if (err_out) *err_out = err;
        return false;
    }
    *out = parsed;
    return true;
}

static inline bool companion_parse_book_notes_restore(const char *body, size_t len,
                                                      bool *has_file_size_out, uint32_t *file_size_out,
                                                      cJSON **root_out,
                                                      read_pico_book_note_item_t **items_out,
                                                      size_t *count_out,
                                                      const char **err_out) {
    if (err_out) *err_out = "无效的笔记 JSON";
    if (has_file_size_out) *has_file_size_out = false;
    if (file_size_out) *file_size_out = 0;
    if (root_out) *root_out = NULL;
    if (items_out) *items_out = NULL;
    if (count_out) *count_out = 0;
    if (!body || !len || !root_out || !items_out || !count_out) return false;

    cJSON *root = companion_parse_strict_json(body, len);
    if (!root || !cJSON_IsObject(root) || !root->child) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *items_arr = NULL;
    bool has_file_size = false, has_book = false, has_count = false;
    uint32_t file_size = 0;
    int expected_count = 0;
    bool ok = true;
    const char *err = "无效的笔记恢复参数";

    for (const cJSON *field = root->child; field && ok; field = field->next) {
        if (!field->string) { ok = false; break; }
        if (!strcmp(field->string, "items")) {
            ok = !items_arr && cJSON_IsArray(field);
            if (ok) items_arr = field;
            else err = "items 须为数组";
        } else if (!strcmp(field->string, "file_size")) {
            ok = !has_file_size && companion_json_u32_range(field, 0, UINT32_MAX, &file_size);
            if (ok) has_file_size = true;
            else err = "file_size 须为无符号 32 位整数";
        } else if (!strcmp(field->string, "book")) {
            ok = !has_book && cJSON_IsString(field) && field->valuestring != NULL;
            if (ok) has_book = true;
            else err = "book 须为字符串";
        } else if (!strcmp(field->string, "count")) {
            ok = !has_count && companion_json_int_range(field, 0, (int)TRANSFER_NOTES_ITEMS_MAX, &expected_count);
            if (ok) has_count = true;
            else err = "count 须为有效非负整数";
        } else {
            ok = false;
            err = "包含未知的笔记顶层字段";
        }
    }

    if (ok && !items_arr) {
        ok = false;
        err = "缺少必填数组 items";
    }
    size_t count = ok ? (size_t)cJSON_GetArraySize(items_arr) : 0;
    if (ok && count > TRANSFER_NOTES_ITEMS_MAX) {
        ok = false;
        err = "单书笔记条数超出上限";
    }
    if (ok && has_count && (size_t)expected_count != count) {
        ok = false;
        err = "count 与 items 实际条数不一致";
    }

    read_pico_book_note_item_t *parsed = NULL;
    if (ok && count > 0) {
        parsed = (read_pico_book_note_item_t *)calloc(count, sizeof(*parsed));
        if (!parsed) {
            cJSON_Delete(root);
            if (err_out) *err_out = "内存不足";
            return false;
        }
    }

    size_t idx = 0;
    for (const cJSON *entry = ok ? items_arr->child : NULL; entry && ok; entry = entry->next, ++idx) {
        if (!cJSON_IsObject(entry) || !entry->child) {
            ok = false;
            err = "items 元素须为非空对象";
            break;
        }
        bool has_kind = false, has_chapter = false, has_offset = false, has_percent = false;
        bool has_text = false, has_index = false, has_truncated = false;
        read_pico_book_note_item_t item = {0};

        for (const cJSON *f = entry->child; f && ok; f = f->next) {
            if (!f->string) { ok = false; break; }
            int iv = 0;
            uint32_t uv = 0;
            if (!strcmp(f->string, "kind")) {
                ok = !has_kind && cJSON_IsString(f) && f->valuestring &&
                     (!strcmp(f->valuestring, "bookmark") || !strcmp(f->valuestring, "excerpt"));
                if (ok) {
                    item.is_excerpt = !strcmp(f->valuestring, "excerpt");
                    has_kind = true;
                } else {
                    err = "kind 须为 bookmark 或 excerpt";
                }
            } else if (!strcmp(f->string, "chapter")) {
                ok = !has_chapter && companion_json_int_range(f, 0, 65535, &iv);
                if (ok) { item.chapter = (uint16_t)iv; has_chapter = true; }
                else err = "chapter 须为 0..65535 的整数";
            } else if (!strcmp(f->string, "offset")) {
                ok = !has_offset && companion_json_u32_range(f, 0, UINT32_MAX, &uv);
                if (ok) { item.byte_off = uv; has_offset = true; }
                else err = "offset 须为无符号 32 位整数";
            } else if (!strcmp(f->string, "percent")) {
                ok = !has_percent && companion_json_int_range(f, 0, 100, &iv);
                if (ok) { item.percent = (uint8_t)iv; has_percent = true; }
                else err = "percent 须为 0..100 的整数";
            } else if (!strcmp(f->string, "text")) {
                if (has_text) { ok = false; err = "重复的 text 字段"; break; }
                has_text = true;
                if (cJSON_IsNull(f)) {
                    item.text = NULL;
                    item.text_len = 0;
                } else if (cJSON_IsString(f) && f->valuestring) {
                    size_t tlen = strlen(f->valuestring);
                    if (tlen == 0 || tlen > TRANSFER_NOTE_TEXT_MAX ||
                        !companion_valid_utf8_text(f->valuestring, tlen, true)) {
                        ok = false;
                        err = "摘录 text 须为 1..4096 字节的合法 UTF-8";
                    } else {
                        item.text = f->valuestring;
                        item.text_len = tlen;
                    }
                } else {
                    ok = false;
                    err = "text 须为字符串或 null";
                }
            } else if (!strcmp(f->string, "index")) {
                ok = !has_index && companion_json_u32_range(f, 0, UINT32_MAX, &uv);
                if (ok) has_index = true;
                else err = "index 须为非负整数";
            } else if (!strcmp(f->string, "truncated")) {
                ok = !has_truncated && cJSON_IsBool(f);
                if (ok) has_truncated = true;
                else err = "truncated 须为布尔值";
            } else {
                ok = false;
                err = "笔记条目包含未知字段";
            }
        }
        if (ok && (!has_kind || !has_chapter || !has_offset || !has_percent)) {
            ok = false;
            err = "笔记条目缺少必填字段 kind / chapter / offset / percent";
        }
        if (ok) {
            if (item.is_excerpt) {
                if (!item.text || !item.text_len) {
                    ok = false;
                    err = "摘录条目缺少非空 text";
                }
            } else if (item.text != NULL) {
                ok = false;
                err = "书签条目不得包含 text";
            }
        }
        if (ok) parsed[idx] = item;
    }

    if (!ok) {
        free(parsed);
        cJSON_Delete(root);
        if (err_out) *err_out = err;
        return false;
    }
    if (has_file_size_out) *has_file_size_out = has_file_size;
    if (file_size_out) *file_size_out = file_size;
    *root_out = root;
    *items_out = parsed;
    *count_out = count;
    return true;
}

static inline bool companion_parse_stats_restore(const char *body, size_t len,
                                                 read_pico_stats_restore_t *out,
                                                 const char **err_out) {
    if (err_out) *err_out = "无效的阅读统计 JSON";
    if (!body || !len || !out) return false;
    memset(out, 0, sizeof(*out));
    cJSON *root = companion_parse_strict_json(body, len);
    if (!root || !cJSON_IsObject(root) || !root->child) {
        cJSON_Delete(root);
        return false;
    }

    read_pico_stats_restore_t parsed = {0};
    bool has_mode = false, has_last_path = false;
    bool ok = true;
    const char *err = "无效的阅读统计参数";

    for (const cJSON *item = root->child; item && ok; item = item->next) {
        if (!item->string) { ok = false; break; }
        if (!strcmp(item->string, "mode")) {
            if (has_mode || !cJSON_IsString(item) || !item->valuestring) {
                ok = false; err = "mode 须为 replace 或 merge";
            } else if (!strcmp(item->valuestring, "replace")) {
                parsed.merge = false; has_mode = true;
            } else if (!strcmp(item->valuestring, "merge")) {
                parsed.merge = true; has_mode = true;
            } else {
                ok = false; err = "mode 须为 replace 或 merge";
            }
        } else if (!strcmp(item->string, "total_reading_sec")) {
            uint32_t sec = 0;
            ok = !parsed.has_total_reading_sec && companion_json_u32_range(item, 0, UINT32_MAX, &sec);
            if (ok) { parsed.total_reading_sec = sec; parsed.has_total_reading_sec = true; }
            else err = "total_reading_sec 须为无符号 32 位整数";
        } else if (!strcmp(item->string, "last_book")) {
            if (parsed.has_last_book) { ok = false; err = "重复的 last_book 字段"; break; }
            parsed.has_last_book = true;
            if (cJSON_IsNull(item)) {
                parsed.clear_last_book = true;
            } else if (cJSON_IsObject(item) && item->child) {
                bool got_title = false, got_pct = false;
                for (const cJSON *f = item->child; f && ok; f = f->next) {
                    if (!f->string) { ok = false; break; }
                    if (!strcmp(f->string, "title")) {
                        if (got_title || !cJSON_IsString(f) || !f->valuestring) {
                            ok = false; err = "last_book.title 须为非空字符串"; break;
                        }
                        size_t tlen = strlen(f->valuestring);
                        if (!tlen || tlen > TRANSFER_STATS_TITLE_MAX ||
                            !companion_valid_utf8_text(f->valuestring, tlen, false)) {
                            ok = false; err = "last_book.title 须为 1..127 字节的单行合法 UTF-8"; break;
                        }
                        memcpy(parsed.last_book_title, f->valuestring, tlen + 1);
                        got_title = true;
                    } else if (!strcmp(f->string, "percent")) {
                        int iv = 0;
                        ok = !got_pct && companion_json_int_range(f, 0, 100, &iv);
                        if (ok) { parsed.last_book_percent = (uint8_t)iv; got_pct = true; }
                        else err = "last_book.percent 须为 0..100 的整数";
                    } else {
                        ok = false; err = "last_book 包含未知字段";
                    }
                }
                if (ok && (!got_title || !got_pct)) {
                    ok = false; err = "last_book 缺少必填字段 title / percent";
                }
            } else {
                ok = false; err = "last_book 须为对象或 null";
            }
        } else if (!strcmp(item->string, "excerpt")) {
            if (parsed.has_excerpt) { ok = false; err = "重复的 excerpt 字段"; break; }
            parsed.has_excerpt = true;
            if (cJSON_IsNull(item)) {
                parsed.clear_excerpt = true;
            } else if (cJSON_IsObject(item) && item->child) {
                bool got_title = false, got_text = false;
                for (const cJSON *f = item->child; f && ok; f = f->next) {
                    if (!f->string) { ok = false; break; }
                    if (!strcmp(f->string, "title")) {
                        if (got_title || !cJSON_IsString(f) || !f->valuestring) {
                            ok = false; err = "excerpt.title 须为非空字符串"; break;
                        }
                        size_t tlen = strlen(f->valuestring);
                        if (!tlen || tlen > TRANSFER_STATS_TITLE_MAX ||
                            !companion_valid_utf8_text(f->valuestring, tlen, false)) {
                            ok = false; err = "excerpt.title 须为 1..127 字节的单行合法 UTF-8"; break;
                        }
                        memcpy(parsed.excerpt_title, f->valuestring, tlen + 1);
                        got_title = true;
                    } else if (!strcmp(f->string, "text")) {
                        if (got_text || !cJSON_IsString(f) || !f->valuestring) {
                            ok = false; err = "excerpt.text 须为非空字符串"; break;
                        }
                        size_t xlen = strlen(f->valuestring);
                        if (!xlen || xlen > TRANSFER_STATS_EXCERPT_MAX ||
                            !companion_valid_utf8_text(f->valuestring, xlen, true)) {
                            ok = false; err = "excerpt.text 须为 1..300 字节的合法 UTF-8"; break;
                        }
                        memcpy(parsed.excerpt_text, f->valuestring, xlen + 1);
                        got_text = true;
                    } else {
                        ok = false; err = "excerpt 包含未知字段";
                    }
                }
                if (ok && (!got_title || !got_text)) {
                    ok = false; err = "excerpt 缺少必填字段 title / text";
                }
            } else {
                ok = false; err = "excerpt 须为对象或 null";
            }
        } else if (!strcmp(item->string, "last_book_path")) {
            // 兼容 GET /stats 原样回传；自动续读路径须通过 PUT /books/progress 的 set_last 更新。
            // Accept read-only last_book_path from GET /stats; resume path is updated via PUT /books/progress set_last.
            ok = !has_last_path && (cJSON_IsString(item) || cJSON_IsNull(item));
            if (ok) has_last_path = true;
            else err = "last_book_path 须为字符串或 null";
        } else {
            ok = false;
            err = "包含未知的阅读统计字段";
        }
    }
    cJSON_Delete(root);
    if (ok && !parsed.has_total_reading_sec && !parsed.has_last_book && !parsed.has_excerpt) {
        ok = false;
        err = "至少须提供 total_reading_sec、last_book 或 excerpt 之一";
    }
    if (!ok) {
        if (err_out) *err_out = err;
        return false;
    }
    *out = parsed;
    return true;
}
