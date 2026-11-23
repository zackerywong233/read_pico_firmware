/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：图书封面缩略图（144x196 4bpp）与锁屏满屏封面缓存（684x1216 4bpp）。
 * 封面来源：EPUB OPF -> CBZ 首图 -> 同名 .jpg/.png -> 实时纸书装帧封面。
 * English: Book cover thumbnail (144x196 4bpp) and full-screen lockscreen cover
 * cache (684x1216 4bpp). Sources: EPUB OPF -> CBZ first image -> sibling jpg/png -> paperback fallback.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BOOK_COVER_THUMB_W 144
#define BOOK_COVER_THUMB_H 196
#define BOOK_COVER_THUMB_BYTES (BOOK_COVER_THUMB_W * BOOK_COVER_THUMB_H / 2)

#define BOOK_COVER_DIR_DEFAULT "/sdcard/books/.covers"
#define BOOK_COVER_LOCK_FILENAME "lock_current.4bpp"

#ifdef __cplusplus
extern "C" {
#endif

/// 可选覆盖缓存目录（仅宿主测试使用；传 NULL 恢复默认 /sdcard/books/.covers）。
/// Optional override for cover cache directory (host tests only; NULL restores default).
void book_cover_set_cache_dir(const char *dir);

/// 加载或生成某本书的 144x196 4bpp 缩略图到 out_4bpp（至少 BOOK_COVER_THUMB_BYTES 字节）。
/// 返回 true 表示命中缓存或成功提取解码；返回 false 表示无图（调用方画纸书默认封面）。
/// Load or generate a 144x196 4bpp cover thumbnail into out_4bpp; returns false if no image exists.
bool book_cover_load_thumb(const char *book_path, uint32_t file_size,
                           uint8_t out_4bpp[BOOK_COVER_THUMB_BYTES]);

/// 清除指定书籍的封面缓存与内存 LRU 项（删书/换书联动调用）。
/// Remove cached thumbnail and LRU entry for book_path (called on delete/replace).
void book_cover_forget(const char *book_path);

/// 释放所有内存 LRU 缓存。/ Clear the in-memory cover LRU cache.
void book_cover_clear_lru(void);

/// 为当前阅读的书生成满屏 684x1216 4bpp 锁屏封面缓存（lock_current.4bpp）；
/// 若书籍无封图则删除旧缓存并返回 false（锁屏时回退实时纸书封）。
/// Build the full-screen 684x1216 4bpp lockscreen cover cache; deletes stale cache and returns false if text-only.
bool book_cover_save_lock_cache(const char *book_path, uint32_t file_size);

/// 极速读取满屏锁屏封面缓存（直接写入物理 684x1216 4bpp fb，耗时 <40ms）。
/// Fast-load the cached full-screen lockscreen cover directly into the 4bpp framebuffer (<40ms).
bool book_cover_load_lock_cache(uint8_t *fb, size_t fb_bytes);

#ifdef __cplusplus
}
#endif
