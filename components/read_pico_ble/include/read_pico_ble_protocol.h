/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：BLE 配套控制协议的 JSON 命令校验、短文本文件名清洗与 UTF-8 正文归一化，不含平台依赖。
 *
 * English: Platform-independent BLE companion command validation, clip filename sanitization and UTF-8 text normalization.
 *
 * 冻结：严禁暴露 VCOM 或电源芯片写操作；Wi-Fi 密码与配对码不得回显；文件名禁止路径穿越。
 * Frozen: Never expose VCOM or PMIC writes; never echo Wi-Fi passwords or PINs; reject path traversal in filenames.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "read_pico_companion_policy.h"

#define READ_PICO_BLE_CLIP_TEXT_MAX 3584U
#define READ_PICO_BLE_CLIP_TITLE_MAX 64U
#define READ_PICO_BLE_CLIP_NAME_MAX 80U

typedef enum {
    BLE_CMD_UNKNOWN = 0,
    BLE_CMD_PING,
    BLE_CMD_STATUS,
    BLE_CMD_AUTH_VERIFY,
    BLE_CMD_WIFI_LIST,
    BLE_CMD_WIFI_SCAN,
    BLE_CMD_WIFI_SAVE,
    BLE_CMD_WIFI_FORGET,
    BLE_CMD_TRANSFER_START,
    BLE_CMD_TRANSFER_STOP,
    BLE_CMD_TIME_SYNC,
    BLE_CMD_SETTINGS_GET,
    BLE_CMD_SETTINGS_PATCH,
    BLE_CMD_MEMO_SEND,
    BLE_CMD_DASHBOARD_GET,
    BLE_CMD_WEBOPEN,
    BLE_CMD_OPEN_APP,
    BLE_CMD_AGENT_GET,
    BLE_CMD_AGENT_SET,
    BLE_CMD_AMBIENT_GET,
    BLE_CMD_AMBIENT_SET,
    BLE_CMD_REFRESH_SCREEN,
    BLE_CMD_LOCK,
    BLE_CMD_WEBMARKS_GET,
    BLE_CMD_WEBMARKS_ADD,
    BLE_CMD_WEBMARKS_DEL,
    BLE_CMD_FOCUS_GET,
    BLE_CMD_FOCUS_START,
    BLE_CMD_FOCUS_STOP,
} ble_cmd_type_t;

typedef enum {
    BLE_APP_TARGET_NONE = 0,
    BLE_APP_TARGET_HOME,
    BLE_APP_TARGET_BOOKS,
    BLE_APP_TARGET_TRANSFER,
    BLE_APP_TARGET_WEB,
    BLE_APP_TARGET_SKETCH,
    BLE_APP_TARGET_FOCUS,
    BLE_APP_TARGET_AGENT,
    BLE_APP_TARGET_PERSONAL,
    BLE_APP_TARGET_SETTINGS,
    BLE_APP_TARGET_FEEDS,
    BLE_APP_TARGET_FILES,
} ble_app_target_t;

typedef enum {
    BLE_TRANSFER_MODE_AUTO = 0,
    BLE_TRANSFER_MODE_AP,
    BLE_TRANSFER_MODE_STA,
} ble_transfer_mode_req_t;

typedef enum {
    BLE_MEMO_TARGET_CLIP = 0,
    BLE_MEMO_TARGET_DASHBOARD,
} ble_memo_target_t;

