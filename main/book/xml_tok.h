/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：轻量流式 XML 分词器与实体解码器，供 EPUB、OPDS 1.2 (Atom) 与 RSS 2.0 复用。
 * English: Lightweight streaming XML tokenizer and entity decoder shared by EPUB, OPDS 1.2 (Atom) and RSS 2.0.
 *
 * 冻结：绝不加载外部 DTD，不展开自定义内部实体，严格限制嵌套深度以防恶意输入。
 * Frozen: Never load external DTDs or expand custom internal entities; strictly bound nesting depth.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XML_TOK_MAX_DEPTH 32

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *p; ///< 切片起址 / Slice start pointer
    size_t n;      ///< 切片字节数 / Slice length in bytes
} xml_span_t;

typedef enum {
    XML_TOK_OPEN = 0, ///< 开标签或自闭合标签 / Opening or self-closing tag
    XML_TOK_CLOSE,    ///< 闭标签 / Closing tag
    XML_TOK_TEXT,     ///< 文本或 CDATA 节点 / Text or CDATA node
} xml_kind_t;

typedef struct {
    xml_kind_t kind; ///< 节点类型 / Token kind
    xml_span_t name; ///< 标签原名（含可选命名空间前缀）/ Raw tag name (including optional namespace prefix)
    xml_span_t attrs;///< 原始属性片段 / Raw attribute span
    xml_span_t text; ///< 文本或 CDATA 载荷 / Text or CDATA payload
    size_t depth;    ///< 节点深度（根节点为 1）/ Node depth (1 for root)
    bool empty;      ///< 自闭合标签标志 / Self-closing tag flag
    bool cdata;      ///< CDATA 文本标志 / CDATA text flag
} xml_tok_t;

typedef struct {
    const char *p;
    const char *end;
    xml_span_t stack[XML_TOK_MAX_DEPTH];
    size_t depth;
    size_t roots;
    bool failed;
    bool lenient;
} xml_reader_t;

/// 初始化流式 XML 读取器；lenient=true 时容忍真实 RSS/OPDS 中的轻微非规范标记。
/// Initialize streaming XML reader; lenient=true tolerates minor real-world RSS/OPDS quirks.
void xml_reader_init(xml_reader_t *r, const char *text, size_t n, bool lenient);

/// 读取下一个 XML 词素；到达末尾或失败返回 false（可通过 r->failed 区分）。
/// Read next XML token; returns false at EOF or on error (check r->failed).
bool xml_reader_next(xml_reader_t *r, xml_tok_t *out);

/// 精确比较切片与字面量。/ Exact comparison between span and C string.
bool xml_span_eq(xml_span_t s, const char *lit);

/// 忽略 ASCII 大小写比较切片与字面量。/ Case-insensitive ASCII comparison between span and C string.
bool xml_span_ieq(xml_span_t s, const char *lit);

/// 剥离命名空间前缀，返回本地名（如 content:encoded -> encoded）。
/// Strip XML namespace prefix and return the local name (e.g. content:encoded -> encoded).
xml_span_t xml_local_name(xml_span_t s);

/// 迭代解析标签属性；遇到语法错在严格模式下将 *offset 设为 SIZE_MAX。
/// Iterate tag attributes; sets *offset = SIZE_MAX on syntax error in strict mode.
bool xml_attr_next(xml_span_t attrs, size_t *offset, xml_span_t *name, xml_span_t *value, bool lenient);

/// 按本地名提取并解码单个属性值；未找到时 out 置空并返回 true（严格模式错误返回 false）。
/// Extract and decode an attribute by local name; sets out empty if absent.
bool xml_tok_attr(const xml_tok_t *t, const char *wanted, char *out, size_t cap, bool lenient);

/// 解码 XML/HTML 常见实体到 UTF-8 缓冲区；cdata=true 时原样复制。
/// Decode XML and common HTML entities to UTF-8; copies verbatim when cdata=true.
size_t xml_decode_span(xml_span_t in, char *out, size_t cap, bool collapse_ws, bool cdata);

#ifdef __cplusplus
}
#endif
