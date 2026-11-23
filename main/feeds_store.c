/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：OPDS 1.2 / RSS 2.0 / Atom 1.0 订阅源 NVS 持久化、XML 解析与离线缓存实现。
 * English: OPDS 1.2 / RSS 2.0 / Atom 1.0 feed persistence, XML parser, and offline cache.
 */
#include "feeds_store.h"
#include "xml_tok.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "nvs.h"
#endif

#define FEEDS_NVS_NS "rp_feeds"

/* ---- 内存分配辅助 / Memory allocation helpers ---- */

static void *feeds_alloc(size_t n) {
#ifdef ESP_PLATFORM
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return calloc(1, n);
}

static char *feeds_strdup_n(const char *s, size_t n) {
    char *p = feeds_alloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

/* ---- URL 校验、解析与 Basic Auth / URL validation, resolution, Basic Auth ---- */

bool feeds_url_valid(const char *url) {
    if (!url) return false;
    bool is_https = (strncmp(url, "https://", 8) == 0);
    bool is_http = (!is_https && strncmp(url, "http://", 7) == 0);
    if (!is_https && !is_http) return false;
    size_t len = strlen(url);
    if (len >= FEEDS_URL_MAX) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 32 || c == 127) return false;
    }
    const char *host = url + (is_https ? 8 : 7);
    const char *end = strpbrk(host, "/?#");
    size_t host_len = end ? (size_t)(end - host) : strlen(host);
    return host_len > 0;
}

bool feeds_resolve_url(const char *base_url, const char *href, char *out, size_t cap) {
    if (!href || !out || cap == 0) return false;
    while (*href == ' ' || *href == '\t' || *href == '\r' || *href == '\n') ++href;
    if (!*href) return false;
    if (strncmp(href, "http://", 7) == 0 || strncmp(href, "https://", 8) == 0) {
        size_t n = strlen(href);
        if (n >= cap) return false;
        memcpy(out, href, n + 1);
        return feeds_url_valid(out);
    }
    if (!base_url || !feeds_url_valid(base_url)) return false;
    const char *scheme_end = strstr(base_url, "://");
    if (!scheme_end) return false;
    size_t scheme_len = (size_t)(scheme_end - base_url);
    if (href[0] == '/' && href[1] == '/') {
        int r = snprintf(out, cap, "%.*s:%s", (int)scheme_len, base_url, href);
        return r > 0 && (size_t)r < cap && feeds_url_valid(out);
    }
    const char *auth_start = scheme_end + 3;
    const char *path_start = strchr(auth_start, '/');
    size_t origin_len = path_start ? (size_t)(path_start - base_url) : strlen(base_url);
    if (href[0] == '/') {
        int r = snprintf(out, cap, "%.*s%s", (int)origin_len, base_url, href);
        return r > 0 && (size_t)r < cap && feeds_url_valid(out);
    }
    if (href[0] == '?') {
        const char *q = strchr(base_url, '?');
        size_t prefix_len = q ? (size_t)(q - base_url) : strlen(base_url);
        int r = snprintf(out, cap, "%.*s%s", (int)prefix_len, base_url, href);
        return r > 0 && (size_t)r < cap && feeds_url_valid(out);
    }
    // 相对路径：取 base_url 最后一个 '/' 之前部分 / Relative path: take directory of base_url.
    const char *base_end = strpbrk(auth_start, "?#");
    size_t base_path_end = base_end ? (size_t)(base_end - base_url) : strlen(base_url);
    size_t dir_len = origin_len;
    if (path_start) {
        for (size_t i = base_path_end; i > origin_len; --i) {
            if (base_url[i - 1] == '/') {
                dir_len = i;
                break;
            }
        }
        int r = snprintf(out, cap, "%.*s%s", (int)dir_len, base_url, href);
        return r > 0 && (size_t)r < cap && feeds_url_valid(out);
    }
    int r = snprintf(out, cap, "%.*s/%s", (int)origin_len, base_url, href);
    return r > 0 && (size_t)r < cap && feeds_url_valid(out);
}

void feeds_source_redact(const feed_source_t *src, feed_public_source_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!src) return;
    snprintf(out->name, sizeof(out->name), "%s", src->name);
    snprintf(out->url, sizeof(out->url), "%s", src->url);
    out->kind = src->kind;
    snprintf(out->auth_user, sizeof(out->auth_user), "%s", src->auth_user);
    out->has_password = (src->auth_pass[0] != '\0');
}

