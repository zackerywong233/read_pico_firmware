/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：以独立 NVS 命名空间记录累计阅读时长。
 * English: Store cumulative reading time in an independent NVS namespace.
 * 冻结：统计不擦除其他阅读进度；摘录与最近阅读各自使用明确的 NVS 键。
 * Frozen: Never erase other reading progress; excerpts and recent-book data use dedicated NVS keys.
 */
#include "reading_stats.h"

#include <string.h>

#include "nvs.h"
#include "esp_err.h"

#define STATS_NS "rp_stats"
#define STATS_KEY "read_sec"
#define KEY_QUOTE_TITLE "quote_title"
#define KEY_QUOTE_TEXT "quote_text"
#define KEY_LAST_TITLE "last_title"
#define KEY_LAST_PERCENT "last_pct"

uint32_t reading_stats_total_seconds(void) {
    nvs_handle_t h;
    uint32_t seconds = 0;
    if (nvs_open(STATS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    (void)nvs_get_u32(h, STATS_KEY, &seconds);
    nvs_close(h);
    return seconds;
}

void reading_stats_add_seconds(uint32_t seconds) {
    if (seconds == 0) return;
    nvs_handle_t h;
    if (nvs_open(STATS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t total = 0;
    (void)nvs_get_u32(h, STATS_KEY, &total);
    if (UINT32_MAX - total < seconds) total = UINT32_MAX;
    else total += seconds;
    if (nvs_set_u32(h, STATS_KEY, total) == ESP_OK) (void)nvs_commit(h);
    nvs_close(h);
}

static bool get_string(const char* key, char* out, uint32_t cap) {
    if (out == NULL || cap == 0) return false;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(STATS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = cap;
    esp_err_t err = nvs_get_str(h, key, out, &n);
    nvs_close(h);
    if (err != ESP_OK) out[0] = '\0';
    return err == ESP_OK;
}

static void put_string(const char* key, const char* value) {
    nvs_handle_t h;
    if (nvs_open(STATS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, key, value ? value : "") == ESP_OK) (void)nvs_commit(h);
    nvs_close(h);
}

void reading_stats_save_excerpt(const char* title, const char* text) {
    put_string(KEY_QUOTE_TITLE, title);
    put_string(KEY_QUOTE_TEXT, text);
}

bool reading_stats_get_excerpt(char* title, uint32_t title_cap, char* text, uint32_t text_cap) {
    bool have_title = get_string(KEY_QUOTE_TITLE, title, title_cap);
    bool have_text = get_string(KEY_QUOTE_TEXT, text, text_cap);
    return have_title && have_text && text[0] != '\0';
}

void reading_stats_set_last_book(const char* title, uint8_t percent) {
    if (percent > 100) percent = 100;
    char prev[128] = {0};
    uint8_t old = 255;
    nvs_handle_t h;
    if (nvs_open(STATS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t n = sizeof(prev);
        (void)nvs_get_str(h, KEY_LAST_TITLE, prev, &n);
        (void)nvs_get_u8(h, KEY_LAST_PERCENT, &old);
        nvs_close(h);
    }
    if (title && strcmp(prev, title) == 0 && old == percent) return;
    if (nvs_open(STATS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_str(h, KEY_LAST_TITLE, title ? title : "");
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_LAST_PERCENT, percent);
    if (err == ESP_OK) (void)nvs_commit(h);
    nvs_close(h);
}

bool reading_stats_get_last_book(char* title, uint32_t title_cap, uint8_t* percent) {
    bool have_title = get_string(KEY_LAST_TITLE, title, title_cap);
    nvs_handle_t h;
    if (!have_title || nvs_open(STATS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t value = 0;
    esp_err_t err = nvs_get_u8(h, KEY_LAST_PERCENT, &value);
    nvs_close(h);
    if (err != ESP_OK || title[0] == '\0') return false;
    if (percent) *percent = value;
    return true;
}
