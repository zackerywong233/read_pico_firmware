/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：验证摘录写入限长、旧 NVS 长摘录兼容读取与按日阅读热力图滚动统计。
 * English: Verify bounded excerpt writes, legacy long reads, and rolling daily reading heatmap stats.
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "nvs.h"
#include "reading_stats.h"

static char quote_title[256];
static char quote_text[4096];
static uint32_t read_seconds;
static uint8_t last_percent;
static char last_title[256];
static uint8_t heatmap_blob[1024];
static size_t heatmap_blob_len;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out) {
    assert(strcmp(name, "rp_stats") == 0);
    (void)mode;
    *out = 1;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out) {
    assert(handle == 1 && strcmp(key, "read_sec") == 0);
    *out = read_seconds;
    return ESP_OK;
}
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value) {
    assert(handle == 1 && strcmp(key, "read_sec") == 0);
    read_seconds = value;
    return ESP_OK;
}
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *out) {
    assert(handle == 1 && strcmp(key, "last_pct") == 0);
    *out = last_percent;
    return ESP_OK;
}
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value) {
    assert(handle == 1 && strcmp(key, "last_pct") == 0);
    last_percent = value;
    return ESP_OK;
}
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *size) {
    assert(handle == 1 && size);
    const char *value = strcmp(key, "quote_title") == 0 ? quote_title :
        strcmp(key, "quote_text") == 0 ? quote_text :
        strcmp(key, "last_title") == 0 ? last_title : NULL;
    if (!value) return ESP_ERR_NVS_NOT_FOUND;
    size_t need = strlen(value) + 1;
    if (!out) { *size = need; return ESP_OK; }
    if (*size < need) return ESP_ERR_INVALID_STATE;
    memcpy(out, value, need);
    *size = need;
    return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value) {
    assert(handle == 1 && value);
    char *out = strcmp(key, "quote_title") == 0 ? quote_title :
        strcmp(key, "quote_text") == 0 ? quote_text :
        strcmp(key, "last_title") == 0 ? last_title : NULL;
    assert(out);
    size_t cap = out == quote_text ? sizeof(quote_text) : sizeof(quote_title);
    assert(strlen(value) < cap);
    strcpy(out, value);
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) {
    assert(handle == 1 && strcmp(key, "heatmap") == 0 && size);
    if (heatmap_blob_len == 0) return ESP_ERR_NVS_NOT_FOUND;
    if (!out) { *size = heatmap_blob_len; return ESP_OK; }
    if (*size < heatmap_blob_len) return ESP_ERR_INVALID_STATE;
    memcpy(out, heatmap_blob, heatmap_blob_len);
    *size = heatmap_blob_len;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size) {
    assert(handle == 1 && strcmp(key, "heatmap") == 0 && value && size <= sizeof(heatmap_blob));
    memcpy(heatmap_blob, value, size);
    heatmap_blob_len = size;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { assert(handle == 1); return ESP_OK; }

static void test_heatmap(void) {
    // 日期换算与闰年往返。/ Date conversion and leap-year roundtrip.
    assert(reading_stats_day_from_ymd(1999, 12, 31) == 0);
    assert(reading_stats_day_from_ymd(2026, 2, 29) == 0);
    uint32_t leap = reading_stats_day_from_ymd(2024, 2, 29);
    assert(leap > 0);
    int y = 0, m = 0, d = 0;
    reading_stats_ymd_from_day(leap, &y, &m, &d);
    assert(y == 2024 && m == 2 && d == 29);

    uint32_t day = reading_stats_day_from_ymd(2026, 11, 25);
    assert(day > 0);
    reading_stats_ymd_from_day(day, &y, &m, &d);
    assert(y == 2026 && m == 11 && d == 25);
    assert((day + 3U) % 7U == 2U); // 2026-11-25 is Wednesday (Mon=0 -> 2).

    // 热力档位边界。/ Heatmap intensity level boundaries.
    assert(reading_stats_heatmap_level(0) == 0);
    assert(reading_stats_heatmap_level(1) == 1);
    assert(reading_stats_heatmap_level(899) == 1);
    assert(reading_stats_heatmap_level(900) == 2);
    assert(reading_stats_heatmap_level(1799) == 2);
    assert(reading_stats_heatmap_level(1800) == 3);
    assert(reading_stats_heatmap_level(3599) == 3);
    assert(reading_stats_heatmap_level(3600) == 4);
    assert(reading_stats_heatmap_level(7200) == 4);

    // 初始为空。/ Empty state.
    reading_heatmap_t hm;
    assert(!reading_stats_get_heatmap(day, &hm));
    assert(hm.has_date && hm.anchor_day == day && hm.active_days == 0 && hm.current_streak == 0);

    // 连续 3 天阅读 + 间隔 2 天 + 连续 2 天阅读。
    // 3-day streak, 2-day gap, then 2-day streak.
    reading_stats_add_seconds_on_day(day - 6, 300);
    reading_stats_add_seconds_on_day(day - 5, 1200);
    reading_stats_add_seconds_on_day(day - 4, 2400);
    reading_stats_add_seconds_on_day(day - 1, 3600);
    reading_stats_add_seconds_on_day(day, 600);
    reading_stats_add_seconds_on_day(day, 600); // 同日累加 / Same-day accumulation.

    assert(reading_stats_get_heatmap(day, &hm));
    assert(hm.has_date && hm.anchor_day == day);
    assert(hm.today_sec == 1200);
    assert(hm.active_days == 5);
    assert(hm.current_streak == 2);
    assert(hm.longest_streak == 3);
    assert(hm.max_daily_sec == 3600);
    assert(hm.window_total_sec == 300 + 1200 + 2400 + 3600 + 1200);

    // 次日尚未阅读时保留昨日连续天数。/ Next morning before reading still preserves streak ending yesterday.
    assert(reading_stats_get_heatmap(day + 1, &hm));
    assert(hm.today_sec == 0 && hm.current_streak == 2 && hm.longest_streak == 3);

    // 隔天未读后当前连续归零。/ After missing a full day, current streak resets to 0.
    assert(reading_stats_get_heatmap(day + 2, &hm));
    assert(hm.today_sec == 0 && hm.current_streak == 0 && hm.longest_streak == 3);

    // 补记历史日（窗口内）不破坏较新日期。/ Out-of-order past day inside window accumulates safely.
    reading_stats_add_seconds_on_day(day - 3, 900);
    assert(reading_stats_get_heatmap(day, &hm));
    assert(hm.daily_sec[READING_HEATMAP_DAYS - 1 - 3] == 900);
    assert(hm.longest_streak == 4 && hm.active_days == 6);

    // 超出窗口的极旧日期被忽略。/ Days older than the 182-day window are ignored safely.
    reading_stats_add_seconds_on_day(day - 200, 1800);
    assert(reading_stats_get_heatmap(day, &hm));
    assert(hm.active_days == 6);

    // 未校准时钟（anchor=0）回退至最后记录日。/ Uncalibrated anchor=0 falls back to last recorded day.
    assert(reading_stats_get_heatmap(0, &hm));
    assert(!hm.has_date && hm.anchor_day == day && hm.today_sec == 1200);

    // 跨度超过 182 天时安全滚动清空旧窗口并饱和处理 uint16。
    // Jump beyond 182 days rolls window cleanly and saturates uint16.
    reading_stats_add_seconds_on_day(day + 200, 70000);
    assert(reading_stats_get_heatmap(day + 200, &hm));
    assert(hm.active_days == 1 && hm.today_sec == UINT16_MAX && hm.current_streak == 1);
}

int main(void) {
    char long_text[1801];
    size_t at = 0;
    while (at + 3 < sizeof(long_text) - 1) {
        memcpy(long_text + at, "汉", 3);
        at += 3;
    }
    long_text[at] = '\0';
    reading_stats_save_excerpt("长摘录", long_text);
    assert(strlen(quote_text) == 300);
    assert(strcmp(quote_text + 297, "…") == 0);

    memset(quote_text, 0, sizeof(quote_text));
    for (int i = 0; i < 400; ++i) memcpy(quote_text + i * 3, "旧", 3);
    strcpy(quote_title, "旧版摘录");
    char title[32], text[50];
    assert(reading_stats_get_excerpt(title, sizeof(title), text, sizeof(text)));
    assert(strcmp(title, "旧版摘录") == 0);
    assert(strlen(text) == 48);
    assert((unsigned char)text[48] == 0);

    // 验证恢复与清空接口 / Verify restore and clear APIs.
    assert(reading_stats_set_total_seconds(12345U) == ESP_OK);
    assert(reading_stats_total_seconds() == 12345U);
    assert(reading_stats_set_last_book_checked("三体.epub", 88) == ESP_OK);
    uint8_t pct = 0;
    assert(reading_stats_get_last_book(title, sizeof(title), &pct) && strcmp(title, "三体.epub") == 0 && pct == 88);
    assert(reading_stats_set_last_book_checked("", 0) == ESP_OK);
    assert(!reading_stats_get_last_book(title, sizeof(title), &pct));
    assert(reading_stats_set_excerpt_checked("三体.epub", "给岁月以文明") == ESP_OK);
    assert(reading_stats_get_excerpt(title, sizeof(title), text, sizeof(text)) &&
           strcmp(title, "三体.epub") == 0 && strcmp(text, "给岁月以文明") == 0);
    assert(reading_stats_set_excerpt_checked("", "") == ESP_OK);
    assert(!reading_stats_get_excerpt(title, sizeof(title), text, sizeof(text)));

    test_heatmap();
    return 0;
}