bool feeds_build_basic_auth(const char *user, const char *pass, char *out, size_t cap) {
    if (!out || cap < 8) return false;
    out[0] = '\0';
    if ((!user || !user[0]) && (!pass || !pass[0])) return false;
    char raw[FEEDS_USER_MAX + FEEDS_PASS_MAX + 2];
    int n = snprintf(raw, sizeof(raw), "%s:%s", user ? user : "", pass ? pass : "");
    if (n <= 0 || (size_t)n >= sizeof(raw)) return false;
    static const char k_b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out_need = 6 + ((size_t)n + 2) / 3 * 4 + 1;
    if (out_need > cap) {
        memset(raw, 0, sizeof(raw));
        return false;
    }
    memcpy(out, "Basic ", 6);
    size_t w = 6;
    const unsigned char *in = (const unsigned char *)raw;
    for (size_t i = 0; i < (size_t)n; i += 3) {
        uint32_t v = ((uint32_t)in[i]) << 16;
        if (i + 1 < (size_t)n) v |= ((uint32_t)in[i + 1]) << 8;
        if (i + 2 < (size_t)n) v |= (uint32_t)in[i + 2];
        out[w++] = k_b64[(v >> 18) & 0x3Fu];
        out[w++] = k_b64[(v >> 12) & 0x3Fu];
        out[w++] = (i + 1 < (size_t)n) ? k_b64[(v >> 6) & 0x3Fu] : '=';
        out[w++] = (i + 2 < (size_t)n) ? k_b64[v & 0x3Fu] : '=';
    }
    out[w] = '\0';
    memset(raw, 0, sizeof(raw));
    return true;
}

/* ---- NVS 持久化（宿主机使用内存存储模拟）/ NVS persistence (in-memory on host) ---- */

#ifndef ESP_PLATFORM
static feed_source_t s_host_sources[FEEDS_MAX_SOURCES];
static size_t s_host_count = 0;
#endif

bool feeds_store_load(feed_source_t *out, size_t max_items, size_t *out_count) {
    if (out_count) *out_count = 0;
    if (!out || max_items == 0) return false;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    esp_err_t err = nvs_open(FEEDS_NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) return false;
    uint8_t count = 0;
    if (nvs_get_u8(h, "count", &count) != ESP_OK) count = 0;
    if (count > FEEDS_MAX_SOURCES) count = FEEDS_MAX_SOURCES;
    size_t got = 0;
    for (uint8_t i = 0; i < count && got < max_items; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "s%u", (unsigned)i);
        feed_source_t item = {0};
        size_t sz = sizeof(item);
        if (nvs_get_blob(h, key, &item, &sz) == ESP_OK && sz == sizeof(item) && feeds_url_valid(item.url)) {
            item.name[sizeof(item.name) - 1] = '\0';
            item.url[sizeof(item.url) - 1] = '\0';
            item.auth_user[sizeof(item.auth_user) - 1] = '\0';
            item.auth_pass[sizeof(item.auth_pass) - 1] = '\0';
            out[got++] = item;
        }
    }
    nvs_close(h);
    if (out_count) *out_count = got;
    return true;
#else
    size_t got = s_host_count < max_items ? s_host_count : max_items;
    if (got) memcpy(out, s_host_sources, got * sizeof(feed_source_t));
    if (out_count) *out_count = got;
    return true;
#endif
}

bool feeds_store_save(const feed_source_t *items, size_t count) {
    if (count > FEEDS_MAX_SOURCES) count = FEEDS_MAX_SOURCES;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open(FEEDS_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(h, "count", (uint8_t)count);
    for (size_t i = 0; err == ESP_OK && i < count; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "s%u", (unsigned)i);
        err = nvs_set_blob(h, key, &items[i], sizeof(feed_source_t));
    }
    for (size_t i = count; err == ESP_OK && i < FEEDS_MAX_SOURCES; ++i) {
        char key[8];
        snprintf(key, sizeof(key), "s%u", (unsigned)i);
        esp_err_t e = nvs_erase_key(h, key);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) err = e;
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK;
#else
    s_host_count = count;
    if (count) memcpy(s_host_sources, items, count * sizeof(feed_source_t));
    return true;
#endif
}

