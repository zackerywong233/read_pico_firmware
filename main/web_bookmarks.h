/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：网页收藏夹 NVS 持久化封装（命名空间 read_pico），供网页阅读页、HTTP 与 BLE 共用。
 *
 * English: NVS persistence wrapper for web bookmarks (namespace read_pico), shared by Web Reader, HTTP and BLE.
 *
 * 冻结：保持 read_pico / webmarks / webmark_n 既有 NVS 布局，最多 20 条。
 * Frozen: Preserve existing read_pico / webmarks / webmark_n NVS layout, up to 20 entries.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "web_bookmark_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 从 NVS 读取全部网页收藏（失败时返回空列表）。/ Load all web bookmarks from NVS (empty list on miss/error).
void web_bookmarks_load(web_bookmark_t out[WEB_BOOKMARK_MAX], size_t *count_out);
/// 将网页收藏数组完整写回 NVS。/ Persist the web bookmarks array to NVS.
esp_err_t web_bookmarks_save(const web_bookmark_t items[WEB_BOOKMARK_MAX], size_t count);
/// 新增或更新一条网页收藏（同 URL 更新标题，新 URL 满 20 条返回 ESP_ERR_NO_MEM）。
/// Add or update a web bookmark (updates title if URL exists; ESP_ERR_NO_MEM when full).
esp_err_t web_bookmarks_upsert(const char *title, const char *url);
/// 按 URL 删除一条网页收藏；未找到返回 ESP_ERR_NOT_FOUND。
/// Delete one web bookmark by URL; returns ESP_ERR_NOT_FOUND when absent.
esp_err_t web_bookmarks_delete(const char *url);

#ifdef __cplusplus
}
#endif
