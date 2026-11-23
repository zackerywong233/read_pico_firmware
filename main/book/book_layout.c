/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：逐码点折行、PSRAM 章节分页、中文避头尾与连体标点保护、段距缩进与温和齐行、
 * 正文坐标命中、智能断句选区以及已摘录下划线回显。
 * English: Codepoint wrapping, PSRAM chapter pagination, CJK kinsoku & punctuation-group
 * protection, paragraph spacing/indent/justification, coordinate hit-test, sentence bounds,
 * and saved-excerpt underline echo.
 *
 * 冻结：原文由调用方持有；字体测量和绘制必须由调用方串行化。
 * 为尽早进入阅读，支持分批分页；未加载图片绘制可点击占位及已读重复提示，不在绘图时读取资源或解码。
 * Frozen: Caller owns source text and serializes all font measurement and drawing.
 * Incremental pagination enables early reading; unloaded images show clickable placeholders
 * and visited-chapter repeat hints; never read or decode resources while drawing.
 */
#include "book_layout.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "ttf_font.h"

#define PAGE_MAX 4096u
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

static const char* s_text;
static size_t s_len;
static size_t* s_pages;
static size_t s_count;
static size_t s_capacity;
static char* s_line;
static EpdRect s_rect;
static int s_px;
static unsigned s_line_height_percent;
static size_t s_scan;
static int64_t s_used;
static bool s_complete;

static const blk_t* s_blocks;
static size_t s_block_count;

static book_layout_opts_t s_opts = { .para_gap = 1, .first_indent = 0, .justify = false };
static const book_text_span_t* s_underlines;
static size_t s_underline_count;
static bool s_has_selection;
static book_text_span_t s_selection;

void book_layout_set_opts(const book_layout_opts_t* opts) {
    if (!opts) {
        s_opts = (book_layout_opts_t){ .para_gap = 1, .first_indent = 0, .justify = false };
        return;
    }
    s_opts.para_gap = opts->para_gap <= 2 ? opts->para_gap : 1;
    s_opts.first_indent = opts->first_indent <= 1 ? opts->first_indent : 0;
    s_opts.justify = opts->justify;
}

void book_layout_set_highlights(const book_text_span_t* underlines, size_t underline_count,
                                const book_text_span_t* active_selection) {
    s_underlines = underlines;
    s_underline_count = underlines ? underline_count : 0;
    if (active_selection && active_selection->end_off > active_selection->start_off) {
        s_has_selection = true;
        s_selection = *active_selection;
    } else {
        s_has_selection = false;
        s_selection = (book_text_span_t){ 0, 0 };
    }
}

static bool ascii_word_char(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-';
}

static uint32_t utf8_decode_cp(const char* glyph, size_t len) {
    if (!glyph || !len) return 0;
    const unsigned char* u = (const unsigned char*)glyph;
    if (len == 1) return u[0];
    if (len == 2) return ((uint32_t)(u[0] & 0x1Fu) << 6) | (u[1] & 0x3Fu);
    if (len == 3) return ((uint32_t)(u[0] & 0x0Fu) << 12) | ((uint32_t)(u[1] & 0x3Fu) << 6) | (u[2] & 0x3Fu);
    if (len == 4) return ((uint32_t)(u[0] & 0x07u) << 18) | ((uint32_t)(u[1] & 0x3Fu) << 12) |
                         ((uint32_t)(u[2] & 0x3Fu) << 6) | (u[3] & 0x3Fu);
    return 0;
}

static bool cp_in_set(uint32_t cp, const uint32_t* set, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (set[i] == cp) return true;
    }
    return false;
}

static bool line_closer(const char* glyph, size_t len) {
    static const uint32_t values[] = {
        0xFF0Cu, 0x3002u, 0xFF01u, 0xFF1Fu, 0x3001u, 0xFF1Bu, 0xFF1Au, // ，。！？、；：
        0x300Bu, 0x201Du, 0x2019u, 0x3011u, 0x300Du, 0x300Fu,          // 》”’】」』
        0xFF09u, 0x3009u, 0x3015u, 0x2026u, 0x2014u, 0x00B7u,          // ）〉〕…—·
        0xFF5Eu, 0xFF05u, 0x2030u, 0x2103u,                            // ～％‰℃
        ',', '.', '!', '?', ';', ':', ')', ']', '}', '%'
    };
    return cp_in_set(utf8_decode_cp(glyph, len), values, sizeof(values) / sizeof(values[0]));
}

static bool line_opener(const char* glyph, size_t len) {
    static const uint32_t values[] = {
        0xFF08u, 0x300Au, 0x201Cu, 0x2018u, 0x3010u, 0x300Cu, 0x300Eu, // （《“‘【「『
        0x3008u, 0x3014u, 0xFFE5u, '$', '(', '[', '{'                  // 〈〔￥$([{
    };
    return cp_in_set(utf8_decode_cp(glyph, len), values, sizeof(values) / sizeof(values[0]));
}

