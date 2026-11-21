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