/// 将 BLE 短文本标题清洗为安全 UTF-8 `.txt` 文件名；空标题或非法字符回退到 `fallback_stem.txt`。
/// Sanitize a BLE memo title into a safe UTF-8 `.txt` filename; empty or invalid titles fall back to `fallback_stem.txt`.
static inline bool ble_sanitize_clip_filename(const char *title, const char *fallback_stem,
                                              char out[READ_PICO_BLE_CLIP_NAME_MAX]) {
    if (!out || !fallback_stem || !fallback_stem[0]) return false;
    out[0] = '\0';
    char stem[READ_PICO_BLE_CLIP_TITLE_MAX + 1];
    size_t n = 0;

    if (title && title[0]) {
        // 清洗非法路径字符与控制字符，按 UTF-8 字符边界截断。
        // Strip path/control characters and truncate on UTF-8 codepoint boundaries.
        const size_t tlen = strlen(title);
        for (size_t i = 0; i < tlen;) {
            unsigned char c = (unsigned char)title[i];
            if (c < 128U) {
                ++i;
                if (c < 32U || c == 127U || strchr("/\\:*?\"<>|", c)) continue;
                if (c == '.' && n == 0U) continue;
                if (c == ' ' && (n == 0U || stem[n - 1] == ' ')) continue;
                if (c == '.' && n > 0U && stem[n - 1] == '.') continue;
                if (n + 1U > READ_PICO_BLE_CLIP_TITLE_MAX) break;
                stem[n++] = (char)c;
                continue;
            }
            size_t seq_len = 0;
            uint32_t cp = 0;
            if (c >= 0xc2U && c <= 0xdfU) { cp = c & 31U; seq_len = 2U; }
            else if (c >= 0xe0U && c <= 0xefU) { cp = c & 15U; seq_len = 3U; }
            else if (c >= 0xf0U && c <= 0xf4U) { cp = c & 7U; seq_len = 4U; }
            else { ++i; continue; }
            if (i + seq_len > tlen) break;
            bool valid = true;
            for (size_t k = 1U; k < seq_len; ++k) {
                unsigned char b = (unsigned char)title[i + k];
                if ((b & 0xc0U) != 0x80U) { valid = false; break; }
                cp = (cp << 6) | (b & 63U);
            }
            if (!valid || (seq_len == 3U && cp < 0x800U) || (seq_len == 4U && cp < 0x10000U) ||
                (cp >= 0xd800U && cp <= 0xdfffU) || cp > 0x10ffffU) {
                ++i;
                continue;
            }
            if (n + seq_len > READ_PICO_BLE_CLIP_TITLE_MAX) break;
            memcpy(stem + n, title + i, seq_len);
            n += seq_len;
            i += seq_len;
        }
        while (n > 0U && (stem[n - 1] == ' ' || stem[n - 1] == '.')) --n;
        // 若末尾已有 .txt（不区分大小写），避免重复追加。
        // Strip trailing .txt (case-insensitive) to avoid double extensions.
        if (n >= 4U && stem[n - 4] == '.' &&
            (stem[n - 3] == 't' || stem[n - 3] == 'T') &&
            (stem[n - 2] == 'x' || stem[n - 2] == 'X') &&
            (stem[n - 1] == 't' || stem[n - 1] == 'T')) {
            n -= 4U;
            while (n > 0U && (stem[n - 1] == ' ' || stem[n - 1] == '.')) --n;
        }
    }
    stem[n] = '\0';

    if (n == 0U || strstr(stem, "..")) {
        size_t flen = strlen(fallback_stem);
        if (flen > READ_PICO_BLE_CLIP_TITLE_MAX) flen = READ_PICO_BLE_CLIP_TITLE_MAX;
        memcpy(stem, fallback_stem, flen);
        stem[flen] = '\0';
        n = flen;
    }

    if (n + 5U > READ_PICO_BLE_CLIP_NAME_MAX) return false;
    memcpy(out, stem, n);
    memcpy(out + n, ".txt", 5U);
    return true;
}

/// 校验并归一化短文本剪藏内容（折叠 CRLF 为 LF，拒绝非法控制字符与损坏 UTF-8）。
/// Validate and normalize short clip text (fold CRLF to LF; reject bad control chars and invalid UTF-8).
static inline bool ble_normalize_clip_text(const char *src, char *out, size_t out_cap, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!src || !out || out_cap < 2U) return false;
    size_t max_bytes = out_cap - 1U;
    if (max_bytes > READ_PICO_BLE_CLIP_TEXT_MAX) max_bytes = READ_PICO_BLE_CLIP_TEXT_MAX;
    size_t n = 0;
    for (size_t i = 0; src[i]; ++i) {
        char c = src[i];
        if (c == '\r') {
            if (src[i + 1] == '\n') ++i;
            c = '\n';
        }
        if (n >= max_bytes) {
            out[0] = '\0';
            return false;
        }
        out[n++] = c;
    }
    out[n] = '\0';
    if (n == 0U || !companion_valid_utf8_note(out, n)) {
        out[0] = '\0';
        return false;
    }
    if (out_len) *out_len = n;
    return true;
}