static bool is_closing_quote_or_paren(const char* glyph, size_t len) {
    static const uint32_t values[] = {
        0x201Du, 0x2019u, 0xFF09u, 0x3011u, 0x300Du, 0x300Fu,          // ”’）】」』
        0x300Bu, 0x3009u, 0x3015u, ')', ']', '}'                       // 》〉〕)]}
    };
    return cp_in_set(utf8_decode_cp(glyph, len), values, sizeof(values) / sizeof(values[0]));
}

static bool is_sentence_stop(const char* glyph, size_t len) {
    static const uint32_t values[] = { 0x3002u, 0xFF01u, 0xFF1Fu, 0xFF1Bu, '.', '!', '?' };
    return cp_in_set(utf8_decode_cp(glyph, len), values, sizeof(values) / sizeof(values[0]));
}

// 拒绝截断、过长编码、代理项和嵌入零字节。/ Reject truncation, overlong encodings, surrogates and embedded NUL.
static size_t codepoint_size(const char* text, size_t remaining) {
    if (!remaining) return 0;
    const unsigned char* p = (const unsigned char*)text;
    if (p[0] > 0 && p[0] < 0x80) return 1;
    size_t n = p[0] >= 0xc2 && p[0] <= 0xdf ? 2 :
               p[0] >= 0xe0 && p[0] <= 0xef ? 3 :
               p[0] >= 0xf0 && p[0] <= 0xf4 ? 4 : 0;
    if (!n || n > remaining) return 0;
    for (size_t i = 1; i < n; i++) if ((p[i] & 0xc0) != 0x80) return 0;
    if ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] >= 0xa0) ||
        (p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] >= 0x90)) return 0;
    return n;
}

// 识别 2~3 码点连体标点组（……、——、。”、！）等），防止跨行拆断。
// Detect 2-3 codepoint inseparable punctuation groups (……, ——, 。” etc.) so they never split across lines.
static size_t punct_group_len(const char* text, size_t limit, size_t off) {
    if (off >= limit) return 0;
    size_t n1 = codepoint_size(text + off, limit - off);
    if (!n1) return 0;
    size_t total = n1;
    // 连体省略号或破折号：…… / ——。/ Repeated ellipsis or em-dash.
    if ((n1 == 3 && (!memcmp(text + off, "…", 3) || !memcmp(text + off, "—", 3)))) {
        while (off + total < limit && total < 9) {
            size_t nk = codepoint_size(text + off + total, limit - (off + total));
            if (nk == n1 && !memcmp(text + off + total, text + off, n1)) total += nk;
            else break;
        }
        // 后随收引号/括号时一并保护。/ Also attach immediately following closing quote/paren.
        if (off + total < limit) {
            size_t nk = codepoint_size(text + off + total, limit - (off + total));
            if (nk && is_closing_quote_or_paren(text + off + total, nk)) total += nk;
        }
        return total;
    }
    // 句末标点紧跟收引号/收括号：。” ！） ？” 等。/ Sentence terminator + closing quote/paren.
    if (is_sentence_stop(text + off, n1) && off + n1 < limit) {
        size_t n2 = codepoint_size(text + off + n1, limit - (off + n1));
        if (n2 && is_closing_quote_or_paren(text + off + n1, n2)) {
            total += n2;
            if (off + total < limit) {
                size_t n3 = codepoint_size(text + off + total, limit - (off + total));
                if (n3 && is_closing_quote_or_paren(text + off + total, n3)) total += n3;
            }
            return total;
        }
    }
    return n1;
}

// 块表是有序字节区间，二分查找当前行样式。/ Blocks are ordered byte ranges; binary-search the line style.
static const blk_t* block_at(size_t off) {
    if (!s_block_count) return NULL;
    size_t lo = 0, hi = s_block_count;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s_blocks[mid].offset <= off) lo = mid;
        else hi = mid;
    }
    return &s_blocks[lo];
}

// 插图等比适配正文区域，不放大；整块换页，不切断图片。
// Fit illustrations proportionally without upscaling; move whole images across page boundaries.
static void image_size(const blk_t* block, int* width, int* height) {
    *width = block->image_width; *height = block->image_height;
    if (*width > s_rect.width) { *height = (int)((int64_t)*height * s_rect.width / *width); *width = s_rect.width; }
    if (*height > s_rect.height) { *width = (int)((int64_t)*width * s_rect.height / *height); *height = s_rect.height; }
    if (*width < 1) *width = 1;
    if (*height < 1) *height = 1;
}

static int row_height(const blk_t* block, int px) {
    if (block && block->image) { int width, height; image_size(block, &width, &height); return height; }
    if (block && block->image_src) return 2 * s_px < s_rect.height ? 2 * s_px : s_rect.height;
    return (int)(((int64_t)px * s_line_height_percent) / 100);
}

static int paragraph_extra(int line_height, bool heading) {
    if (heading) return line_height / 2;
    if (s_opts.para_gap == 0) return 0;
    if (s_opts.para_gap == 2) return line_height;
    return line_height / 3;
}

static bool is_paragraph_start(size_t off, const blk_t* block) {
    if (block) return off == block->offset;
    return off == 0 || s_text[off - 1] == '\n' || s_text[off - 1] == '\r';
}

