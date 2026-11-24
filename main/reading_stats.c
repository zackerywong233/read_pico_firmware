/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：以独立 NVS 命名空间记录累计阅读时长、按日阅读热力图、摘录与最近阅读。
 * English: Store cumulative reading time, daily reading heatmap, excerpts, and recent book in an independent NVS namespace.
 * 冻结：统计不擦除其他阅读进度；摘录、最近阅读与热力图各自使用明确的 NVS 键。
 * Frozen: Never erase other reading progress; excerpts, recent-book, and heatmap data use dedicated NVS keys.
 */
#include "reading_stats.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvs.h"
#include "esp_err.h"

#define STATS_NS "rp_stats"
#define STATS_KEY "read_sec"
#define KEY_QUOTE_TITLE "quote_title"
#define KEY_QUOTE_TEXT "quote_text"
#define KEY_LAST_TITLE "last_title"
#define KEY_LAST_PERCENT "last_pct"
#define KEY_HEATMAP "heatmap"
#define EXCERPT_MAX_BYTES 300
#define HEATMAP_BLOB_VERSION 1U
#define STATS_MIN_VALID_UTC 946684800
#define STATS_MAX_VALID_UTC 4102444799

typedef struct {
    uint16_t version;
    uint16_t count;
    uint32_t last_day;
    uint16_t seconds[READING_HEATMAP_DAYS];
} heatmap_blob_t;

static size_t utf8_prefix(const char* text, size_t len, size_t limit) {
    if (limit >= len) return len;
    size_t end = limit;
    while (end > 0 && ((uint8_t)text[end] & 0xc0) == 0x80) --end;
    if (end == limit && end > 0) {
        uint8_t lead = (uint8_t)text[end - 1];
        size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc2 ? 2 : 1;
        if (need > limit - (end - 1)) --end;
    }
    return end;
}

static bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int days_in_month(int year, int month) {
    static const int k_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap_year(year)) return 29;
    return k_days[month - 1];
}

uint32_t reading_stats_day_from_ymd(int year, int month, int day) {
    if (year < 2000 || year > 2099 || month < 1 || month > 12) return 0;
    if (day < 1 || day > days_in_month(year, month)) return 0;
    int64_t y = (int64_t)year - (month <= 2 ? 1 : 0);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t m = month;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return days > 0 ? (uint32_t)days : 0;
}

void reading_stats_ymd_from_day(uint32_t day_index, int* year, int* month, int* day) {
    int64_t z = (int64_t)day_index + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    if (year) *year = (int)y;
    if (month) *month = (int)m;
    if (day) *day = (int)d;
}

uint8_t reading_stats_heatmap_level(uint32_t seconds) {
    if (seconds == 0) return 0;
    if (seconds < 900U) return 1;  // < 15 分钟 / < 15 min
    if (seconds < 1800U) return 2; // 15..29 分钟 / 15..29 min
    if (seconds < 3600U) return 3; // 30..59 分钟 / 30..59 min
    return 4;                      // >= 60 分钟 / >= 60 min
}

static uint16_t sat_add_u16(uint16_t base, uint32_t add) {
    uint32_t sum = (uint32_t)base + add;
    return sum > UINT16_MAX ? UINT16_MAX : (uint16_t)sum;
}