static inline ble_cmd_type_t ble_parse_cmd_type(const char *cmd) {
    if (!cmd || !cmd[0]) return BLE_CMD_UNKNOWN;
    if (!strcmp(cmd, "ping")) return BLE_CMD_PING;
    if (!strcmp(cmd, "status")) return BLE_CMD_STATUS;
    if (!strcmp(cmd, "auth_verify")) return BLE_CMD_AUTH_VERIFY;
    if (!strcmp(cmd, "wifi_list")) return BLE_CMD_WIFI_LIST;
    if (!strcmp(cmd, "wifi_scan")) return BLE_CMD_WIFI_SCAN;
    if (!strcmp(cmd, "wifi_save")) return BLE_CMD_WIFI_SAVE;
    if (!strcmp(cmd, "wifi_forget")) return BLE_CMD_WIFI_FORGET;
    if (!strcmp(cmd, "transfer_start")) return BLE_CMD_TRANSFER_START;
    if (!strcmp(cmd, "transfer_stop")) return BLE_CMD_TRANSFER_STOP;
    if (!strcmp(cmd, "time_sync")) return BLE_CMD_TIME_SYNC;
    if (!strcmp(cmd, "settings_get")) return BLE_CMD_SETTINGS_GET;
    if (!strcmp(cmd, "settings_patch")) return BLE_CMD_SETTINGS_PATCH;
    if (!strcmp(cmd, "memo_send")) return BLE_CMD_MEMO_SEND;
    if (!strcmp(cmd, "dashboard_get")) return BLE_CMD_DASHBOARD_GET;
    if (!strcmp(cmd, "webopen")) return BLE_CMD_WEBOPEN;
    if (!strcmp(cmd, "open_app")) return BLE_CMD_OPEN_APP;
    if (!strcmp(cmd, "agent_get")) return BLE_CMD_AGENT_GET;
    if (!strcmp(cmd, "agent_set")) return BLE_CMD_AGENT_SET;
    if (!strcmp(cmd, "ambient_get")) return BLE_CMD_AMBIENT_GET;
    if (!strcmp(cmd, "ambient_set")) return BLE_CMD_AMBIENT_SET;
    if (!strcmp(cmd, "refresh_screen")) return BLE_CMD_REFRESH_SCREEN;
    if (!strcmp(cmd, "lock")) return BLE_CMD_LOCK;
    if (!strcmp(cmd, "webmarks_get")) return BLE_CMD_WEBMARKS_GET;
    if (!strcmp(cmd, "webmarks_add")) return BLE_CMD_WEBMARKS_ADD;
    if (!strcmp(cmd, "webmarks_del")) return BLE_CMD_WEBMARKS_DEL;
    if (!strcmp(cmd, "focus_get")) return BLE_CMD_FOCUS_GET;
    if (!strcmp(cmd, "focus_start")) return BLE_CMD_FOCUS_START;
    if (!strcmp(cmd, "focus_stop")) return BLE_CMD_FOCUS_STOP;
    return BLE_CMD_UNKNOWN;
}

/// 当开启 BLE 配对码保护时，除 ping / status / auth_verify 外的指令均须先鉴权。
/// When BLE PIN protection is enabled, all commands except ping, status and auth_verify require authentication.
static inline bool ble_cmd_requires_auth(ble_cmd_type_t cmd) {
    return cmd != BLE_CMD_PING && cmd != BLE_CMD_STATUS &&
           cmd != BLE_CMD_AUTH_VERIFY && cmd != BLE_CMD_UNKNOWN;
}

static inline ble_app_target_t ble_parse_app_target(const char *name) {
    if (!name || !name[0]) return BLE_APP_TARGET_NONE;
    if (!strcmp(name, "home")) return BLE_APP_TARGET_HOME;
    if (!strcmp(name, "books")) return BLE_APP_TARGET_BOOKS;
    if (!strcmp(name, "transfer")) return BLE_APP_TARGET_TRANSFER;
    if (!strcmp(name, "web")) return BLE_APP_TARGET_WEB;
    if (!strcmp(name, "sketch")) return BLE_APP_TARGET_SKETCH;
    if (!strcmp(name, "focus")) return BLE_APP_TARGET_FOCUS;
    if (!strcmp(name, "agent")) return BLE_APP_TARGET_AGENT;
    if (!strcmp(name, "personal")) return BLE_APP_TARGET_PERSONAL;
    if (!strcmp(name, "settings")) return BLE_APP_TARGET_SETTINGS;
    if (!strcmp(name, "feeds")) return BLE_APP_TARGET_FEEDS;
    if (!strcmp(name, "files")) return BLE_APP_TARGET_FILES;
    return BLE_APP_TARGET_NONE;
}