static int line_indent_px(size_t off, const blk_t* block, int px, bool heading) {
    if (s_opts.first_indent != 1 || heading || !is_paragraph_start(off, block)) return 0;
    if (off >= s_len) return 0;
    // 原文已有空格或全角缩进时不重复叠加。/ Skip if source already starts with space or ideographic space.
    unsigned char c = (unsigned char)s_text[off];
    if (c == ' ' || c == '\t') return 0;
    if (off + 3 <= s_len && !memcmp(s_text + off, "\xE3\x80\x80", 3)) return 0;
    int indent = 2 * px;
    return indent < s_rect.width / 2 ? indent : 0;
}

static EpdRect placeholder_rect(EpdRect body, int top, int height) {
    int pad = body.width > 24 && height > 16 ? 8 : 0;
    return (EpdRect){body.x + pad, body.y + top + pad / 2, body.width - 2 * pad, height - pad};
}

static void draw_placeholder(uint8_t* fb, const blk_t* block, EpdRect rect) {
    for (int x = rect.x; x < rect.x + rect.width; ++x) {
        epd_draw_pixel(x, rect.y, 0, fb); epd_draw_pixel(x, rect.y + rect.height - 1, 0, fb);
    }
    for (int y = rect.y; y < rect.y + rect.height; ++y) {
        epd_draw_pixel(rect.x, y, 0, fb); epd_draw_pixel(rect.x + rect.width - 1, y, 0, fb);
    }
    int px = s_px < 32 ? s_px : 32;
    bool repeat = block->image_repeated && !block->image_title;
    const char* label = block->image_title ? "标题图 · 点击查看" : repeat ? "重复图片 · 点击查看" : "点击查看图片";
    if (ttf_text_width_px(px, label) > rect.width - 16) return;
    int lines = repeat ? 2 : 1;
    int top = rect.y + (rect.height - lines * (px + 4)) / 2;
    ttf_draw_text_px(fb, rect.x + rect.width / 2, top + ttf_ascender_px(px), px,
                     label, EPD_DRAW_ALIGN_CENTER, 0, 15);
    if (repeat) {
        char origin[64]; snprintf(origin, sizeof(origin), "已读最早：第 %u 节", (unsigned)block->image_first_chapter + 1);
        int small = px < 24 ? px : 24;
        if (ttf_text_width_px(small, origin) <= rect.width - 16)
            ttf_draw_text_px(fb, rect.x + rect.width / 2, top + px + 4 + ttf_ascender_px(small), small,
                             origin, EPD_DRAW_ALIGN_CENTER, 0, 15);
    }
}

void book_layout_free(void) {
    free(s_pages);
    free(s_line);
    s_pages = NULL;
    s_line = NULL;
    s_text = NULL;
    s_count = s_capacity = s_len = 0;
    s_px = 0;
    s_line_height_percent = 150;
    s_scan = 0;
    s_used = 0;
    s_complete = false;
    s_blocks = NULL;
    s_block_count = 0;
}

static bool append_page(size_t off) {
    if (s_count == PAGE_MAX) return false;
    if (s_count == s_capacity) {
        size_t cap = s_capacity ? s_capacity * 2 : 16;
        size_t* pages = heap_caps_realloc(s_pages, cap * sizeof(*pages), PSRAM_CAPS);
        if (!pages) return false;
        s_pages = pages;
        s_capacity = cap;
    }
    s_pages[s_count++] = off;
    return true;
}