static void derive_source_name(const char *url, char out[FEEDS_NAME_MAX]) {
    const char *host = url + (strncmp(url, "https://", 8) == 0 ? 8 : 7);
    size_t n = strcspn(host, "/?#:");
    if (n >= FEEDS_NAME_MAX) n = FEEDS_NAME_MAX - 1;
    if (n == 0) {
        snprintf(out, FEEDS_NAME_MAX, "订阅源");
        return;
    }
    memcpy(out, host, n);
    out[n] = '\0';
}

bool feeds_store_upsert(const char *name, const char *url, feed_kind_t kind,
                        const char *auth_user, const char *auth_pass, bool clear_auth,
                        bool *out_full) {
    if (out_full) *out_full = false;
    if (!feeds_url_valid(url)) return false;
    if (kind != FEED_KIND_RSS && kind != FEED_KIND_OPDS) return false;

    // 避免在 HTTPD 或主任务栈上分配 ~8 KB 源表导致栈溢出。
    // Allocate the ~8 KB source table off-stack to prevent HTTPD / main-task stack overflow.
    feed_source_t *list = feeds_alloc(sizeof(*list) * FEEDS_MAX_SOURCES);
    if (!list) return false;
    size_t count = 0;
    (void)feeds_store_load(list, FEEDS_MAX_SOURCES, &count);

    size_t idx = count;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(list[i].url, url) == 0) {
            idx = i;
            break;
        }
    }
    if (idx == count) {
        if (count >= FEEDS_MAX_SOURCES) {
            if (out_full) *out_full = true;
            memset(list, 0, sizeof(*list) * FEEDS_MAX_SOURCES);
            free(list);
            return false;
        }
        memset(&list[idx], 0, sizeof(list[idx]));
        count++;
    }

    if (name && name[0]) {
        snprintf(list[idx].name, sizeof(list[idx].name), "%s", name);
    } else if (!list[idx].name[0]) {
        derive_source_name(url, list[idx].name);
    }
    snprintf(list[idx].url, sizeof(list[idx].url), "%s", url);
    list[idx].kind = (uint8_t)kind;

    if (clear_auth) {
        memset(list[idx].auth_user, 0, sizeof(list[idx].auth_user));
        memset(list[idx].auth_pass, 0, sizeof(list[idx].auth_pass));
    } else {
        if (auth_user) {
            snprintf(list[idx].auth_user, sizeof(list[idx].auth_user), "%s", auth_user);
        }
        if (auth_pass && auth_pass[0]) {
            snprintf(list[idx].auth_pass, sizeof(list[idx].auth_pass), "%s", auth_pass);
        }
    }
    bool ok = feeds_store_save(list, count);
    memset(list, 0, sizeof(*list) * FEEDS_MAX_SOURCES);
    free(list);
    return ok;
}

bool feeds_store_delete(const char *url, bool *out_found) {
    if (out_found) *out_found = false;
    if (!url || !url[0]) return false;
    feed_source_t *list = feeds_alloc(sizeof(*list) * FEEDS_MAX_SOURCES);
    if (!list) return false;
    size_t count = 0;
    if (!feeds_store_load(list, FEEDS_MAX_SOURCES, &count)) {
        memset(list, 0, sizeof(*list) * FEEDS_MAX_SOURCES);
        free(list);
        return false;
    }
    size_t found = count;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(list[i].url, url) == 0) {
            found = i;
            break;
        }
    }
    if (found == count) {
        memset(list, 0, sizeof(*list) * FEEDS_MAX_SOURCES);
        free(list);
        return true;
    }
    if (out_found) *out_found = true;
    for (size_t i = found + 1; i < count; ++i) {
        list[i - 1] = list[i];
    }
    count--;
    bool ok = feeds_store_save(list, count);
    memset(list, 0, sizeof(*list) * FEEDS_MAX_SOURCES);
    free(list);
    return ok;
}

size_t feeds_store_list_public(feed_public_source_t *out, size_t max_items) {
    if (!out || max_items == 0) return 0;
    feed_source_t *list = feeds_alloc(sizeof(*list) * FEEDS_MAX_SOURCES);
    if (!list) return 0;
    size_t count = 0;
    if (!feeds_store_load(list, FEEDS_MAX_SOURCES, &count)) {
        memset(list, 0, sizeof(*list) * FEEDS_MAX_SOURCES);
        free(list);
        return 0;
    }
    size_t n = count < max_items ? count : max_items;
    for (size_t i = 0; i < n; ++i) {
        feeds_source_redact(&list[i], &out[i]);
    }
    memset(list, 0, sizeof(*list) * FEEDS_MAX_SOURCES);
    free(list);
    return n;
}