static inline bool ble_valid_http_url(const char *url, size_t max_len) {
    if (!url || !url[0]) return false;
    size_t len = strlen(url);
    if (len >= max_len) return false;
    bool http = !strncmp(url, "http://", 7);
    bool https = !strncmp(url, "https://", 8);
    if (!http && !https) return false;
    size_t host_start = http ? 7U : 8U;
    if (len <= host_start) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)url[i];
        if (c <= 32U || c >= 127U) return false;
    }
    return true;
}

/// 校验 Wi-Fi SSID 与密码（与传书页一致：SSID 1..32 字节无控制字符，密码空或 8..63 可打印 ASCII）。
/// Validate Wi-Fi SSID and password (matching Transfer: 1..32 byte SSID without controls; password empty or 8..63 printable ASCII).
static inline bool ble_valid_wifi_credentials(const char *ssid, const char *password) {
    if (!ssid || !password) return false;
    size_t slen = strlen(ssid);
    if (slen == 0U || slen > 32U) return false;
    for (size_t i = 0; i < slen; ++i) {
        unsigned char c = (unsigned char)ssid[i];
        if (c < 32U || c == 127U) return false;
    }
    size_t plen = strlen(password);
    if (plen != 0U && (plen < 8U || plen > 63U)) return false;
    for (size_t i = 0; i < plen; ++i) {
        unsigned char c = (unsigned char)password[i];
        if (c < 32U || c > 126U) return false;
    }
    return true;
}

/// 解析通用命令信封：提取可选数字 id 与必选字符串 cmd。调用方须负责 cJSON_Delete(*root_out)。
/// Parse common command envelope: optional numeric id and required cmd string. Caller owns *root_out.
static inline bool ble_parse_command_envelope(const char *json, size_t len,
                                              cJSON **root_out, bool *has_id_out, int *id_out,
                                              ble_cmd_type_t *cmd_out, char cmd_name[32]) {
    if (root_out) *root_out = NULL;
    if (has_id_out) *has_id_out = false;
    if (id_out) *id_out = 0;
    if (cmd_out) *cmd_out = BLE_CMD_UNKNOWN;
    if (cmd_name) cmd_name[0] = '\0';
    if (!json || !len || !root_out || !cmd_out) return false;

    cJSON *root = companion_parse_strict_json(json, len);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (id_item) {
        int id_val = 0;
        if (!companion_json_int_range(id_item, 0, 2147483647, &id_val)) {
            cJSON_Delete(root);
            return false;
        }
        if (has_id_out) *has_id_out = true;
        if (id_out) *id_out = id_val;
    }

    const cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd_item) || !cmd_item->valuestring || !cmd_item->valuestring[0]) {
        cJSON_Delete(root);
        return false;
    }
    if (cmd_name) {
        snprintf(cmd_name, 32, "%.31s", cmd_item->valuestring);
    }
    *cmd_out = ble_parse_cmd_type(cmd_item->valuestring);
    *root_out = root;
    return true;
}

static inline cJSON *ble_make_response_base(bool has_id, int id, const char *cmd, bool ok) {
    cJSON *res = cJSON_CreateObject();
    if (!res) return NULL;
    if (has_id) cJSON_AddNumberToObject(res, "id", id);
    if (cmd && cmd[0]) cJSON_AddStringToObject(res, "cmd", cmd);
    cJSON_AddBoolToObject(res, "ok", ok);
    return res;
}

static inline cJSON *ble_make_error_response(bool has_id, int id, const char *cmd,
                                             const char *code, const char *message) {
    cJSON *res = ble_make_response_base(has_id, id, cmd, false);
    if (!res) return NULL;
    if (code && code[0]) cJSON_AddStringToObject(res, "code", code);
    cJSON_AddStringToObject(res, "error", (message && message[0]) ? message : "请求无效");
    return res;
}
