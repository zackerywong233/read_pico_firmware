/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：书源编码和章节解析，不依赖界面。
 * English: Source encoding and chapter parsing without UI dependencies.
 * 冻结：保留原文件偏移，无效编码替换，不写文件。
 * Frozen: Preserve original byte offsets, replace invalid encoding, never write files.
 */
#pragma once
#include "book_source.h"
#include <stdbool.h>
#include <stdio.h>
#include "html_text.h"
#define BOOK_CHAPTER_MAX 2048
#define BOOK_CHAPTER_BYTES_MAX (96 * 1024)
typedef struct {
    uint32_t offset; ///< 原文件偏移 / Original file offset
    char title[41]; ///< UTF-8 标题 / UTF-8 title
} book_entry_t;
typedef struct {
    FILE *file; ///< 文件句柄 / File handle
    book_entry_t *entries; ///< PSRAM 目录 / PSRAM index
    size_t count; ///< 目录条数 / Entry count
    uint32_t total; ///< 文件长度 / File length
    bool gbk; ///< GBK 编码 / GBK encoding
    uint32_t bom; ///< BOM 字节数 / BOM size
} book_txt_t;
/// 建立 TXT 目录。/ Build the TXT index.
esp_err_t book_txt_open(book_txt_t *book, const char *path);
/// 加载一章并转为 UTF-8。/ Load a chapter as UTF-8.
esp_err_t book_txt_load(book_txt_t *book, size_t i, char **utf8, size_t *len);

typedef struct book_md book_md_t;
esp_err_t book_md_open(const char *path, book_md_t **out);
void book_md_close(book_md_t *book);
size_t book_md_chapter_count(const book_md_t *book);
esp_err_t book_md_chapter_title(const book_md_t *book, size_t i, char *buf, size_t cap);
esp_err_t book_md_load(const book_md_t *book, size_t i, html_text_t *out);
uint32_t book_md_total_bytes(const book_md_t *book);
uint32_t book_md_chapter_offset(const book_md_t *book, size_t i);

typedef struct book_cbz book_cbz_t;
esp_err_t book_cbz_open(const char *path, book_cbz_t **out);
void book_cbz_close(book_cbz_t *book);
size_t book_cbz_chapter_count(const book_cbz_t *book);
esp_err_t book_cbz_chapter_title(const book_cbz_t *book, size_t i, char *buf, size_t cap);
esp_err_t book_cbz_load(const book_cbz_t *book, size_t i, html_text_t *out);
esp_err_t book_cbz_load_image(book_cbz_t *book, size_t chapter, const char *reference,
                              uint8_t **pixels, uint16_t *width, uint16_t *height);
uint32_t book_cbz_total_bytes(const book_cbz_t *book);
uint32_t book_cbz_chapter_offset(const book_cbz_t *book, size_t i);