// 折行时保留原文字节位置；CRLF 算一个段落边界。
// Preserve source offsets while wrapping; CRLF is one paragraph boundary.
static bool take_line(size_t off, size_t* next, size_t* text_end_out,
                      bool* paragraph_end, int* px, bool* heading, int* indent_out,
                      int64_t* width_out) {
    const blk_t* block = block_at(off);
    *heading = block && block->heading;
    *px = s_px + (*heading ? 8 : 0);
    int indent = line_indent_px(off, block, *px, *heading);
    if (indent_out) *indent_out = indent;
    int avail_w = s_rect.width - indent;
    if (avail_w <= 0) avail_w = s_rect.width;
    size_t limit = block ? block->offset + block->len : s_len;
    size_t end = off;
    int64_t width = 0;
    s_line[0] = 0;
    *paragraph_end = false;
    if (block && (block->image || block->image_src)) {
        *heading = false;
        if (indent_out) *indent_out = 0;
        if (text_end_out) *text_end_out = limit;
        if (width_out) *width_out = 0;
        *next = limit + (limit < s_len && s_text[limit] == '\n');
        return *next > off;
    }
    while (end < limit && s_text[end] != '\r' && s_text[end] != '\n') {
        size_t n = codepoint_size(s_text + end, s_len - end);
        if (!n) return false;
        // 识别连体标点组（……、——、。”等）作为不可拆整体。
        // Measure inseparable punctuation groups atomically so they never break mid-group.
        size_t glen = punct_group_len(s_text, limit, end);
        if (glen > n && end > off) {
            int64_t gw = 0;
            size_t pos = 0;
            while (pos < glen) {
                size_t gn = codepoint_size(s_text + end + pos, glen - pos);
                if (!gn) return false;
                char gbuf[5]; memcpy(gbuf, s_text + end + pos, gn); gbuf[gn] = 0;
                int adv = ttf_text_width_px(*px, gbuf);
                if (adv < 0) return false;
                gw += adv;
                pos += gn;
            }
            if (width + gw > avail_w) {
                // 连体标点组本身若是行首禁则（如 。” 或 ……），回退一个前置字一起下移。
                // If the group starts with a closer, pull one preceding glyph down with it.
                if (line_closer(s_text + end, n)) {
                    size_t previous = end - off;
                    do { --previous; } while (previous > 0 && (((unsigned char)s_line[previous] & 0xc0) == 0x80));
                    if (previous > 0) {
                        end = off + previous;
                        s_line[previous] = 0;
                    }
                }
                break;
            }
            width += gw;
            memcpy(s_line + end - off, s_text + end, glen);
            s_line[end - off + glen] = 0;
            end += glen;
            continue;
        }
        char glyph[5];
        memcpy(glyph, s_text + end, n);
        glyph[n] = 0;
        int64_t candidate = width + ttf_text_width_px(*px, glyph);
        if (candidate < 0) return false;
        if (width > 0 && ascii_word_char((unsigned char)glyph[0]) &&
            !ascii_word_char((unsigned char)s_text[end - 1])) {
            size_t word_end = end;
            int64_t word_width = 0;
            while (word_end < limit && ascii_word_char((unsigned char)s_text[word_end])) {
                char word_glyph[2] = {s_text[word_end], 0};
                int advance = ttf_text_width_px(*px, word_glyph);
                if (advance < 0 || word_width > INT64_MAX - advance) return false;
                word_width += advance;
                ++word_end;
            }
            if (word_width <= avail_w && width > 0 && width + word_width > avail_w) break;
        }
        if (line_opener(glyph, n)) {
            size_t following = end + n;
            if (following < limit && s_text[following] != '\r' && s_text[following] != '\n') {
                size_t following_size = codepoint_size(s_text + following, s_len - following);
                if (!following_size) return false;
                char following_glyph[5];
                memcpy(following_glyph, s_text + following, following_size);
                following_glyph[following_size] = 0;
                int next_width = ttf_text_width_px(*px, following_glyph);
                if (next_width < 0) return false;
                if (candidate <= avail_w && candidate + next_width > avail_w && end > off) break;
            }
        }
        if (candidate > avail_w) {
            if (end == off) return false;
            if (line_closer(glyph, n)) {
                // 温和齐行开启时允许单个行尾标点轻度悬挂（≤ px/2），否则回退一字避免标点落行首。
                // With justification enabled allow slight punctuation hang (<= px/2), else pull back one glyph.
                if (s_opts.justify && candidate - avail_w <= *px / 2) {
                    width = candidate;
                    memcpy(s_line + end - off, glyph, n);
                    s_line[end - off + n] = 0;
                    end += n;
                    break;
                }
                size_t previous = end - off;
                do { --previous; } while (previous > 0 && (((unsigned char)s_line[previous] & 0xc0) == 0x80));
                if (previous > 0) {
                    end = off + previous;
                    s_line[previous] = 0;
                } else {
                    memcpy(s_line + end - off, glyph, n);
                    s_line[end - off + n] = 0;
                    end += n;
                }
            }
            break;
        }
        width = candidate;
        memcpy(s_line + end - off, glyph, n);
        s_line[end - off + n] = 0;
        end += n;
    }
    if (text_end_out) *text_end_out = end;
    if (width_out) *width_out = width;
    *next = end;
    if (end < s_len && (s_text[end] == '\r' || s_text[end] == '\n')) {
        *next = end + 1;
        if (s_text[end] == '\r' && *next < s_len && s_text[*next] == '\n') (*next)++;
        *paragraph_end = true;
    }
    return *next > off;
}

bool book_layout_build(const char* utf8, size_t len, EpdRect rect, int px) {
    return book_layout_build_blocks(utf8, len, NULL, 0, rect, px);
}

bool book_layout_build_blocks(const char* utf8, size_t len, const blk_t* blocks, size_t count, EpdRect rect, int px) {
    return book_layout_build_blocks_metrics(utf8, len, blocks, count, rect, px, 150);
}

bool book_layout_build_blocks_metrics(const char* utf8, size_t len, const blk_t* blocks, size_t count,
                                     EpdRect rect, int px, unsigned line_height_percent) {
    return book_layout_begin_blocks_metrics(utf8, len, blocks, count, rect, px, line_height_percent) && book_layout_extend(PAGE_MAX);
}

bool book_layout_begin_blocks(const char* utf8, size_t len, const blk_t* blocks, size_t count, EpdRect rect, int px) {
    return book_layout_begin_blocks_metrics(utf8, len, blocks, count, rect, px, 150);
}