/* ---- RSS 2.0 / Atom 1.0 / OPDS 1.2 XML 解析 / Feed XML parser ---- */

static int acq_ext_priority(const char *ext) {
    if (!ext || !ext[0]) return 0;
    if (strcasecmp(ext, "epub") == 0) return 4;
    if (strcasecmp(ext, "cbz") == 0) return 3;
    if (strcasecmp(ext, "txt") == 0) return 2;
    if (strcasecmp(ext, "md") == 0) return 1;
    return 0;
}

static bool detect_book_ext(const char *mime, const char *href, char out_ext[8]) {
    out_ext[0] = '\0';
    if (mime && mime[0]) {
        if (strstr(mime, "epub")) { strcpy(out_ext, "epub"); return true; }
        if (strstr(mime, "comicbook+zip") || strstr(mime, "x-cbz")) { strcpy(out_ext, "cbz"); return true; }
        if (strstr(mime, "text/markdown") || strstr(mime, "text/x-markdown")) { strcpy(out_ext, "md"); return true; }
        if (strstr(mime, "text/plain")) { strcpy(out_ext, "txt"); return true; }
    }
    if (href && href[0]) {
        const char *end = strpbrk(href, "?#");
        size_t len = end ? (size_t)(end - href) : strlen(href);
        if (len >= 5 && strncasecmp(href + len - 5, ".epub", 5) == 0) { strcpy(out_ext, "epub"); return true; }
        if (len >= 4 && strncasecmp(href + len - 4, ".cbz", 4) == 0) { strcpy(out_ext, "cbz"); return true; }
        if (len >= 4 && strncasecmp(href + len - 4, ".txt", 4) == 0) { strcpy(out_ext, "txt"); return true; }
        if (len >= 3 && strncasecmp(href + len - 3, ".md", 3) == 0) { strcpy(out_ext, "md"); return true; }
    }
    return false;
}

static void append_text_buf(char *dst, size_t cap, xml_span_t text, bool cdata, bool collapse_ws) {
    size_t cur = strlen(dst);
    if (cur + 1 >= cap) return;
    char tmp[512];
    size_t got = xml_decode_span(text, tmp, sizeof(tmp), collapse_ws, cdata);
    if (!got) return;
    if (collapse_ws && cur > 0 && dst[cur - 1] != ' ' && cur + 1 < cap) {
        dst[cur++] = ' ';
    }
    size_t avail = cap - 1 - cur;
    if (got > avail) got = avail;
    memcpy(dst + cur, tmp, got);
    dst[cur + got] = '\0';
}

static void append_heap_text(char **buf, size_t *len, size_t *cap, xml_span_t text, bool cdata) {
    if (!text.n) return;
    // 最大保留 128 KiB 单篇文章正文 / Cap single article body at 128 KiB.
    const size_t k_max = 128u * 1024u;
    if (*len >= k_max) return;
    size_t chunk_cap = text.n + 16;
    if (chunk_cap > k_max - *len + 1) chunk_cap = k_max - *len + 1;
    char *decoded = feeds_alloc(chunk_cap);
    if (!decoded) return;
    size_t got = xml_decode_span(text, decoded, chunk_cap, false, cdata);
    if (got > 0) {
        if (*len + got > k_max) got = k_max - *len;
        if (*len + got + 1 > *cap) {
            size_t next_cap = (*cap) ? (*cap) * 2 : 1024;
            while (next_cap < *len + got + 1) next_cap *= 2;
            if (next_cap > k_max + 1) next_cap = k_max + 1;
            char *next = feeds_alloc(next_cap);
            if (!next) {
                free(decoded);
                return;
            }
            if (*buf && *len) memcpy(next, *buf, *len);
            free(*buf);
            *buf = next;
            *cap = next_cap;
        }
        memcpy(*buf + *len, decoded, got);
        *len += got;
        (*buf)[*len] = '\0';
    }
    free(decoded);
}

