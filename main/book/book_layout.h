/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：借用章节 UTF-8 文本，生成分页并绘制正文；支持中文避头尾、连体标点保护、
 * 段距/段首缩进/温和齐行、逐字坐标命中、智能断句选区以及摘录下划线回显。
 * English: Paginate borrowed chapter UTF-8 text and draw its body; supports CJK
 * kinsoku, inseparable punctuation groups, paragraph spacing/indent/justification,
 * coordinate hit-testing, sentence selection bounds, and excerpt underline echo.
 *
 * 冻结：不释放原文、不刷新屏幕；调用方持有字体绘制互斥锁。
 * Frozen: Never free source text or present the display; caller holds the font draw lock.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "epdiy.h"
#include "html_text.h"

/// 可选排版增强参数。/ Optional typography refinement options.
typedef struct {
    uint8_t para_gap;     ///< 段间距：0 无 / 1 半行（默认）/ 2 一行 / Paragraph gap: 0 none, 1 half, 2 full
    uint8_t first_indent; ///< 段首缩进：0 跟随原文（默认）/ 1 强制两格 / First-line indent: 0 source, 1 force 2em
    bool justify;         ///< 非段末行温和齐行（默认关以兼容既有单测；阅读页开启）/ Gentle justification on non-final lines
} book_layout_opts_t;

/// 正文字节区间，用于已摘录下划线回显或当前活动选区高亮。
/// Byte range in chapter UTF-8 text for saved excerpt underlines or active selection.
typedef struct {
    size_t start_off;
    size_t end_off;
} book_text_span_t;

/// 设置后续排版与绘制使用的扩展参数；传入 NULL 恢复默认。
/// Configure extended typography options for subsequent builds; NULL restores defaults.
void book_layout_set_opts(const book_layout_opts_t* opts);

/// 设置当前章已摘录下划线区间表及当前活动选区；调用方保证数组在绘制期间存活。
/// Set saved excerpt underline spans and optional active selection span; caller keeps arrays alive during draw.
void book_layout_set_highlights(const book_text_span_t* underlines, size_t underline_count,
                                const book_text_span_t* active_selection);

/// 重排借用文本；空文一页，失败清空布局，超过 4096 页返回 false。/ Borrow and paginate; empty text has one page, failure clears layout, over 4096 pages fails.
bool book_layout_build(const char* utf8, size_t len, EpdRect rect, int px);
/// 使用指定行高百分比建立布局（135、150 或 175）；既有入口保持 150%。/ Build with a line-height percentage (135, 150 or 175); existing entry points keep 150%.
bool book_layout_build_blocks_metrics(const char* utf8, size_t len, const blk_t* blocks, size_t count,
                                     EpdRect rect, int px, unsigned line_height_percent);
/// 借用块表；标题字号加8，块间单换行；原文与块表须存活至free。/ Borrow blocks; headings add 8 px, with one newline between blocks; text and blocks must outlive layout.
bool book_layout_build_blocks(const char* utf8, size_t len, const blk_t* blocks, size_t count, EpdRect rect, int px);
/// 初始化并只排前两页；后续由调用方分批推进，原文继续借用。/ Initialize the first two pages; caller advances later batches while retaining source ownership.
bool book_layout_begin_blocks(const char* utf8, size_t len, const blk_t* blocks, size_t count, EpdRect rect, int px);
/// 使用指定行高百分比开始增量布局。/ Begin incremental pagination with the selected line-height percentage.
bool book_layout_begin_blocks_metrics(const char* utf8, size_t len, const blk_t* blocks, size_t count,
                                     EpdRect rect, int px, unsigned line_height_percent);
/// 最多增加指定页数；失败清空布局，调用方必须停止阅读。/ Add at most the requested pages; failure clears layout and requires ending reading.
bool book_layout_extend(size_t pages);
/// 是否完成整章分页。/ Whether the whole chapter has been paginated.
bool book_layout_complete(void);
/// 释放页表，不释放原文。/ Free layout storage, never the borrowed text.
void book_layout_free(void);
/// 返回已完成页数；未建立布局时为零，分批排版时不是整章总页数。/ Return completed pages, zero without a layout; incremental counts are not the chapter total.
size_t book_layout_page_count(void);
/// 使用建立布局时的宽高与字号绘图；不匹配或越界时不绘制。/ Draw with the built dimensions and size; mismatches or invalid pages do nothing.
void book_layout_draw_page(uint8_t* fb, size_t page, EpdRect rect, int px);
/// 命中本页未加载图片占位，返回块索引及可选矩形；未命中为 SIZE_MAX；与绘制使用相同锁。
/// Hit an unloaded image placeholder, returning its block index and optional bounds; SIZE_MAX if absent; use the drawing lock.
size_t book_layout_image_at(size_t page, EpdRect rect, int x, int y, EpdRect* hit);
/// 命中本页正文坐标 (x, y) 对应的 UTF-8 字节偏移（图片或空白返回 false）；可选返回所在行纵坐标。
/// Hit-test (x, y) on a page to a UTF-8 byte offset (false on blanks/images); optionally outputs line top/height.
bool book_layout_hit_text(size_t page, EpdRect rect, int x, int y, size_t* out_off, int* out_line_y);
/// 按中英文句末标点将字节偏移扩展为当前完整一句，并裁剪到 [clamp_start, clamp_end]。
/// Expand a byte offset to its enclosing sentence using CJK/ASCII terminators, clamped to [clamp_start, clamp_end].
bool book_layout_sentence_bounds(const char* utf8, size_t len, size_t off,
                                 size_t clamp_start, size_t clamp_end,
                                 size_t* out_start, size_t* out_end);
/// 向前扩选一句（不越过 clamp_start）。/ Expand selection backward by one sentence (clamped to clamp_start).
bool book_layout_expand_prev_sentence(const char* utf8, size_t len,
                                      size_t cur_start, size_t clamp_start,
                                      size_t* out_start);
/// 向后扩选一句（不越过 clamp_end）。/ Expand selection forward by one sentence (clamped to clamp_end).
bool book_layout_expand_next_sentence(const char* utf8, size_t len,
                                      size_t cur_end, size_t clamp_end,
                                      size_t* out_end);
/// 返回包含偏移 off 的完整段落区间（不含首尾换行）。/ Return the full paragraph span enclosing off.
bool book_layout_paragraph_bounds(const char* utf8, size_t len, size_t off,
                                  size_t* out_start, size_t* out_end);
/// 查找字节偏移所属页，越界偏移夹到末页。/ Find page containing a byte offset; excessive offsets clamp to the last page.
size_t book_layout_page_for_offset(size_t off);
/// 返回页首偏移；已完成页数索引是待排页起点（完成后为文本末尾），更大索引返回文本长度。
/// Return a page start; index equal to completed count marks the pending page (EOF when complete), larger indexes return text length.
size_t book_layout_page_start_offset(size_t page);
/// 将基础正文区域收窄为给定左右边距，失败时返回空矩形。/ Apply symmetric side margins to a base text area; return empty on invalid input.
EpdRect book_layout_side_margins(EpdRect rect, unsigned margin);
/// 在 UTF-8 文本中查找匹配并返回字节偏移；ASCII 忽略大小写，其余码点按字节匹配。
/// Search UTF-8 text and return byte offsets; ASCII is case-insensitive and non-ASCII code points match exactly.
size_t book_text_search(const char* utf8, size_t len, const char* query,
                        size_t from, size_t* offsets, size_t capacity);
