/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：在图书存储目录中按完整书籍路径和文件大小保存书签与摘录。
 * English: Store bookmarks and excerpts beside book storage, keyed by full path and size.
 *
 * 冻结：使用隐藏的图书数据目录；仅完整路径匹配时更新，读取还须匹配文件大小；旧 NVS 记录在成功迁移后清除。
 * Frozen: Use a hidden book-data directory; updates require an exact path, loads also require matching file size, and legacy NVS records are erased only after migration succeeds.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "book_store.h"
#include "esp_err.h"

typedef struct {
    uint16_t chapter;
    uint32_t byte_off;
    uint8_t percent;
} book_note_position_t;

typedef struct {
    book_note_position_t position;
    char *text;
    size_t text_len;
} book_note_excerpt_t;

typedef struct {
    size_t bookmark_count;
    size_t excerpt_count;
    book_note_position_t *bookmarks;
    book_note_excerpt_t *excerpts;
} book_notes_t;

/// 读取匹配笔记文件并动态分配列表；调用者须调用 book_notes_free。无记录返回 ESP_ERR_NOT_FOUND。
/// Load a matching sidecar and allocate its lists; caller must call book_notes_free. Return ESP_ERR_NOT_FOUND when absent.
esp_err_t book_notes_load(const char *path, uint32_t file_size, book_notes_t *out);
/// 原子保存当前笔记；拒绝空摘录及路径哈希碰撞，不限制条目数量。
/// Save notes atomically; reject empty excerpts and path-hash collisions without a per-book item limit.
esp_err_t book_notes_save(const char *path, uint32_t file_size, const book_notes_t *notes);
/// 释放读取列表。/ Free lists returned by book_notes_load.
void book_notes_free(book_notes_t *notes);
/// 删除指定路径的笔记；路径碰撞不会删除其他书籍记录。
/// Forget notes for one path; collisions never delete another book's record.
esp_err_t book_notes_forget(const char *path);