void feeds_parsed_free(feed_parsed_t *parsed) {
    if (!parsed) return;
    if (parsed->items) {
        for (size_t i = 0; i < parsed->count; ++i) {
            free(parsed->items[i].content_html);
        }
        free(parsed->items);
    }
    memset(parsed, 0, sizeof(*parsed));
}

bool feeds_parse_xml(const char *xml, size_t len, const char *base_url,
                     feed_parsed_t *out, size_t max_items) {
    if (!xml || !len || !out) return false;
    memset(out, 0, sizeof(*out));
    if (max_items == 0 || max_items > FEEDS_MAX_ITEMS) max_items = FEEDS_MAX_ITEMS;

    out->items = feeds_alloc(max_items * sizeof(feed_item_t));
    if (!out->items) return false;

    xml_reader_t r;
    xml_reader_init(&r, xml, len, true);

    bool in_entry = false;
    size_t entry_depth = 0;
    feed_item_t cur = {0};
    char *cur_content = NULL;
    size_t cur_content_len = 0;
    size_t cur_content_cap = 0;
    bool cur_has_encoded = false;

    enum {
        FIELD_NONE = 0,
        FIELD_FEED_TITLE,
        FIELD_ITEM_TITLE,
        FIELD_ITEM_LINK,
        FIELD_ITEM_AUTHOR,
        FIELD_ITEM_SUMMARY,
        FIELD_ITEM_CONTENT,
    } field = FIELD_NONE;
    size_t field_depth = 0;
    bool in_author = false;
    size_t author_depth = 0;

    xml_tok_t tok;
    while (xml_reader_next(&r, &tok)) {
        if (tok.kind == XML_TOK_OPEN) {
            xml_span_t local = xml_local_name(tok.name);
            if (out->doc_kind == FEED_DOC_UNKNOWN && tok.depth == 1) {
                if (xml_span_ieq(local, "rss") || xml_span_ieq(local, "rdf")) {
                    out->doc_kind = FEED_DOC_RSS;
                } else if (xml_span_ieq(local, "feed")) {
                    out->doc_kind = FEED_DOC_ATOM;
                }
            }

            if (!in_entry) {
                if ((out->doc_kind == FEED_DOC_RSS && xml_span_ieq(local, "item")) ||
                    ((out->doc_kind == FEED_DOC_ATOM || out->doc_kind == FEED_DOC_OPDS) && xml_span_ieq(local, "entry"))) {
                    if (!tok.empty) {
                        in_entry = true;
                        entry_depth = tok.depth;
                        memset(&cur, 0, sizeof(cur));
                        cur_content = NULL;
                        cur_content_len = 0;
                        cur_content_cap = 0;
                        cur_has_encoded = false;
                        in_author = false;
                    }
                } else if (xml_span_ieq(local, "title") && !out->title[0] && !tok.empty) {
                    field = FIELD_FEED_TITLE;
                    field_depth = tok.depth;
                } else if (xml_span_ieq(local, "link")) {
                    char rel[64] = {0}, href[FEEDS_URL_MAX] = {0}, type[128] = {0};
                    (void)xml_tok_attr(&tok, "rel", rel, sizeof(rel), true);
                    (void)xml_tok_attr(&tok, "href", href, sizeof(href), true);
                    (void)xml_tok_attr(&tok, "type", type, sizeof(type), true);
                    if (strstr(type, "opds-catalog")) {
                        out->doc_kind = FEED_DOC_OPDS;
                    }
                    if (href[0]) {
                        char resolved[FEEDS_URL_MAX];
                        if (feeds_resolve_url(base_url, href, resolved, sizeof(resolved))) {
                            if (strcasecmp(rel, "next") == 0 && !out->next_url[0]) {
                                snprintf(out->next_url, sizeof(out->next_url), "%s", resolved);
                            } else if (strcasecmp(rel, "search") == 0 && !out->search_url[0]) {
                                snprintf(out->search_url, sizeof(out->search_url), "%s", resolved);
                            }
                        }
                    }
                }
            } else {
                // 条目内部标签 / Tags inside <item> or <entry>.
                if (xml_span_ieq(local, "title") && !tok.empty && !cur.title[0]) {
                    field = FIELD_ITEM_TITLE;
                    field_depth = tok.depth;
                } else if (xml_span_ieq(local, "author")) {
                    if (out->doc_kind == FEED_DOC_RSS && !tok.empty && !cur.author[0]) {
                        field = FIELD_ITEM_AUTHOR;
                        field_depth = tok.depth;
                    } else if (!tok.empty) {
                        in_author = true;
                        author_depth = tok.depth;
                    }
                } else if (in_author && xml_span_ieq(local, "name") && !tok.empty && !cur.author[0]) {
                    field = FIELD_ITEM_AUTHOR;
                    field_depth = tok.depth;
                } else if (xml_span_ieq(local, "creator") && !tok.empty && !cur.author[0]) {
                    field = FIELD_ITEM_AUTHOR;
                    field_depth = tok.depth;
                } else if (xml_span_ieq(local, "summary") && !tok.empty) {
                    field = FIELD_ITEM_SUMMARY;
                    field_depth = tok.depth;
                } else if (xml_span_ieq(local, "description") && !tok.empty) {
                    field = FIELD_ITEM_SUMMARY;
                    field_depth = tok.depth;
                } else if ((xml_span_ieq(local, "encoded") || xml_span_ieq(local, "content")) && !tok.empty) {
                    // content:encoded 或 Atom <content> 优先作为正文 / Prefer full content over summary.
                    if (xml_span_ieq(local, "encoded") || !cur_has_encoded) {
                        free(cur_content);
                        cur_content = NULL;
                        cur_content_len = 0;
                        cur_content_cap = 0;
                        cur_has_encoded = true;
                        field = FIELD_ITEM_CONTENT;
                        field_depth = tok.depth;
                    }
                } else if (xml_span_ieq(local, "link")) {
                    char href[FEEDS_URL_MAX] = {0}, rel[128] = {0}, type[128] = {0};
                    (void)xml_tok_attr(&tok, "href", href, sizeof(href), true);
                    (void)xml_tok_attr(&tok, "rel", rel, sizeof(rel), true);
                    (void)xml_tok_attr(&tok, "type", type, sizeof(type), true);
                    if (href[0]) {
                        char resolved[FEEDS_URL_MAX];
                        if (feeds_resolve_url(base_url, href, resolved, sizeof(resolved))) {
                            bool is_acq = (strncmp(rel, "http://opds-spec.org/acquisition", 32) == 0);
                            bool is_opds_nav = (strstr(type, "application/atom+xml") != NULL ||
                                                strstr(type, "opds-catalog") != NULL);
                            char ext[8] = {0};
                            bool has_book = detect_book_ext(type, resolved, ext);
                            if (is_acq || (out->doc_kind == FEED_DOC_OPDS && has_book) ||
                                (has_book && strcasecmp(rel, "alternate") != 0 && !is_opds_nav)) {
                                if (is_acq) out->doc_kind = FEED_DOC_OPDS;
                                if (has_book && acq_ext_priority(ext) > acq_ext_priority(cur.acq_ext)) {
                                    snprintf(cur.acq_url, sizeof(cur.acq_url), "%s", resolved);
                                    snprintf(cur.acq_ext, sizeof(cur.acq_ext), "%s", ext);
                                }
                            } else if (is_opds_nav && strcasecmp(rel, "self") != 0) {
                                out->doc_kind = FEED_DOC_OPDS;
                                if (!cur.link[0]) {
                                    snprintf(cur.link, sizeof(cur.link), "%s", resolved);
                                    cur.is_nav = true;
                                }
                            } else if ((!rel[0] || strcasecmp(rel, "alternate") == 0) && !cur.link[0]) {
                                snprintf(cur.link, sizeof(cur.link), "%s", resolved);
                            }
                        }
                    } else if (!tok.empty && !cur.link[0]) {
                        // RSS 2.0 <link>https://...</link>
                        field = FIELD_ITEM_LINK;
                        field_depth = tok.depth;
                    }
                }
            }
        } else if (tok.kind == XML_TOK_TEXT) {
            if (field == FIELD_FEED_TITLE) {
                append_text_buf(out->title, sizeof(out->title), tok.text, tok.cdata, true);
            } else if (field == FIELD_ITEM_TITLE) {
                append_text_buf(cur.title, sizeof(cur.title), tok.text, tok.cdata, true);
            } else if (field == FIELD_ITEM_AUTHOR) {
                append_text_buf(cur.author, sizeof(cur.author), tok.text, tok.cdata, true);
            } else if (field == FIELD_ITEM_LINK) {
                char tmp[FEEDS_URL_MAX] = {0};
                xml_decode_span(tok.text, tmp, sizeof(tmp), true, tok.cdata);
                if (tmp[0] && !cur.link[0]) {
                    (void)feeds_resolve_url(base_url, tmp, cur.link, sizeof(cur.link));
                }
            } else if (field == FIELD_ITEM_SUMMARY) {
                append_text_buf(cur.summary, sizeof(cur.summary), tok.text, tok.cdata, true);
                if (!cur_has_encoded) {
                    append_heap_text(&cur_content, &cur_content_len, &cur_content_cap, tok.text, tok.cdata);
                }
            } else if (field == FIELD_ITEM_CONTENT) {
                append_heap_text(&cur_content, &cur_content_len, &cur_content_cap, tok.text, tok.cdata);
            }
        } else if (tok.kind == XML_TOK_CLOSE) {
            if (field != FIELD_NONE && tok.depth <= field_depth) {
                field = FIELD_NONE;
                field_depth = 0;
            }
            if (in_author && tok.depth <= author_depth) {
                in_author = false;
                author_depth = 0;
            }
            if (in_entry && tok.depth <= entry_depth) {
                in_entry = false;
                entry_depth = 0;
                if (cur.acq_url[0]) cur.is_nav = false;
                if (out->count < max_items && (cur.title[0] || cur.link[0] || cur.acq_url[0])) {
                    if (!cur.title[0]) snprintf(cur.title, sizeof(cur.title), "未命名条目");
                    cur.content_html = cur_content;
                    cur.content_len = cur_content_len;
                    cur_content = NULL;
                    if (!cur.content_html && cur.summary[0]) {
                        cur.content_html = feeds_strdup_n(cur.summary, strlen(cur.summary));
                        cur.content_len = cur.content_html ? strlen(cur.content_html) : 0;
                    }
                    out->items[out->count++] = cur;
                } else {
                    free(cur_content);
                    cur_content = NULL;
                }
                memset(&cur, 0, sizeof(cur));
                cur_content_len = 0;
                cur_content_cap = 0;
            }
        }
    }

    free(cur_content);
    if (out->doc_kind == FEED_DOC_UNKNOWN) {
        feeds_parsed_free(out);
        return false;
    }
    return true;
}

