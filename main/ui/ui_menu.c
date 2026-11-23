/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：屏下三键命中测试与底部按键说明带绘制。
 *
 * English: Hit-test for the three physical keys and drawing for the bottom key-hint strip.
 */

#include "ui_menu.h"

#include "ui_kit.h"

// 按 UI_KEY_PITCH 均分，三个按键的中心（80/240/400）正好落在各段中点，
// 手指偏一点也还在同一段里。
// Split by UI_KEY_PITCH so the three key centers (80/240/400) sit at mid-segment;
// a slight finger miss still lands in the same segment.
int ui_key_hit_test(uint16_t x, uint16_t y) {
    if (y < UI_KEY_AREA_TOP) return -1;
    int index = (int)x / UI_KEY_PITCH;
    if (index >= UI_KEY_COUNT) return -1;
    return index;
}

void ui_draw_key_hints(uint8_t* framebuffer, const ui_key_hints_t* hints) {
    if (framebuffer == NULL || hints == NULL || hints->hidden) return;
    const int width = epd_rotated_display_width();
    if (width <= 0) return;

    ui_clear_rect_fast(framebuffer, (EpdRect){
        .x = 0,
        .y = UI_KEY_HINT_RULE_Y,
        .width = width,
        .height = UI_LOCK_HEIGHT - UI_KEY_HINT_RULE_Y,
    });
    ui_hairline(framebuffer, UI_KEY_HINT_RULE_Y, 0, width, UI_GRAY_LIGHT);

    const int col_w = width / 3;
    const int cy = UI_KEY_HINT_TOP + UI_KEY_HINT_H / 2;
    const int div_h = 20;
    const int div_y = cy - div_h / 2;
    epd_fill_rect((EpdRect){ .x = col_w, .y = div_y, .width = 1, .height = div_h },
                  UI_GRAY_LIGHT, framebuffer);
    epd_fill_rect((EpdRect){ .x = col_w * 2, .y = div_y, .width = 1, .height = div_h },
                  UI_GRAY_LIGHT, framebuffer);

    const char* labels[3] = {
        hints->left,
        hints->center != NULL ? hints->center : "刷新 · 长按桌面",
        hints->right,
    };
    for (int i = 0; i < 3; i++) {
        if (labels[i] == NULL || labels[i][0] == '\0') continue;
        int cx = i * col_w + col_w / 2;
        if (i == 2) cx = col_w * 2 + (width - col_w * 2) / 2;
        ui_text_vc(framebuffer, cx, cy, UI_PX_KEY_HINT, labels[i],
                   EPD_DRAW_ALIGN_CENTER, false);
    }
}
