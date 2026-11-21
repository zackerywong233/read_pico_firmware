/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 凭据校验和有界连接状态机；不含平台资源。/ Credential validation and bounded connection policy without platform resources.
 * 冻结：最多保存五组 Wi-Fi 凭据；密码不进入公开状态；每轮连接最多三次、15秒。
 * Frozen: Store up to five Wi-Fi credentials; passwords never enter public status; three attempts within 15 seconds.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "include/read_pico_transfer_network.h"

// 调用方在同一锁下检查并改变准入状态。/ Caller checks and changes admission under the same lock.
static bool admission_begin(bool stopping, bool *uploading) {
    if (stopping || *uploading) return false;
    *uploading = true; return true;
}

static bool admission_stop(bool *stopping, bool uploading) {
    if (uploading) return false;
    *stopping = true; return true;
}

// 正文须在15秒内收齐；每次阻塞接收前后检查停止。/ Accept bodies only within 15 seconds; check cancellation around each blocking receive.
static bool receive_credentials_body(char *body, size_t total,
        int (*receive)(void *, char *, size_t), bool (*stopped)(void *),
        int64_t (*now)(void *), void *ctx) {
    int64_t deadline = now(ctx) + 15000;
    size_t done = 0;
    while (done < total) {
        if (stopped(ctx) || now(ctx) >= deadline) return false;
        int got = receive(ctx, body + done, total - done);
        if (got <= 0 || (size_t)got > total - done || stopped(ctx) || now(ctx) >= deadline) return false;
        done += (size_t)got;
    }
    return true;
}

typedef struct {
    char ssid[33]; ///< 网络名，最多32字节 / Network name, at most 32 bytes
    char password[65]; ///< 私有凭据，用毕擦除 / Private secret, erased after use
} transfer_wifi_entry_t;

typedef struct {
    uint8_t version; ///< 格式版本 / Format version
    uint8_t count; ///< 有效网络数 / Number of networks
    uint8_t recent; ///< 最近成功的索引 / Index of most recently connected network
    uint8_t reserved;
    transfer_wifi_entry_t entries[5]; ///< 私有凭据，用毕擦除 / Private credentials, erased after use
} transfer_credentials_t;

typedef struct {
    uint8_t version; ///< 旧版格式 / Legacy format
    char ssid[33];
    char password[65];
} transfer_credentials_v1_t;

static void clear_secret(void *ptr, size_t n) {
    volatile unsigned char *p = ptr;
    while (n--) *p++ = 0;
}

static bool wifi_entry_valid(const transfer_wifi_entry_t *entry) {
    if (!memchr(entry->ssid, 0, sizeof(entry->ssid)) ||
        !memchr(entry->password, 0, sizeof(entry->password))) return false;
    size_t ssid_len = strlen(entry->ssid), pass_len = strlen(entry->password);
    if (!ssid_len || ssid_len > 32 || (pass_len && (pass_len < 8 || pass_len > 64))) return false;
    for (size_t i = 0; i < ssid_len; ++i)
        if ((unsigned char)entry->ssid[i] < 32 || (unsigned char)entry->ssid[i] == 127) return false;
    for (size_t i = 0; i < pass_len; ++i) {
        unsigned char p = (unsigned char)entry->password[i];
        if (p < 32 || p > 126) return false;
        if (pass_len == 64 && !((p >= '0' && p <= '9') || (p >= 'a' && p <= 'f') || (p >= 'A' && p <= 'F'))) return false;
    }
    return true;
}

static bool credentials_valid(const transfer_credentials_t *c) {
    if (c->version != 2 || !c->count || c->count > 5 || c->recent >= c->count || c->reserved) return false;
    for (size_t i = 0; i < c->count; ++i) {
        if (!wifi_entry_valid(&c->entries[i])) return false;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(c->entries[i].ssid, c->entries[j].ssid)) return false;
    }
    for (size_t i = c->count; i < 5; ++i) {
        const unsigned char *bytes = (const unsigned char *)&c->entries[i];
        for (size_t j = 0; j < sizeof(c->entries[i]); ++j) if (bytes[j]) return false;
    }
    return true;
}

