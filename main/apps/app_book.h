/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：图书应用公开入口与跨模块运行时设置同步钩子。
 * English: Public Books app entry and cross-module runtime settings sync hooks.
 */
#pragma once

#include <stdbool.h>

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const app_desc_t app_book;

/// 排队在进入图书页后立即打开的书籍完整路径。
/// Queue a book file path to open immediately upon entering Books.
void app_book_queue_open(const char *path);

/// 在图书页保持活动时同步快捷控制中心或远程修改的阅读/排版/方向设置。
/// Synchronize reader typography, HUD, gesture, and orientation changes while Books is active.
void app_book_sync_runtime_settings(app_ctx_t *ctx, bool apply_px_to_open_book);

#ifdef __cplusplus
}
#endif