static uint32_t current_local_day(void) {
    time_t now = time(NULL);
    if (now < (time_t)STATS_MIN_VALID_UTC || now > (time_t)STATS_MAX_VALID_UTC) return 0;
    struct tm local;
    if (!localtime_r(&now, &local)) return 0;
    return reading_stats_day_from_ymd(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
}

static bool load_heatmap_blob(nvs_handle_t h, heatmap_blob_t* out) {
    memset(out, 0, sizeof(*out));
    out->version = HEATMAP_BLOB_VERSION;
    out->count = READING_HEATMAP_DAYS;
    size_t len = sizeof(*out);
    if (nvs_get_blob(h, KEY_HEATMAP, out, &len) != ESP_OK) return false;
    if (len != sizeof(*out) || out->version != HEATMAP_BLOB_VERSION ||
        out->count != READING_HEATMAP_DAYS) {
        memset(out, 0, sizeof(*out));
        out->version = HEATMAP_BLOB_VERSION;
        out->count = READING_HEATMAP_DAYS;
        return false;
    }
    return true;
}

static void record_heatmap_day(nvs_handle_t h, uint32_t local_day, uint32_t seconds) {
    if (local_day == 0 || seconds == 0) return;
    heatmap_blob_t blob;
    (void)load_heatmap_blob(h, &blob);
    if (blob.last_day == 0) {
        blob.last_day = local_day;
        blob.seconds[READING_HEATMAP_DAYS - 1] = sat_add_u16(0, seconds);
    } else if (local_day > blob.last_day) {
        uint32_t shift = local_day - blob.last_day;
        if (shift >= READING_HEATMAP_DAYS) {
            memset(blob.seconds, 0, sizeof(blob.seconds));
        } else {
            memmove(blob.seconds, blob.seconds + shift,
                    (READING_HEATMAP_DAYS - shift) * sizeof(blob.seconds[0]));
            memset(blob.seconds + (READING_HEATMAP_DAYS - shift), 0,
                   shift * sizeof(blob.seconds[0]));
        }
        blob.last_day = local_day;
        blob.seconds[READING_HEATMAP_DAYS - 1] = sat_add_u16(0, seconds);
    } else {
        uint32_t age = blob.last_day - local_day;
        if (age >= READING_HEATMAP_DAYS) return;
        size_t idx = (size_t)(READING_HEATMAP_DAYS - 1U - age);
        blob.seconds[idx] = sat_add_u16(blob.seconds[idx], seconds);
    }
    (void)nvs_set_blob(h, KEY_HEATMAP, &blob, sizeof(blob));
}

uint32_t reading_stats_total_seconds(void) {
    nvs_handle_t h;
    uint32_t seconds = 0;
    if (nvs_open(STATS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    (void)nvs_get_u32(h, STATS_KEY, &seconds);
    nvs_close(h);
    return seconds;
}

void reading_stats_add_seconds_on_day(uint32_t local_day, uint32_t seconds) {
    if (seconds == 0) return;
    nvs_handle_t h;
    if (nvs_open(STATS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint32_t total = 0;
    (void)nvs_get_u32(h, STATS_KEY, &total);
    if (UINT32_MAX - total < seconds) total = UINT32_MAX;
    else total += seconds;
    esp_err_t err = nvs_set_u32(h, STATS_KEY, total);
    if (local_day != 0) record_heatmap_day(h, local_day, seconds);
    if (err == ESP_OK) (void)nvs_commit(h);
    nvs_close(h);
}

void reading_stats_add_seconds(uint32_t seconds) {
    reading_stats_add_seconds_on_day(current_local_day(), seconds);
}

bool reading_stats_get_heatmap(uint32_t anchor_day, reading_heatmap_t* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    heatmap_blob_t blob;
    bool have_blob = false;
    nvs_handle_t h;
    if (nvs_open(STATS_NS, NVS_READONLY, &h) == ESP_OK) {
        have_blob = load_heatmap_blob(h, &blob);
        nvs_close(h);
    }

    uint32_t effective_anchor = anchor_day != 0 ? anchor_day : (have_blob ? blob.last_day : 0);
    out->has_date = anchor_day != 0;
    out->anchor_day = effective_anchor;

    if (have_blob && blob.last_day != 0 && effective_anchor != 0) {
        for (uint32_t i = 0; i < READING_HEATMAP_DAYS; ++i) {
            uint32_t back = (READING_HEATMAP_DAYS - 1U) - i;
            if (effective_anchor <= back) continue;
            uint32_t day = effective_anchor - back;
            if (day <= blob.last_day) {
                uint32_t age = blob.last_day - day;
                if (age < READING_HEATMAP_DAYS) {
                    out->daily_sec[i] = blob.seconds[READING_HEATMAP_DAYS - 1U - age];
                }
            }
        }
    }

    uint16_t run = 0;
    for (uint32_t i = 0; i < READING_HEATMAP_DAYS; ++i) {
        uint16_t sec = out->daily_sec[i];
        out->window_total_sec += sec;
        if (sec > out->max_daily_sec) out->max_daily_sec = sec;
        if (sec > 0) {
            out->active_days++;
            run++;
            if (run > out->longest_streak) out->longest_streak = run;
        } else {
            run = 0;
        }
    }
    out->today_sec = out->daily_sec[READING_HEATMAP_DAYS - 1U];

    // 今日已读从今日倒推；今日未读但昨日有读，仍保留昨日连续天数。
    // Count back from today when active; otherwise preserve streak ending yesterday.
    int start = (int)READING_HEATMAP_DAYS - 1;
    if (out->daily_sec[start] == 0 && start > 0 && out->daily_sec[start - 1] > 0) {
        start--;
    }
    uint16_t cur = 0;
    while (start >= 0 && out->daily_sec[start] > 0) {
        cur++;
        start--;
    }
    out->current_streak = cur;
    return out->active_days > 0;
}

esp_err_t reading_stats_set_total_seconds(uint32_t seconds) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(STATS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(h, STATS_KEY, seconds);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool get_string(const char* key, char* out, uint32_t cap) {
    if (out == NULL || cap == 0) return false;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(STATS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t need = 0;
    esp_err_t err = nvs_get_str(h, key, NULL, &need);
    if (err != ESP_OK || need == 0) {
        nvs_close(h);
        return false;
    }
    char* value = malloc(need);
    if (!value) {
        nvs_close(h);
        return false;
    }
    err = nvs_get_str(h, key, value, &need);
    nvs_close(h);
    if (err == ESP_OK) {
        size_t len = strlen(value);
        size_t copied = utf8_prefix(value, len, cap - 1);
        memcpy(out, value, copied);
        out[copied] = '\0';
    }
    free(value);
    if (err != ESP_OK) out[0] = '\0';
    return err == ESP_OK;
}

static esp_err_t put_string_checked(const char* key, const char* value) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(STATS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, value ? value : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t reading_stats_set_excerpt_checked(const char* title, const char* text) {
    if (!text || !text[0]) {
        // 原子清空摘录标题与正文。/ Clear both excerpt title and text atomically.
        nvs_handle_t h;
        esp_err_t err = nvs_open(STATS_NS, NVS_READWRITE, &h);
        if (err != ESP_OK) return err;
        err = nvs_set_str(h, KEY_QUOTE_TITLE, "");
        if (err == ESP_OK) err = nvs_set_str(h, KEY_QUOTE_TEXT, "");
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        return err;
    }
    const char* safe_title = title ? title : "";
    char excerpt[EXCERPT_MAX_BYTES + 1];
    const char* stored_text = text;
    size_t len = strlen(text);
    if (len > EXCERPT_MAX_BYTES) {
        size_t keep = utf8_prefix(text, len, EXCERPT_MAX_BYTES - 3);
        memcpy(excerpt, text, keep);
        memcpy(excerpt + keep, "…", 3);
        excerpt[keep + 3] = '\0';
        stored_text = excerpt;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(STATS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, KEY_QUOTE_TITLE, safe_title);
    if (err == ESP_OK) err = nvs_set_str(h, KEY_QUOTE_TEXT, stored_text);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

void reading_stats_save_excerpt(const char* title, const char* text) {
    (void)put_string_checked(KEY_QUOTE_TITLE, title);
    if (!text) text = "";
    size_t len = strlen(text);
    if (len <= EXCERPT_MAX_BYTES) {
        (void)put_string_checked(KEY_QUOTE_TEXT, text);
        return;
    }
    size_t keep = utf8_prefix(text, len, EXCERPT_MAX_BYTES - 3);
    char excerpt[EXCERPT_MAX_BYTES + 1];
    memcpy(excerpt, text, keep);
    memcpy(excerpt + keep, "…", 3);
    excerpt[keep + 3] = '\0';
    (void)put_string_checked(KEY_QUOTE_TEXT, excerpt);
}

bool reading_stats_get_excerpt(char* title, uint32_t title_cap, char* text, uint32_t text_cap) {
    bool have_title = get_string(KEY_QUOTE_TITLE, title, title_cap);
    bool have_text = get_string(KEY_QUOTE_TEXT, text, text_cap);
    return have_title && have_text && text[0] != '\0';
}

esp_err_t reading_stats_set_last_book_checked(const char* title, uint8_t percent) {
    if (!title || !title[0]) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(STATS_NS, NVS_READWRITE, &h);
        if (err != ESP_OK) return err;
        err = nvs_set_str(h, KEY_LAST_TITLE, "");
        if (err == ESP_OK) err = nvs_set_u8(h, KEY_LAST_PERCENT, 0);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        return err;
    }
    if (percent > 100) percent = 100;
    nvs_handle_t h;
    esp_err_t err = nvs_open(STATS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, KEY_LAST_TITLE, title);
    if (err == ESP_OK) err = nvs_set_u8(h, KEY_LAST_PERCENT, percent);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
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