static bool credentials_from_text(const char *ssid, const char *password, transfer_credentials_t *out) {
    memset(out, 0, sizeof(*out));
    if (!ssid || !password || strnlen(ssid, 33) > 32 || strnlen(password, 65) > 64) return false;
    out->version = 2; out->count = 1;
    strcpy(out->entries[0].ssid, ssid); strcpy(out->entries[0].password, password);
    if (credentials_valid(out)) return true;
    clear_secret(out, sizeof(*out)); return false;
}

static void scan_offer(read_pico_transfer_network_t out[READ_PICO_TRANSFER_SCAN_MAX], size_t *count,
                       const read_pico_transfer_network_t *candidate) {
    if (!candidate->ssid[0]) return;
    size_t index = 0;
    while (index < *count && strcmp(out[index].ssid, candidate->ssid)) ++index;
    if (index < *count) {
        if (candidate->rssi <= out[index].rssi) return;
    } else if (*count < READ_PICO_TRANSFER_SCAN_MAX) {
        ++*count;
    } else {
        index = *count - 1;
        if (candidate->rssi <= out[index].rssi) return;
    }
    out[index] = *candidate;
    while (index && out[index].rssi > out[index - 1].rssi) {
        read_pico_transfer_network_t swap = out[index - 1];
        out[index - 1] = out[index]; out[index] = swap; --index;
    }
}

static uint8_t saved_network_best_index(const transfer_credentials_t *saved,
        const read_pico_transfer_network_t *scanned, size_t scanned_count) {
    uint8_t best = saved->recent;
    int8_t best_rssi = INT8_MIN;
    for (size_t i = 0; i < scanned_count; ++i) {
        for (uint8_t j = 0; j < saved->count; ++j) {
            if (!strcmp(scanned[i].ssid, saved->entries[j].ssid) && scanned[i].rssi > best_rssi) {
                best_rssi = scanned[i].rssi;
                best = j;
            }
        }
    }
    return best;
}

static bool parse_credentials(const char *body, size_t length, transfer_credentials_t *out) {
    memset(out, 0, sizeof(*out));
    if (!length || length > 256 || memchr(body, 0, length) || body[length] != 0) return false;
    // cJSON使用C字符串；拒绝嵌入零以免字符串静默截断。/ cJSON uses C strings; reject embedded zero escapes to prevent truncation.
    if (strstr(body, "\\u0000")) return false;
    // 配置只有平面字符串字段；在递归解析前限制容器深度，保护HTTP任务栈。
    // Configuration has flat string fields; bound container depth before recursive parsing to protect the HTTP stack.
    bool quoted = false, escaped = false;
    unsigned depth = 0;
    for (size_t i = 0; i < length; ++i) {
        char ch = body[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 1) return false; }
        else if (ch == '}' || ch == ']') { if (!depth) return false; --depth; }
    }
    cJSON *json = cJSON_ParseWithLengthOpts(body, length + 1, NULL, true);
    if (!json) return false;
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
    bool valid = cJSON_IsObject(json) && cJSON_GetArraySize(json) == 2 && cJSON_IsString(ssid) && cJSON_IsString(password);
    if (valid) valid = strlen(ssid->valuestring) <= 32 && strlen(password->valuestring) <= 64;
    if (valid) {
        out->version = 2; out->count = 1; out->recent = 0;
        strcpy(out->entries[0].ssid, ssid->valuestring);
        strcpy(out->entries[0].password, password->valuestring);
        valid = credentials_valid(out);
    }
    // 解析树退出前擦除密码，包括验证失败的重复字段。/ Erase parsed passwords, including duplicate fields on invalid requests.
    for (cJSON *item = json->child; item; item = item->next)
        if (item->valuestring) clear_secret(item->valuestring, strlen(item->valuestring));
    cJSON_Delete(json);
    if (!valid) clear_secret(out, sizeof(*out));
    return valid;
}

