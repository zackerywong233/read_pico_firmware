/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：锁屏图片（PNG/JPEG）文件头校验与手写便签文件名校验，不含平台依赖。
 *
 * English: Header validation for lock-screen images (PNG/JPEG) and sketch filenames without platform dependencies.
 *
 * 冻结：锁屏图片上限 2 MiB，仅接受 PNG 与 JPEG；手写便签仅接受扁平目录内的合法 .png 文件名。
 * Frozen: Lock-screen images are capped at 2 MiB and restricted to PNG/JPEG; sketch names accept flat valid .png files only.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define TRANSFER_SCREENSAVER_MAX (2U * 1024U * 1024U)
#define TRANSFER_IMAGE_MAX_DIM 8192U
#define TRANSFER_SKETCH_LIST_MAX 64U

typedef enum {
    TRANSFER_IMAGE_INVALID = 0, ///< 无效格式 / Invalid format
    TRANSFER_IMAGE_PNG,         ///< PNG 图片 / PNG image
    TRANSFER_IMAGE_JPEG,        ///< JPEG 图片 / JPEG image
} transfer_image_format_t;

static inline uint32_t transfer_image_be32(const uint8_t p[4]) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline transfer_image_format_t detect_image_magic(const uint8_t *buf, size_t len) {
    static const uint8_t png_sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (!buf) return TRANSFER_IMAGE_INVALID;
    if (len >= 24 && memcmp(buf, png_sig, sizeof(png_sig)) == 0) {
        if (transfer_image_be32(buf + 8) != 13U || memcmp(buf + 12, "IHDR", 4) != 0)
            return TRANSFER_IMAGE_INVALID;
        uint32_t w = transfer_image_be32(buf + 16);
        uint32_t h = transfer_image_be32(buf + 20);
        if (!w || !h || w > TRANSFER_IMAGE_MAX_DIM || h > TRANSFER_IMAGE_MAX_DIM)
            return TRANSFER_IMAGE_INVALID;
        return TRANSFER_IMAGE_PNG;
    }
    if (len >= 4 && buf[0] == 0xFF && buf[1] == 0xD8 && buf[2] == 0xFF && buf[3] != 0x00)
        return TRANSFER_IMAGE_JPEG;
    return TRANSFER_IMAGE_INVALID;
}

static inline bool valid_image_file(const char *path, transfer_image_format_t *fmt_out) {
    if (fmt_out) *fmt_out = TRANSFER_IMAGE_INVALID;
    if (!path) return false;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (st.st_size < 16 || (uint64_t)st.st_size > TRANSFER_SCREENSAVER_MAX) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t head[33] = {0};
    size_t got = fread(head, 1, sizeof(head), f);
    fclose(f);
    transfer_image_format_t fmt = detect_image_magic(head, got);
    if (fmt == TRANSFER_IMAGE_PNG && (st.st_size < 33 || got < 33)) return false;
    if (fmt == TRANSFER_IMAGE_INVALID) return false;
    if (fmt_out) *fmt_out = fmt;
    return true;
}

static inline int transfer_img_hex(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static inline bool decode_sketch_name(const char *encoded, char out[121]) {
    if (!encoded || !out) return false;
    size_t n = 0;
    while (*encoded) {
        unsigned char c = (unsigned char)*encoded++;
        if (c == '%') {
            if (!encoded[0] || !encoded[1]) return false;
            int a = transfer_img_hex((unsigned char)encoded[0]);
            int b = transfer_img_hex((unsigned char)encoded[1]);
            if (a < 0 || b < 0) return false;
            c = (unsigned char)((a << 4) | b);
            encoded += 2;
        }
        if (n + 1 >= 121 || c < 32 || c == 127 || strchr("/\\:*?\"<>|", c)) return false;
        out[n++] = (char)c;
    }
    out[n] = 0;
    if (!n || out[0] == '.' || out[0] == ' ' || out[n - 1] == ' ' || strstr(out, "..")) return false;
    const char *ext = strrchr(out, '.');
    if (!ext || strcasecmp(ext, ".png") != 0 || ext == out) return false;
    for (size_t i = 0; i < n;) {
        uint32_t cp;
        unsigned more;
        unsigned char c = (unsigned char)out[i++];
        if (c < 128) continue;
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; more = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; more = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; more = 3; }
        else return false;
        unsigned width = more;
        while (more--) {
            if (i == n || ((unsigned char)out[i] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | ((unsigned char)out[i++] & 63);
        }
        if ((width == 2 && cp < 0x800) || (width == 3 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return false;
    }
    return true;
}
