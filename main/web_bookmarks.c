/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：网页收藏夹 NVS 读写实现（命名空间 rp_web）。
 *
 * English: NVS load/store implementation for web bookmarks (namespace rp_web).
 */
#include "web_bookmarks.h"

#include <stdio.h>
#include <string.h>

#include "nvs.h"

#define WEB_NVS_NS "read_pico"

static bool valid_bookmark_url(const char *url) {
    if (!url || !url[0]) return false;
    size_t len = strlen(url);
    if (len >= WEB_BOOKMARK_URL_MAX) return false;
    bool http = !strncmp(url, "http://", 7);
    bool https = !strncmp(url, "https://", 8);
    if (!http && !https) return false;
    size_t host_start = http ? 7U : 8U;
    if (len <= host_start) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 32U || c >= 127U) return false;
    }
    return true;
}

static void clean_bookmark_title(const char *raw, const char *url, char out[WEB_BOOKMARK_TITLE_MAX]) {
    const char *src = (raw && raw[0]) ? raw : url;
    size_t n = 0;
    if (src) {
        for (size_t i = 0; src[i] && n + 1 < WEB_BOOKMARK_TITLE_MAX; ++i) {
            unsigned char c = (unsigned char)src[i];
            if (c < 32U || c == 127U) continue;
            out[n++] = (char)c;
        }
    }
    while (n > 0 && out[n - 1] == ' ') --n;
    // 确保截断不落在 UTF-8 多字节字符中间。/ Avoid splitting a multi-byte UTF-8 sequence.
    while (n > 0 && ((unsigned char)out[n - 1] & 0xC0U) == 0x80U) --n;
    if (n > 0 && ((unsigned char)out[n - 1] & 0xC0U) == 0xC0U) --n;
    out[n] = '\0';
    if (!out[0] && url) {
        snprintf(out, WEB_BOOKMARK_TITLE_MAX, "%.63s", url);
    }
}

void web_bookmarks_load(web_bookmark_t out[WEB_BOOKMARK_MAX], size_t *count_out) {
    if (out) memset(out, 0, sizeof(web_bookmark_t) * WEB_BOOKMARK_MAX);
    if (count_out) *count_out = 0;
    if (!out || !count_out) return;
    nvs_handle_t h;
    if (nvs_open(WEB_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t size = sizeof(web_bookmark_t) * WEB_BOOKMARK_MAX;
    uint8_t count = 0;
    esp_err_t err = nvs_get_blob(h, "webmarks", out, &size);
    if (err == ESP_OK && size == sizeof(web_bookmark_t) * WEB_BOOKMARK_MAX &&
        nvs_get_u8(h, "webmark_n", &count) == ESP_OK && count <= WEB_BOOKMARK_MAX) {
        *count_out = count;
    } else {
        memset(out, 0, sizeof(web_bookmark_t) * WEB_BOOKMARK_MAX);
        *count_out = 0;
    }
    nvs_close(h);
}

esp_err_t web_bookmarks_save(const web_bookmark_t items[WEB_BOOKMARK_MAX], size_t count) {
    if (!items || count > WEB_BOOKMARK_MAX) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(WEB_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, "webmarks", items, sizeof(web_bookmark_t) * WEB_BOOKMARK_MAX);
    if (err == ESP_OK) err = nvs_set_u8(h, "webmark_n", (uint8_t)count);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t web_bookmarks_upsert(const char *title, const char *url) {
    if (!valid_bookmark_url(url)) return ESP_ERR_INVALID_ARG;
    web_bookmark_t items[WEB_BOOKMARK_MAX];
    size_t count = 0;
    web_bookmarks_load(items, &count);

    char clean_title[WEB_BOOKMARK_TITLE_MAX] = {0};
    clean_bookmark_title(title, url, clean_title);

    size_t idx = web_bookmark_find(items, count, url);
    if (idx < count) {
        snprintf(items[idx].title, sizeof(items[idx].title), "%s", clean_title);
    } else {
        if (!web_bookmark_add(items, &count, clean_title, url)) {
            return count >= WEB_BOOKMARK_MAX ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
        }
    }
    return web_bookmarks_save(items, count);
}

esp_err_t web_bookmarks_delete(const char *url) {
    if (!url || !url[0] || strlen(url) >= WEB_BOOKMARK_URL_MAX) return ESP_ERR_INVALID_ARG;
    web_bookmark_t items[WEB_BOOKMARK_MAX];
    size_t count = 0;
    web_bookmarks_load(items, &count);
    if (!web_bookmark_remove(items, &count, url)) return ESP_ERR_NOT_FOUND;
    return web_bookmarks_save(items, count);
}
