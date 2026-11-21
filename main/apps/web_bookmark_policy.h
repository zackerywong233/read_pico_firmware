/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 网页收藏夹的固定容量操作，不包含 NVS 与页面逻辑。
 * Fixed-capacity bookmark operations without NVS or UI behavior.
 * 冻结：最多保存 20 条，每条只包含标题和 HTTP(S) 地址。
 * Frozen: Store at most 20 title and HTTP(S) URL pairs.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define WEB_BOOKMARK_MAX 20
#define WEB_BOOKMARK_TITLE_MAX 64
#define WEB_BOOKMARK_URL_MAX 256

typedef struct { char title[WEB_BOOKMARK_TITLE_MAX]; char url[WEB_BOOKMARK_URL_MAX]; } web_bookmark_t;

static size_t web_bookmark_find(const web_bookmark_t items[WEB_BOOKMARK_MAX], size_t count, const char *url) {
    for (size_t i = 0; i < count && i < WEB_BOOKMARK_MAX; ++i)
        if (!strcmp(items[i].url, url)) return i;
    return WEB_BOOKMARK_MAX;
}

static bool web_bookmark_add(web_bookmark_t items[WEB_BOOKMARK_MAX], size_t *count, const char *title, const char *url) {
    if (!items || !count || !title || !url || *count >= WEB_BOOKMARK_MAX ||
        strlen(title) >= WEB_BOOKMARK_TITLE_MAX || strlen(url) >= WEB_BOOKMARK_URL_MAX ||
        web_bookmark_find(items, *count, url) != WEB_BOOKMARK_MAX) return false;
    web_bookmark_t *item = &items[(*count)++];
    strcpy(item->title, title); strcpy(item->url, url);
    return true;
}

static bool web_bookmark_remove(web_bookmark_t items[WEB_BOOKMARK_MAX], size_t *count, const char *url) {
    if (!items || !count || !url) return false;
    size_t index = web_bookmark_find(items, *count, url);
    if (index == WEB_BOOKMARK_MAX) return false;
    for (size_t i = index; i + 1 < *count; ++i) items[i] = items[i + 1];
    memset(&items[--*count], 0, sizeof(items[0]));
    return true;
}
