/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：验证剪藏换行折叠与控制字符清洗。
 * English: Verify clip newline folding and control-character cleanup.
 */
#include <assert.h>
#include <string.h>

#include "clip_text.h"

int main(void) {
    char body[] = "A\r\nB\rC\nD\fE\vF\001G\177H\tI";
    assert(clip_safe_text(body, false));
    assert(strcmp(body, "A\nB\nC\nD E F G H\tI") == 0);

    char title[] = "One\r\nTwo\rThree\nFour\fFive";
    assert(clip_safe_text(title, true));
    assert(strcmp(title, "One Two Three Four Five") == 0);
    return 0;
}