bool book_layout_begin_blocks_metrics(const char* utf8, size_t len, const blk_t* blocks, size_t count,
                                     EpdRect rect, int px, unsigned line_height_percent) {
    book_layout_free();
    if ((!utf8 && len) || len == SIZE_MAX || px <= 0 || px > INT_MAX / 3 ||
        rect.width <= 0 || rect.height <= 0 || rect.x < 0 || rect.y < 0 ||
        rect.x > INT_MAX - rect.width || rect.y > INT_MAX - rect.height ||
        (line_height_percent != 135 && line_height_percent != 150 && line_height_percent != 175)) return false;
    s_line_height_percent = line_height_percent;
    int line_height = (int)(((int64_t)px * line_height_percent) / 100);
    if (line_height > rect.height) return false;
    for (size_t off = 0; off < len;) {
        size_t n = codepoint_size(utf8 + off, len - off);
        if (!n) return false;
        off += n;
    }
    if (count) {
        if (!blocks || count > HTML_TEXT_MAX_BLOCKS) return false;
        size_t expected = 0;
        for (size_t i = 0; i < count; ++i) {
            const blk_t* b = &blocks[i];
            if (b->image && (!b->image_width || !b->image_height)) return false;
            if (b->offset != expected || b->offset >= len || !b->len || b->len > len - b->offset ||
                ((unsigned char)utf8[b->offset] & 0xc0) == 0x80) return false;
            size_t end = b->offset + b->len;
            if (i + 1 < count) {
                if (end >= len || utf8[end] != '\n') return false;
                expected = end + 1;
            } else if (end != len) return false;
        }
    }
    s_blocks = blocks;
    s_block_count = count;
    s_text = utf8;
    s_len = len;
    s_px = px;
    s_rect = rect;
    s_line = heap_caps_malloc(len + 1, PSRAM_CAPS);
    if (!s_line || !append_page(0)) goto fail;
    return book_layout_extend(2);
fail:
    book_layout_free();
    return false;
}

EpdRect book_layout_side_margins(EpdRect rect, unsigned margin) {
    if (rect.width <= 0 || margin > (unsigned)(rect.width / 2)) return (EpdRect){0};
    rect.x += (int)margin;
    rect.width -= (int)(2 * margin);
    return rect;
}

