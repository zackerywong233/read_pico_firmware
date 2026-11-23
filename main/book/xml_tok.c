/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：轻量流式 XML 分词器与实体解码器实现。
 * English: Lightweight streaming XML tokenizer and entity decoder implementation.
 */
#include "xml_tok.h"

#include <ctype.h>
#include <string.h>

static bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool is_name_char(char c) {
    unsigned char u = (unsigned char)c;
    return isalnum(u) || c == ':' || c == '_' || c == '-' || c == '.' || u >= 0x80;
}

static bool starts_with(const char *p, const char *end, const char *lit) {
    size_t n = strlen(lit);
    return (size_t)(end - p) >= n && memcmp(p, lit, n) == 0;
}

static const char *find_marker(const char *p, const char *end, const char *marker) {
    size_t n = strlen(marker);
    if (!n || (size_t)(end - p) < n) return NULL;
    const char *limit = end - n;
    for (; p <= limit; ++p) {
        if (*p == marker[0] && memcmp(p, marker, n) == 0) return p;
    }
    return NULL;
}

static size_t encode_utf8(uint32_t cp, char *out, size_t avail) {
    if (cp == 0 || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu;
    if (cp < 0x80u) {
        if (avail < 1) return 0;
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        if (avail < 2) return 0;
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        if (avail < 3) return 0;
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (avail < 4) return 0;
    out[0] = (char)(0xF0u | (cp >> 18));
    out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

static bool decode_entity(const char *ent, size_t n, uint32_t *cp) {
    if (!n) return false;
    if (ent[0] == '#') {
        uint32_t v = 0;
        if (n >= 2 && (ent[1] == 'x' || ent[1] == 'X')) {
            if (n == 2) return false;
            for (size_t i = 2; i < n; ++i) {
                char c = ent[i];
                uint32_t d;
                if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
                else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
                else return false;
                if (v > 0x10FFFFu) return false;
                v = (v << 4) | d;
            }
        } else {
            if (n == 1) return false;
            for (size_t i = 1; i < n; ++i) {
                if (ent[i] < '0' || ent[i] > '9') return false;
                if (v > 0x10FFFFu) return false;
                v = v * 10u + (uint32_t)(ent[i] - '0');
            }
        }
        *cp = v;
        return true;
    }
    static const struct { const char *name; uint32_t cp; } k_map[] = {
        {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''},
        {"nbsp", ' '}, {"mdash", 0x2014u}, {"ndash", 0x2013u}, {"hellip", 0x2026u},
        {"lsquo", 0x2018u}, {"rsquo", 0x2019u}, {"ldquo", 0x201Cu}, {"rdquo", 0x201Du},
        {"copy", 0x00A9u}, {"middot", 0x00B7u}, {"bull", 0x2022u},
    };
    for (size_t i = 0; i < sizeof(k_map) / sizeof(k_map[0]); ++i) {
        if (strlen(k_map[i].name) == n && memcmp(ent, k_map[i].name, n) == 0) {
            *cp = k_map[i].cp;
            return true;
        }
    }
    return false;
}

bool xml_span_eq(xml_span_t s, const char *lit) {
    size_t n = strlen(lit);
    return s.n == n && (n == 0 || memcmp(s.p, lit, n) == 0);
}

bool xml_span_ieq(xml_span_t s, const char *lit) {
    size_t n = strlen(lit);
    if (s.n != n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (tolower((unsigned char)s.p[i]) != tolower((unsigned char)lit[i])) return false;
    }
    return true;
}

xml_span_t xml_local_name(xml_span_t s) {
    for (size_t i = 0; i < s.n; ++i) {
        if (s.p[i] == ':') {
            return (xml_span_t){s.p + i + 1, s.n - i - 1};
        }
    }
    return s;
}

size_t xml_decode_span(xml_span_t in, char *out, size_t cap, bool collapse_ws, bool cdata) {
    if (!out || cap == 0) return 0;
    size_t w = 0;
    bool prev_ws = collapse_ws;
    for (size_t i = 0; i < in.n && w + 1 < cap;) {
        if (!cdata && in.p[i] == '&') {
            size_t semi = i + 1;
            while (semi < in.n && semi - i <= 12 && in.p[semi] != ';' && in.p[semi] != '&' && !is_ws(in.p[semi])) {
                ++semi;
            }
            uint32_t cp = 0;
            if (semi < in.n && in.p[semi] == ';' && decode_entity(in.p + i + 1, semi - i - 1, &cp)) {
                if (collapse_ws && (cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n')) {
                    if (!prev_ws && w + 1 < cap) {
                        out[w++] = ' ';
                        prev_ws = true;
                    }
                } else {
                    char utf8[4];
                    size_t ulen = encode_utf8(cp, utf8, sizeof(utf8));
                    if (w + ulen < cap) {
                        memcpy(out + w, utf8, ulen);
                        w += ulen;
                        prev_ws = false;
                    } else {
                        break;
                    }
                }
                i = semi + 1;
                continue;
            }
        }
        char c = in.p[i++];
        if (collapse_ws && is_ws(c)) {
            if (!prev_ws && w + 1 < cap) {
                out[w++] = ' ';
                prev_ws = true;
            }
            continue;
        }
        out[w++] = c;
        prev_ws = false;
    }
    if (collapse_ws) {
        while (w > 0 && out[w - 1] == ' ') --w;
    }
    out[w] = '\0';
    return w;
}

bool xml_attr_next(xml_span_t attrs, size_t *offset, xml_span_t *name, xml_span_t *value, bool lenient) {
    if (!offset || *offset == SIZE_MAX) return false;
    size_t i = *offset;
    while (i < attrs.n && is_ws(attrs.p[i])) ++i;
    if (i >= attrs.n) {
        *offset = attrs.n;
        return false;
    }
    size_t start = i;
    while (i < attrs.n && is_name_char(attrs.p[i])) ++i;
    if (i == start) {
        *offset = lenient ? attrs.n : SIZE_MAX;
        return false;
    }
    *name = (xml_span_t){attrs.p + start, i - start};
    while (i < attrs.n && is_ws(attrs.p[i])) ++i;
    if (i >= attrs.n || attrs.p[i] != '=') {
        if (!lenient) {
            *offset = SIZE_MAX;
            return false;
        }
        *value = (xml_span_t){attrs.p + i, 0};
        *offset = i;
        return true;
    }
    ++i;
    while (i < attrs.n && is_ws(attrs.p[i])) ++i;
    if (i >= attrs.n) {
        *offset = lenient ? attrs.n : SIZE_MAX;
        return false;
    }
    if (attrs.p[i] == '\'' || attrs.p[i] == '"') {
        char quote = attrs.p[i++];
        start = i;
        while (i < attrs.n && attrs.p[i] != quote) {
            if (!lenient && attrs.p[i] == '<') {
                *offset = SIZE_MAX;
                return false;
            }
            ++i;
        }
        if (i >= attrs.n) {
            *offset = lenient ? attrs.n : SIZE_MAX;
            return false;
        }
        *value = (xml_span_t){attrs.p + start, i - start};
        *offset = i + 1;
        return true;
    }
    if (!lenient) {
        *offset = SIZE_MAX;
        return false;
    }
    start = i;
    while (i < attrs.n && !is_ws(attrs.p[i])) ++i;
    *value = (xml_span_t){attrs.p + start, i - start};
    *offset = i;
    return true;
}

bool xml_tok_attr(const xml_tok_t *t, const char *wanted, char *out, size_t cap, bool lenient) {
    if (!t || !wanted || !out || cap == 0) return false;
    out[0] = '\0';
    size_t off = 0;
    xml_span_t an, av;
    bool found = false;
    while (xml_attr_next(t->attrs, &off, &an, &av, lenient)) {
        xml_span_t local = xml_local_name(an);
        bool match = lenient ? xml_span_ieq(local, wanted) : xml_span_eq(an, wanted);
        if (match) {
            if (!lenient && found) return false;
            xml_decode_span(av, out, cap, false, false);
            found = true;
            if (lenient) return true;
        }
    }
    return off != SIZE_MAX;
}

void xml_reader_init(xml_reader_t *r, const char *text, size_t n, bool lenient) {
    memset(r, 0, sizeof(*r));
    r->p = text;
    r->end = text + n;
    r->lenient = lenient;
    if (!text) {
        r->failed = true;
        return;
    }
    if (!lenient && memchr(text, 0, n) != NULL) {
        r->failed = true;
        return;
    }
    if (n >= 3 && memcmp(text, "\xef\xbb\xbf", 3) == 0) {
        r->p += 3;
    }
}

bool xml_reader_next(xml_reader_t *r, xml_tok_t *t) {
    memset(t, 0, sizeof(*t));
    while (r->p < r->end && !r->failed) {
        const char *p = r->p;
        if (*p != '<') {
            const char *start = p;
            while (p < r->end && *p != '<') ++p;
            if (!r->depth && !r->lenient) {
                const char *q = start;
                while (q < p && is_ws(*q)) ++q;
                if (q != p) break;
            }
            t->kind = XML_TOK_TEXT;
            t->text = (xml_span_t){start, (size_t)(p - start)};
            t->depth = r->depth;
            r->p = p;
            return true;
        }
        if (starts_with(p, r->end, "<!--") || starts_with(p, r->end, "<?")) {
            bool comment = (p[1] == '!');
            const char *marker = comment ? "-->" : "?>";
            const char *end = find_marker(p + (comment ? 4 : 2), r->end, marker);
            if (!end) break;
            r->p = end + strlen(marker);
            continue;
        }
        if (starts_with(p, r->end, "<![CDATA[")) {
            if (!r->depth && !r->lenient) break;
            const char *end = find_marker(p + 9, r->end, "]]>");
            if (!end) break;
            t->kind = XML_TOK_TEXT;
            t->cdata = true;
            t->text = (xml_span_t){p + 9, (size_t)(end - (p + 9))};
            t->depth = r->depth;
            r->p = end + 3;
            return true;
        }
        if (starts_with(p, r->end, "<!DOCTYPE") || starts_with(p, r->end, "<!doctype")) {
            // 跳过声明但不加载外部 DTD。/ Skip declarations without loading external DTDs.
            int brackets = 0;
            char quote = 0;
            p += 9;
            while (p < r->end) {
                char c = *p++;
                if (quote) {
                    if (c == quote) quote = 0;
                    continue;
                }
                if (c == '\'' || c == '"') quote = c;
                else if (c == '[') ++brackets;
                else if (c == ']') {
                    if (!brackets) break;
                    --brackets;
                } else if (c == '>' && !brackets) {
                    r->p = p;
                    break;
                }
            }
            if (r->p != p || quote || brackets) break;
            continue;
        }
        ++p;
        bool closing = (p < r->end && *p == '/');
        if (closing) ++p;
        const char *name = p;
        while (p < r->end && is_name_char(*p)) ++p;
        if (p == name) {
            if (r->lenient) {
                // 跳过非法 '<' 字符作为文本推进 / Skip stray '<' in lenient mode.
                r->p = name;
                continue;
            }
            break;
        }
        t->name = (xml_span_t){name, (size_t)(p - name)};
        const char *attrs = p;
        char quote = 0;
        while (p < r->end) {
            if (quote) {
                if (*p == quote) quote = 0;
            } else if (*p == '\'' || *p == '"') {
                quote = *p;
            } else if (*p == '>') {
                break;
            }
            ++p;
        }
        if (p == r->end || quote) break;
        const char *end = p;
        while (end > attrs && is_ws(end[-1])) --end;
        if (end > attrs && end[-1] == '/') {
            t->empty = true;
            --end;
        }
        t->attrs = (xml_span_t){attrs, (size_t)(end - attrs)};
        if (!r->lenient) {
            size_t at = 0;
            xml_span_t an, av;
            while (xml_attr_next(t->attrs, &at, &an, &av, false)) {}
            if (at == SIZE_MAX) break;
        }
        if (closing) {
            if (!r->lenient) {
                if (t->empty || !r->depth) break;
                for (size_t i = 0; i < t->attrs.n; ++i) {
                    if (!is_ws(t->attrs.p[i])) {
                        r->failed = true;
                        return false;
                    }
                }
                xml_span_t top = r->stack[r->depth - 1];
                if (top.n != t->name.n || memcmp(top.p, t->name.p, top.n) != 0) break;
                t->kind = XML_TOK_CLOSE;
                t->depth = r->depth;
                --r->depth;
            } else {
                // 宽松模式：向上回溯匹配最近同名标签 / Lenient mode: unwind to nearest matching open tag.
                size_t match = r->depth;
                while (match > 0) {
                    xml_span_t s = r->stack[match - 1];
                    if (s.n == t->name.n && memcmp(s.p, t->name.p, s.n) == 0) break;
                    --match;
                }
                if (match == 0) {
                    r->p = p + 1;
                    continue;
                }
                t->kind = XML_TOK_CLOSE;
                t->depth = match;
                r->depth = match - 1;
            }
        } else {
            if (r->depth >= XML_TOK_MAX_DEPTH) {
                if (r->lenient) {
                    r->p = p + 1;
                    continue;
                }
                break;
            }
            if (!r->depth && ++r->roots != 1 && !r->lenient) break;
            t->kind = XML_TOK_OPEN;
            t->depth = r->depth + 1;
            if (!t->empty) r->stack[r->depth++] = t->name;
        }
        r->p = p + 1;
        return true;
    }
    if (!r->lenient && (r->p != r->end || r->depth || r->roots != 1)) {
        r->failed = true;
    }
    return false;
}
