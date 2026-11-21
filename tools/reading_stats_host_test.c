/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：验证摘录写入限长和旧 NVS 长摘录兼容读取。
 * English: Verify bounded excerpt writes and compatible reads of legacy long values.
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
esp_err_t nvs_commit(nvs_handle_t handle) { assert(handle == 1); return ESP_OK; }

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
    return 0;
}