/* ---- TF 卡离线缓存 / TF-card offline cache ---- */

static uint32_t fnv1a_32(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        h ^= (unsigned char)*s;
        h *= 16777619u;
    }
    return h;
}

bool feeds_cache_path(const char *sd_root, const char *url, char *out, size_t cap) {
    if (!sd_root || !sd_root[0] || !url || !url[0] || !out || cap == 0) return false;
    uint32_t h = fnv1a_32(url);
    int r = snprintf(out, cap, "%s/feeds/%08lx.xml", sd_root, (unsigned long)h);
    return r > 0 && (size_t)r < cap;
}

bool feeds_cache_write(const char *sd_root, const char *url, const char *data, size_t len) {
    if (!sd_root || !sd_root[0] || !url || !data || !len || len > FEEDS_CACHE_MAX_BYTES) return false;
    char dir[320];
    if (snprintf(dir, sizeof(dir), "%s/feeds", sd_root) >= (int)sizeof(dir)) return false;
    (void)mkdir(dir, 0755);
    char path[360];
    if (!feeds_cache_path(sd_root, url, path, sizeof(path))) return false;
    FILE *fp = fopen(path, "wb");
    if (!fp) return false;
    bool ok = (fwrite(data, 1, len, fp) == len);
    fclose(fp);
    return ok;
}

bool feeds_cache_read(const char *sd_root, const char *url, char **out_data, size_t *out_len) {
    if (out_data) *out_data = NULL;
    if (out_len) *out_len = 0;
    if (!sd_root || !sd_root[0] || !url || !out_data || !out_len) return false;
    char path[360];
    if (!feeds_cache_path(sd_root, url, path, sizeof(path))) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long sz = ftell(fp);
    if (sz <= 0 || (size_t)sz > FEEDS_CACHE_MAX_BYTES || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }
    char *buf = feeds_alloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return false; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) {
        free(buf);
        return false;
    }
    buf[got] = '\0';
    *out_data = buf;
    *out_len = got;
    return true;
}