static bool webopen_url_valid(const char *url) {
    if (!url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) || strlen(url) > 255) return false;
    const char *host = url + (url[4] == 's' ? 8 : 7);
    const char *end = strpbrk(host, "/?#");
    const char *authority_end = end ? end : host + strlen(host);
    if (authority_end == host || host[0] == ':' || memchr(host, '@', (size_t)(authority_end - host))) return false;
    if (host[0] == '[') {
        const char *close = memchr(host, ']', (size_t)(authority_end - host));
        if (!close || close == host + 1 || (close + 1 < authority_end && close[1] != ':')) return false;
    }
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p <= 32 || *p == 127) return false;
    return true;
}

static bool mdns_host_matches(const char *host, const char *name) {
    if (!host || !name || !name[0]) return false;
    char plain[70], port[80];
    snprintf(plain, sizeof(plain), "%s.local", name);
    snprintf(port, sizeof(port), "%s:80", plain);
    return !strcmp(host, plain) || !strcmp(host, port);
}

static bool management_host_allowed(const char *host, const char *ip, const char *mdns_host) {
    if (!host || !ip) return false;
    char port[72];
    snprintf(port, sizeof(port), "%.56s:80", ip);
    const char *name = mdns_host && mdns_host[0] ? mdns_host : "readpico";
    return !strcmp(host, ip) || !strcmp(host, port) || mdns_host_matches(host, name);
}

static bool management_origin_allowed(const char *origin, const char *ip_url, const char *mdns_host) {
    if (!origin || !ip_url) return false;
    char port[80];
    snprintf(port, sizeof(port), "%.63s:80", ip_url);
    char mdns_origin[96], mdns_port[112];
    snprintf(mdns_origin, sizeof(mdns_origin), "http://%s.local", mdns_host && mdns_host[0] ? mdns_host : "readpico");
    snprintf(mdns_port, sizeof(mdns_port), "%s:80", mdns_origin);
    return !strcmp(origin, ip_url) || !strcmp(origin, port) || !strcmp(origin, mdns_origin) || !strcmp(origin, mdns_port);
}

static bool parse_webopen(const char *body, size_t length, char url[256]) {
    if (!body || !length || length > 320 || memchr(body, 0, length) || body[length] != 0) return false;
    cJSON *json = cJSON_ParseWithLengthOpts(body, length + 1, NULL, true);
    if (!json) return false;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(json, "url");
    bool valid = cJSON_IsObject(json) && cJSON_GetArraySize(json) == 1 && cJSON_IsString(item) &&
        webopen_url_valid(item->valuestring);
    if (valid) strcpy(url, item->valuestring);
    cJSON_Delete(json);
    if (!valid && url) url[0] = 0;
    return valid;
}

typedef struct {
    bool online, pending, failed; ///< 在线、待连接与终止状态 / Online, pending and terminal states
    unsigned attempts; ///< 本轮尝试次数 / Attempts in this round
    int64_t deadline_ms, next_ms; ///< 总截止与下次尝试时刻 / Overall deadline and next attempt time
} transfer_connection_t;

static void connection_begin(transfer_connection_t *c, int64_t now) {
    *c = (transfer_connection_t){.pending = true, .deadline_ms = now + 15000, .next_ms = now};
}

static void connection_lost(transfer_connection_t *c, int64_t now) {
    if (c->failed) return;
    if (c->online) connection_begin(c, now);
    c->online = false; c->pending = true; c->next_ms = now + 1000;
}

// 0等待、1连接、-1终止本轮。/ 0 waits, 1 connects, -1 terminates this round.
static int connection_poll(transfer_connection_t *c, int64_t now) {
    if (c->online || c->failed) return 0;
    if (now >= c->deadline_ms || (c->pending && c->attempts >= 3)) {
        c->failed = true; c->pending = false; return -1;
    }
    if (!c->pending || now < c->next_ms) return 0;
    c->pending = false; ++c->attempts; return 1;
}