size_t book_text_search(const char* utf8, size_t len, const char* query,
                        size_t from, size_t* offsets, size_t capacity) {
    if ((!utf8 && len) || !query || !query[0]) return 0;
    size_t query_len = strlen(query);
    if (!query_len || query_len > len || from > len) return 0;
    for (size_t q = 0; q < query_len;) {
        size_t size = codepoint_size(query + q, query_len - q);
        if (!size) return 0;
        q += size;
    }
    size_t found = 0;
    for (size_t i = from; i + query_len <= len; ++i) {
        if (i && (((unsigned char)utf8[i] & 0xc0) == 0x80)) continue;
        if (i + query_len < len && (((unsigned char)utf8[i + query_len] & 0xc0) == 0x80)) continue;
        bool match = true;
        for (size_t j = 0; j < query_len; ++j) {
            unsigned char a = (unsigned char)utf8[i + j];
            unsigned char b = (unsigned char)query[j];
            if (a < 0x80 && b < 0x80 && a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
            if (a < 0x80 && b < 0x80 && b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
            if (a != b) { match = false; break; }
        }
        if (match) {
            if (offsets && found < capacity) offsets[found] = i;
            ++found;
        }
    }
    return found < capacity ? found : capacity;
}

bool book_layout_extend(size_t pages) {
    if (!s_count) return false;
    if (!pages || s_complete) return true;
    size_t target = pages > PAGE_MAX - book_layout_page_count() ? PAGE_MAX : book_layout_page_count() + pages;
    while (s_scan < s_len) {
        size_t next;
        bool paragraph_end, heading;
        int line_px;
        if (!take_line(s_scan, &next, NULL, &paragraph_end, &line_px, &heading, NULL, NULL)) goto fail;
        const blk_t* block = block_at(s_scan);
        int line_height = row_height(block, line_px);
        if (line_height > s_rect.height) goto fail;
        if (s_used + line_height > s_rect.height) {
            if (!append_page(s_scan)) goto fail;
            s_used = 0;
            if (book_layout_page_count() >= target) return true;
        }
        s_used += line_height;
        if (paragraph_end) s_used += paragraph_extra(line_height, heading);
        s_scan = next;
    }
    s_complete = true;
    return true;
fail:
    book_layout_free();
    return false;
}

size_t book_layout_page_count(void) { return s_count ? s_count - !s_complete : 0; }
bool book_layout_complete(void) { return s_complete; }

size_t book_layout_page_start_offset(size_t page) {
    return page < s_count ? s_pages[page] : s_len;
}

size_t book_layout_page_for_offset(size_t off) {
    size_t lo = 0, hi = book_layout_page_count();
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (s_pages[mid] <= off) lo = mid;
        else hi = mid;
    }
    return lo;
}

// 计算一行内字节偏移对应的横坐标（含齐行间距分配）。
// Compute x position for a byte offset within a line (accounting for justification).
static int line_x_for_offset(size_t line_start, size_t line_end, size_t target_off,
                             int base_x, int px, int extra_total, int glyph_gaps) {
    if (target_off <= line_start) return base_x;
    size_t cur = line_start;
    int x = base_x;
    int idx = 0;
    while (cur < line_end && cur < target_off) {
        size_t n = codepoint_size(s_text + cur, line_end - cur);
        if (!n) break;
        char g[5]; memcpy(g, s_text + cur, n); g[n] = 0;
        x += ttf_text_width_px(px, g);
        if (glyph_gaps > 0 && extra_total > 0 && cur + n < line_end) {
            int next_extra = (extra_total * (idx + 1)) / glyph_gaps;
            int prev_extra = (extra_total * idx) / glyph_gaps;
            x += next_extra - prev_extra;
        }
        cur += n;
        idx++;
    }
    return x;
}

static void draw_span_decor(uint8_t* fb, int x0, int x1, int top_y, int line_px, bool is_selection) {
    if (x1 <= x0) return;
    if (is_selection) {
        // 活动选区用浅灰底色高亮并加底线。/ Highlight active selection with light gray fill + underline.
        for (int y = top_y; y < top_y + line_px + 4; ++y) {
            for (int x = x0; x < x1; ++x) epd_draw_pixel(x, y, 0xD0, fb);
        }
        int uy = top_y + line_px + 4;
        for (int x = x0; x < x1; ++x) {
            epd_draw_pixel(x, uy, 0, fb);
            epd_draw_pixel(x, uy + 1, 0, fb);
        }
    } else {
        // 已保存摘录用实体书风格细下划线回显。/ Saved excerpts draw a subtle underline echo.
        int uy = top_y + line_px + 3;
        for (int x = x0; x < x1; ++x) {
            if ((x & 3) != 3) epd_draw_pixel(x, uy, 0, fb);
            if ((x & 3) != 3) epd_draw_pixel(x, uy + 1, 0, fb);
        }
    }
}

void book_layout_draw_page(uint8_t* fb, size_t page, EpdRect rect, int px) {
    if (!fb || page >= book_layout_page_count() || px != s_px || rect.width != s_rect.width ||
        rect.height != s_rect.height || rect.x < 0 || rect.y < 0 ||
        rect.x > INT_MAX - rect.width || rect.y > INT_MAX - rect.height) return;
    size_t off = s_pages[page];
    size_t end = page + 1 < s_count ? s_pages[page + 1] : s_len;
    int64_t used = 0;
    while (off < end) {
        size_t next, text_end;
        bool paragraph_end, heading;
        int line_px, indent = 0;
        int64_t line_w = 0;
        if (!take_line(off, &next, &text_end, &paragraph_end, &line_px, &heading, &indent, &line_w)) return;
        const blk_t* block = block_at(off);
        int line_height = row_height(block, line_px);
        int image_width = 0;
        if (block && block->image) image_size(block, &image_width, &line_height);
        if (used + line_height > rect.height) return;
        if (image_width) {
            int left = rect.x + (rect.width - image_width) / 2;
            for (int y = 0; y < line_height; ++y) for (int x = 0; x < image_width; ++x) {
                size_t src = (size_t)((int64_t)y * block->image_height / line_height) * block->image_width +
                             (size_t)((int64_t)x * block->image_width / image_width);
                epd_draw_pixel(left + x, rect.y + (int)used + y, block->image[src], fb);
            }
        } else if (block && block->image_src) {
            draw_placeholder(fb, block, placeholder_rect(rect, (int)used, line_height));
        } else if (s_line[0]) {
            int base_x = rect.x + indent;
            int line_top = rect.y + (int)used;
            int avail_w = rect.width - indent;
            int glyph_count = 0;
            int extra_total = 0;
            if (s_opts.justify && !paragraph_end && !heading && text_end > off) {
                for (size_t p = off; p < text_end;) {
                    size_t n = codepoint_size(s_text + p, text_end - p);
                    if (!n) break;
                    glyph_count++;
                    p += n;
                }
                int rem = avail_w - (int)line_w;
                if (glyph_count >= 2 && rem > 0 && rem <= line_px * 2) {
                    int max_extra = (glyph_count - 1) * (line_px / 3 + 1);
                    extra_total = rem <= max_extra ? rem : max_extra;
                }
            }
            int glyph_gaps = glyph_count > 1 ? glyph_count - 1 : 0;

            // 先绘活动选区浅灰底，再绘正文文字，最后绘已摘录下划线。
            // Draw selection background first, then text glyphs, then saved-excerpt underlines.
            if (s_has_selection && s_selection.end_off > off && s_selection.start_off < text_end) {
                size_t s0 = s_selection.start_off > off ? s_selection.start_off : off;
                size_t s1 = s_selection.end_off < text_end ? s_selection.end_off : text_end;
                int x0 = line_x_for_offset(off, text_end, s0, base_x, line_px, extra_total, glyph_gaps);
                int x1 = line_x_for_offset(off, text_end, s1, base_x, line_px, extra_total, glyph_gaps);
                draw_span_decor(fb, x0, x1, line_top, line_px, true);
            }

            if (extra_total > 0 && glyph_gaps > 0) {
                int gx = base_x;
                int idx = 0;
                for (size_t p = off; p < text_end;) {
                    size_t n = codepoint_size(s_text + p, text_end - p);
                    if (!n) break;
                    char g[5]; memcpy(g, s_text + p, n); g[n] = 0;
                    ttf_draw_text_px(fb, gx, line_top + ttf_ascender_px(line_px), line_px,
                                     g, EPD_DRAW_ALIGN_LEFT, 0, 15);
                    gx += ttf_text_width_px(line_px, g);
                    if (p + n < text_end) {
                        int next_extra = (extra_total * (idx + 1)) / glyph_gaps;
                        int prev_extra = (extra_total * idx) / glyph_gaps;
                        gx += next_extra - prev_extra;
                    }
                    p += n;
                    idx++;
                }
            } else {
                ttf_draw_text_px(fb, base_x, line_top + ttf_ascender_px(line_px), line_px,
                                 s_line, EPD_DRAW_ALIGN_LEFT, 0, 15);
            }

            for (size_t u = 0; u < s_underline_count; ++u) {
                if (s_underlines[u].end_off <= off || s_underlines[u].start_off >= text_end) continue;
                size_t s0 = s_underlines[u].start_off > off ? s_underlines[u].start_off : off;
                size_t s1 = s_underlines[u].end_off < text_end ? s_underlines[u].end_off : text_end;
                int x0 = line_x_for_offset(off, text_end, s0, base_x, line_px, extra_total, glyph_gaps);
                int x1 = line_x_for_offset(off, text_end, s1, base_x, line_px, extra_total, glyph_gaps);
                draw_span_decor(fb, x0, x1, line_top, line_px, false);
            }
        }
        used += line_height;
        if (paragraph_end) used += paragraph_extra(line_height, heading);
        off = next;
    }
}

size_t book_layout_image_at(size_t page, EpdRect rect, int x, int y, EpdRect* hit) {
    if (page >= book_layout_page_count() || rect.width != s_rect.width || rect.height != s_rect.height ||
        rect.x < 0 || rect.y < 0 || rect.x > INT_MAX - rect.width || rect.y > INT_MAX - rect.height ||
        x < rect.x || y < rect.y || x >= rect.x + rect.width || y >= rect.y + rect.height) return SIZE_MAX;
    size_t off = s_pages[page], end = page + 1 < s_count ? s_pages[page + 1] : s_len;
    int64_t used = 0;
    while (off < end) {
        size_t next; bool paragraph_end, heading; int px;
        if (!take_line(off, &next, NULL, &paragraph_end, &px, &heading, NULL, NULL)) return SIZE_MAX;
        const blk_t* block = block_at(off);
        int height = row_height(block, px);
        if (used + height > rect.height) return SIZE_MAX;
        if (block && block->image_src && !block->image) {
            EpdRect area = placeholder_rect(rect, (int)used, height);
            if (x >= area.x && x < area.x + area.width && y >= area.y && y < area.y + area.height) {
                if (hit) *hit = area;
                return (size_t)(block - s_blocks);
            }
        }
        used += height;
        if (paragraph_end) used += paragraph_extra(height, heading);
        off = next;
    }
    return SIZE_MAX;
}

bool book_layout_hit_text(size_t page, EpdRect rect, int x, int y, size_t* out_off, int* out_line_y) {
    if (!out_off || page >= book_layout_page_count() || rect.width != s_rect.width || rect.height != s_rect.height ||
        x < rect.x || y < rect.y || x >= rect.x + rect.width || y >= rect.y + rect.height) return false;
    size_t off = s_pages[page], end = page + 1 < s_count ? s_pages[page + 1] : s_len;
    int64_t used = 0;
    while (off < end) {
        size_t next, text_end;
        bool paragraph_end, heading;
        int line_px, indent = 0;
        int64_t line_w = 0;
        if (!take_line(off, &next, &text_end, &paragraph_end, &line_px, &heading, &indent, &line_w)) return false;
        const blk_t* block = block_at(off);
        int height = row_height(block, line_px);
        if (used + height > rect.height) return false;
        int line_top = rect.y + (int)used;
        int line_bot = line_top + height;
        if (y >= line_top && y < line_bot) {
            if (block && (block->image || block->image_src)) return false;
            if (text_end <= off) return false;
            if (out_line_y) *out_line_y = line_top;
            int gx = rect.x + indent;
            size_t last_good = off;
            for (size_t p = off; p < text_end;) {
                size_t n = codepoint_size(s_text + p, text_end - p);
                if (!n) break;
                char g[5]; memcpy(g, s_text + p, n); g[n] = 0;
                int adv = ttf_text_width_px(line_px, g);
                last_good = p;
                if (x < gx + adv) {
                    *out_off = p;
                    return true;
                }
                gx += adv;
                p += n;
            }
            *out_off = last_good;
            return true;
        }
        used += height;
        if (paragraph_end) used += paragraph_extra(height, heading);
        off = next;
    }
    return false;
}

static size_t prev_cp(const char* utf8, size_t off) {
    if (!off) return 0;
    size_t p = off - 1;
    while (p > 0 && (((unsigned char)utf8[p] & 0xc0) == 0x80)) --p;
    return p;
}

static bool is_ws_byte(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool book_layout_sentence_bounds(const char* utf8, size_t len, size_t off,
                                 size_t clamp_start, size_t clamp_end,
                                 size_t* out_start, size_t* out_end) {
    if (!utf8 || !len || !out_start || !out_end) return false;
    if (clamp_end > len) clamp_end = len;
    if (clamp_start >= clamp_end) return false;
    if (off < clamp_start) off = clamp_start;
    if (off >= clamp_end) off = prev_cp(utf8, clamp_end);
    while (off > clamp_start && (((unsigned char)utf8[off] & 0xc0) == 0x80)) --off;

    // 向前寻找最近句末标点或换行。/ Scan backward to preceding sentence stop or newline.
    size_t s = off;
    while (s > clamp_start) {
        size_t p = prev_cp(utf8, s);
        size_t n = s - p;
        if (utf8[p] == '\n' || utf8[p] == '\r') break;
        if (is_sentence_stop(utf8 + p, n)) {
            // 如果紧跟着收引号，跳过收引号作为前一句的结尾。/ Skip closing quote belonging to previous sentence.
            size_t after = s;
            while (after < off) {
                size_t an = codepoint_size(utf8 + after, clamp_end - after);
                if (an && is_closing_quote_or_paren(utf8 + after, an)) after += an;
                else break;
            }
            s = after;
            break;
        }
        s = p;
    }
    while (s < clamp_end && is_ws_byte(utf8[s])) ++s;

    // 向后寻找当前句末标点或换行。/ Scan forward to end of current sentence or newline.
    size_t e = off;
    while (e < clamp_end) {
        if (utf8[e] == '\n' || utf8[e] == '\r') break;
        size_t n = codepoint_size(utf8 + e, clamp_end - e);
        if (!n) break;
        e += n;
        if (is_sentence_stop(utf8 + e - n, n)) {
            while (e < clamp_end) {
                size_t an = codepoint_size(utf8 + e, clamp_end - e);
                if (an && is_closing_quote_or_paren(utf8 + e, an)) e += an;
                else break;
            }
            break;
        }
    }
    while (e > s && is_ws_byte(utf8[e - 1])) --e;
    if (e <= s) return false;
    *out_start = s;
    *out_end = e;
    return true;
}

bool book_layout_expand_prev_sentence(const char* utf8, size_t len,
                                      size_t cur_start, size_t clamp_start,
                                      size_t* out_start) {
    if (!utf8 || !out_start || cur_start <= clamp_start || clamp_start >= len) return false;
    size_t probe = cur_start;
    while (probe > clamp_start) {
        size_t p = prev_cp(utf8, probe);
        if (!is_ws_byte(utf8[p])) {
            // 退过前一句末尾标点与收引号，进入前一句正文。
            // Step back across the preceding sentence's closing quote and stop into its body.
            size_t n = probe - p;
            if (is_closing_quote_or_paren(utf8 + p, n) || is_sentence_stop(utf8 + p, n)) {
                probe = p;
                continue;
            }
            size_t s = 0, e = 0;
            if (book_layout_sentence_bounds(utf8, len, p, clamp_start, cur_start, &s, &e) && s < cur_start) {
                *out_start = s;
                return true;
            }
            *out_start = p;
            return true;
        }
        probe = p;
    }
    return false;
}

bool book_layout_expand_next_sentence(const char* utf8, size_t len,
                                      size_t cur_end, size_t clamp_end,
                                      size_t* out_end) {
    if (!utf8 || !out_end) return false;
    if (clamp_end > len) clamp_end = len;
    if (cur_end >= clamp_end) return false;
    size_t probe = cur_end;
    while (probe < clamp_end && is_ws_byte(utf8[probe])) ++probe;
    if (probe >= clamp_end) return false;
    size_t s = 0, e = 0;
    if (book_layout_sentence_bounds(utf8, len, probe, cur_end, clamp_end, &s, &e) && e > cur_end) {
        *out_end = e;
        return true;
    }
    return false;
}

bool book_layout_paragraph_bounds(const char* utf8, size_t len, size_t off,
                                  size_t* out_start, size_t* out_end) {
    if (!utf8 || !len || !out_start || !out_end) return false;
    if (off >= len) off = prev_cp(utf8, len);
    while (off > 0 && (((unsigned char)utf8[off] & 0xc0) == 0x80)) --off;
    size_t s = off;
    while (s > 0 && utf8[s - 1] != '\n' && utf8[s - 1] != '\r') --s;
    size_t e = off;
    while (e < len && utf8[e] != '\n' && utf8[e] != '\r') ++e;
    while (s < e && is_ws_byte(utf8[s])) ++s;
    while (e > s && is_ws_byte(utf8[e - 1])) --e;
    if (e <= s) return false;
    *out_start = s;
    *out_end = e;
    return true;
}
