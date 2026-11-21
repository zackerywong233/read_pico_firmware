/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * OTA 元数据与电源准入的纯策略函数。
 * Pure policy helpers for OTA metadata and battery admission.
 * 冻结：非充电且电量低于 20% 时拒绝安装。
 * Frozen: Reject installation below 20% when not charging.
 */
#pragma once
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static inline int ota_update_compare_versions(const char *a, const char *b) {
    if (!a || !b) return 0;
    if (*a == 'v' || *a == 'V') ++a;
    if (*b == 'v' || *b == 'V') ++b;
    for (unsigned field = 0; field < 3; ++field) {
        if (*a < '0' || *a > '9' || *b < '0' || *b > '9') return 0;
        char *ae, *be;
        unsigned long av = strtoul(a, &ae, 10), bv = strtoul(b, &be, 10);
        if (av != bv) return av < bv ? -1 : 1;
        a = ae; b = be;
        if (field < 2 && (*a++ != '.' || *b++ != '.')) return 0;
    }
    if (*a == '-' || *b == '-') {
        if (*a == '-' && *b != '-') return -1;
        if (*b == '-' && *a != '-') return 1;
        return strcmp(a, b) < 0 ? -1 : strcmp(a, b) > 0 ? 1 : 0;
    }
    return 0;
}

static inline bool ota_update_battery_allowed(uint16_t soc_permille, bool charging) {
    return soc_permille <= 1000 && (soc_permille >= 200 || charging);
}

static inline bool ota_update_parse_digest(const char *value, char hex[65]) {
    if (!value || !hex || strncmp(value, "sha256:", 7) || strlen(value + 7) != 64) return false;
    for (unsigned i = 0; i < 64; ++i) {
        char c = value[i + 7];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        hex[i] = c >= 'A' && c <= 'F' ? (char)(c + ('a' - 'A')) : c;
    }
    hex[64] = 0;
    return true;
}

static inline bool ota_update_parse_checksum_file(const char *value, char hex[65]) {
    if (!value || strlen(value) < 64) return false;
    char raw[72] = "sha256:";
    memcpy(raw + 7, value, 64);
    raw[71] = 0;
    if (!ota_update_parse_digest(raw, hex)) return false;
    const char *tail = value + 64;
    if (*tail == '\r' || *tail == '\n' || *tail == 0) return true;
    if (*tail != ' ' && *tail != '\t' && *tail != '*') return false;
    while (*tail == ' ' || *tail == '\t' || *tail == '*') ++tail;
    const size_t name_len = sizeof("pico-os-app.bin") - 1;
    if (strncmp(tail, "pico-os-app.bin", name_len)) return false;
    tail += name_len;
    return *tail == 0 || (*tail == '\n' && tail[1] == 0) || (*tail == '\r' && tail[1] == '\n' && tail[2] == 0);
}

static inline bool ota_update_parse_release_redirect(const char *location,
                                                     char version[32],
                                                     char url[512],
                                                     char checksum_url[512]) {
    static const char tag_prefix[] = "https://github.com/zackerywong233/read_pico_firmware/releases/tag/";
    static const char dl_prefix[] = "https://github.com/zackerywong233/read_pico_firmware/releases/download/";
    if (!location || !version || !url || !checksum_url) return false;
    size_t plen = sizeof(tag_prefix) - 1;
    if (strncmp(location, tag_prefix, plen) != 0) return false;
    const char *tag = location + plen;
    size_t tlen = 0;
    while (tag[tlen] && tag[tlen] != '/' && tag[tlen] != '?' && tag[tlen] != '#') {
        unsigned char c = (unsigned char)tag[tlen];
        bool ok_char = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                       (c >= 'a' && c <= 'z') || c == '.' || c == '-' || c == '_';
        if (!ok_char || tlen >= 31) return false;
        ++tlen;
    }
    if (tlen < 2 || (tag[0] != 'v' && tag[0] != 'V')) return false;
    char ver[32];
    memcpy(ver, tag, tlen);
    ver[tlen] = '\0';
    // 要求主.次.修订三段数字。/ Require numeric major.minor.patch.
    const char *p = ver + 1;
    for (unsigned field = 0; field < 3; ++field) {
        if (*p < '0' || *p > '9') return false;
        while (*p >= '0' && *p <= '9') ++p;
        if (field < 2 && *p++ != '.') return false;
    }
    if (*p != '\0' && *p != '-') return false;
    if (ota_update_compare_versions(ver, "v0.0.0") < 0) return false;
    int u1 = snprintf(url, 512, "%s%s/pico-os-app.bin", dl_prefix, ver);
    int u2 = snprintf(checksum_url, 512, "%s%s/pico-os-app.bin.sha256", dl_prefix, ver);
    if (u1 <= 0 || u1 >= 512 || u2 <= 0 || u2 >= 512) return false;
    strcpy(version, ver);
    return true;
}
