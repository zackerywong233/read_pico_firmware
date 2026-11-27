/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 网页收藏夹容量与增删回归。/ Bookmark capacity and add/remove regression.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/apps/web_bookmark_policy.h"

int main(void) {
    web_bookmark_t items[WEB_BOOKMARK_MAX] = {0};
    size_t count = 0;
    char title[32], url[64];
    for (int i = 0; i < WEB_BOOKMARK_MAX; ++i) {
        snprintf(title, sizeof(title), "Page %d", i);
        snprintf(url, sizeof(url), "https://example.com/%d", i);
        assert(web_bookmark_add(items, &count, title, url));
    }
    assert(count == WEB_BOOKMARK_MAX);
    assert(!web_bookmark_add(items, &count, "overflow", "https://example.com/overflow"));
    assert(!web_bookmark_add(items, &count, "duplicate", "https://example.com/0"));
    assert(web_bookmark_remove(items, &count, "https://example.com/7"));
    assert(count == WEB_BOOKMARK_MAX - 1);
    assert(web_bookmark_find(items, count, "https://example.com/7") == WEB_BOOKMARK_MAX);
    assert(web_bookmark_add(items, &count, "replacement", "https://example.com/new"));
    assert(count == WEB_BOOKMARK_MAX);
    puts("web bookmarks host tests passed");
}
