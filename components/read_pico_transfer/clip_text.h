/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：规范化剪藏文本中的换行与控制字符。
 * English: Normalize newlines and control characters in clipped text.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

static inline bool clip_safe_text(char *text, bool title) {
    size_t read = 0, write_at = 0;
    while (text[read]) {
        unsigned char c = (unsigned char)text[read++];
        if (c == '\r') {
            if (text[read] == '\n') ++read;
            text[write_at++] = title ? ' ' : '\n';
        } else if (c == '\n') {
            text[write_at++] = title ? ' ' : '\n';
        } else if ((c < 32 && c != '\t') || c == 127) {
            text[write_at++] = ' ';
        } else {
            text[write_at++] = (char)c;
        }
    }
    text[write_at] = '\0';
    return true;
}
