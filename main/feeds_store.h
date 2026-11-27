/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：OPDS 1.2 / RSS 2.0 / Atom 1.0 订阅源独立 NVS 持久化（命名空间 rp_feeds）、
 * 凭据脱敏、URL 解析、XML 列表解析与 TF 卡离线缓存。
 * English: Dedicated NVS persistence (namespace rp_feeds), credential redaction,
 * URL resolution, RSS/Atom/OPDS XML parsing, and TF-card offline cache for feeds.
 *
 * 冻结：订阅源认证密码绝不经任何公开 HTTP/BLE 状态接口回显。
 * Frozen: Feed authentication passwords must never be echoed over public HTTP/BLE APIs.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FEEDS_MAX_SOURCES 16
#define FEEDS_NAME_MAX 64
#define FEEDS_URL_MAX 256
#define FEEDS_USER_MAX 64
#define FEEDS_PASS_MAX 128
#define FEEDS_MAX_ITEMS 64
#define FEEDS_CACHE_MAX_BYTES (512u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FEED_KIND_RSS = 0,  ///< RSS 2.0 / Atom 1.0 资讯订阅 / RSS or Atom news feed
    FEED_KIND_OPDS = 1, ///< OPDS 1.2 在线书库 / OPDS 1.2 book catalog
} feed_kind_t;

typedef struct {
    char name[FEEDS_NAME_MAX];      ///< 订阅名称 / Display name
    char url[FEEDS_URL_MAX];        ///< HTTP(S) 地址 / HTTP(S) URL
    uint8_t kind;                   ///< feed_kind_t
    char auth_user[FEEDS_USER_MAX]; ///< 可选 Basic Auth 用户名 / Optional Basic Auth username
    char auth_pass[FEEDS_PASS_MAX]; ///< 仅本机使用，绝不回显 / Local use only, never echoed
} feed_source_t;

typedef struct {
    char name[FEEDS_NAME_MAX];      ///< 订阅名称 / Display name
    char url[FEEDS_URL_MAX];        ///< HTTP(S) 地址 / HTTP(S) URL
    uint8_t kind;                   ///< feed_kind_t
    char auth_user[FEEDS_USER_MAX]; ///< 用户名 / Username
    bool has_password;              ///< 是否已保存密码（不回显明文）/ Whether a password is saved (never plaintext)
} feed_public_source_t;

typedef enum {
    FEED_DOC_UNKNOWN = 0,
    FEED_DOC_RSS,
    FEED_DOC_ATOM,
    FEED_DOC_OPDS,
} feed_doc_kind_t;

typedef struct {
    char title[160];      ///< 条目标题 / Entry title
    char author[64];      ///< 作者 / Author
    char summary[256];    ///< 摘要 / Short summary
    char link[FEEDS_URL_MAX];    ///< 原文或子目录链接 / Article or sub-catalog link
    char acq_url[FEEDS_URL_MAX]; ///< OPDS 图书下载链接 / OPDS acquisition download URL
    char acq_ext[8];      ///< 下载格式扩展名（epub/cbz/txt/md）/ Acquisition extension
    char *content_html;   ///< 堆分配的正文 HTML 或纯文本 / Heap-allocated article body
    size_t content_len;   ///< 正文字节数 / Article body length
    bool is_nav;          ///< 是否为 OPDS 导航子目录 / True for OPDS navigation entry
} feed_item_t;

typedef struct {
    feed_doc_kind_t doc_kind;       ///< 文档格式 / Detected feed format
    char title[128];                ///< 频道或目录标题 / Feed or catalog title
    char next_url[FEEDS_URL_MAX];   ///< 分页下一页链接（rel="next"）/ Pagination next URL
    char search_url[FEEDS_URL_MAX]; ///< 搜索链接（rel="search"）/ Search URL if present
    feed_item_t *items;             ///< 条目数组 / Parsed entries
    size_t count;                   ///< 条目数量 / Number of entries
} feed_parsed_t;

/// 校验 HTTP(S) 订阅地址合法性。/ Validate an HTTP(S) feed URL.
bool feeds_url_valid(const char *url);

/// 基于基准 URL 解析相对或绝对 href。/ Resolve relative or absolute href against base_url.
bool feeds_resolve_url(const char *base_url, const char *href, char *out, size_t cap);

/// 将内部源结构脱敏为对外结构（不含密码明文）。/ Redact internal source into public view without password.
void feeds_source_redact(const feed_source_t *src, feed_public_source_t *out);

/// 构造 HTTP Basic Authorization 请求头值（"Basic ..."）。/ Build HTTP Basic Authorization header value.
bool feeds_build_basic_auth(const char *user, const char *pass, char *out, size_t cap);

/// 读取全部订阅源（本机含凭据）。/ Load all feed sources including local credentials.
bool feeds_store_load(feed_source_t *out, size_t max_items, size_t *out_count);

/// 保存订阅源列表。/ Persist the full feed source list.
bool feeds_store_save(const feed_source_t *items, size_t count);

/// 新增或按 URL 更新订阅源；auth_pass 为空且 clear_auth=false 时保留已有密码。
/// Upsert a feed source by URL; preserves existing password when auth_pass is empty and clear_auth is false.
bool feeds_store_upsert(const char *name, const char *url, feed_kind_t kind,
                        const char *auth_user, const char *auth_pass, bool clear_auth,
                        bool *out_full);

/// 按 URL 删除订阅源。/ Delete a feed source by URL.
bool feeds_store_delete(const char *url, bool *out_found);

/// 读取脱敏后的订阅源列表（供 HTTP/UI 使用）。/ List redacted feed sources for HTTP/UI.
size_t feeds_store_list_public(feed_public_source_t *out, size_t max_items);

/// 解析 RSS 2.0、Atom 1.0 或 OPDS 1.2 XML 文档。/ Parse RSS 2.0, Atom 1.0 or OPDS 1.2 XML document.
bool feeds_parse_xml(const char *xml, size_t len, const char *base_url,
                     feed_parsed_t *out, size_t max_items);

/// 释放解析结果占用的堆内存。/ Free heap memory owned by a parsed feed.
void feeds_parsed_free(feed_parsed_t *parsed);

/// 生成 TF 卡离线缓存文件路径 <sd_root>/feeds/<hash>.xml。
/// Compute offline cache file path under <sd_root>/feeds/<hash>.xml.
bool feeds_cache_path(const char *sd_root, const char *url, char *out, size_t cap);

/// 将最近拉取的 XML 写入 TF 卡离线缓存。/ Write fetched XML to TF-card offline cache.
bool feeds_cache_write(const char *sd_root, const char *url, const char *data, size_t len);

/// 从 TF 卡离线缓存读取上次拉取的 XML（调用方 free(*out_data)）。
/// Read cached XML from TF card (caller must free(*out_data)).
bool feeds_cache_read(const char *sd_root, const char *url, char **out_data, size_t *out_len);

#ifdef __cplusplus
}
#endif
