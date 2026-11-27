/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：用主机像素缓冲验证按压背景和刷新区域。
 * English: Verify pressed backgrounds and refresh regions using a host pixel buffer.
 * 冻结：仅用于主机测试。/ Frozen: Host tests only.
 */
#include "ui_kit.h"
#include "ttf_font.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#define W 100
#define H 100
static uint8_t s_last_fg = 0xFF, s_last_bg = 0xFF;
static char s_last_text[64];
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t *fb) {
    assert(x >= 0 && x < W && y >= 0 && y < H); fb[y * W + x] = color;
}
void epd_fill_rect(EpdRect r, uint8_t color, uint8_t *fb) {
    for (int y = r.y; y < r.y + r.height; ++y)
        for (int x = r.x; x < r.x + r.width; ++x) epd_draw_pixel(x, y, color, fb);
}
void epd_fill_circle_helper(int x, int y, int radius, int corners, int delta, uint8_t color, uint8_t *fb) {
    (void)x; (void)y; (void)radius; (void)corners; (void)delta; (void)color; (void)fb;
}
void ttf_measure_line_px(int px, const char *text, int *above, int *below) {
    (void)text; if (above) *above = px; if (below) *below = 0;
}
void ttf_draw_text_px(uint8_t *fb, int x, int y, int px, const char *text, enum EpdFontFlags align, uint8_t fg, uint8_t bg) {
    (void)fb; (void)x; (void)y; (void)px; (void)align;
    s_last_fg = fg; s_last_bg = bg;
    snprintf(s_last_text, sizeof(s_last_text), "%s", text ? text : "");
}
static void rect_eq(EpdRect r, int x, int y, int w, int h) {
    assert(r.x == x && r.y == y && r.width == w && r.height == h);
}
int main(void) {
    rect_eq(ui_rect_union((EpdRect){0}, (EpdRect){10,20,30,40}), 10,20,30,40);
    rect_eq(ui_rect_union((EpdRect){10,20,30,40}, (EpdRect){5,30,50,10}), 5,20,50,40);
    rect_eq(ui_rect_union((EpdRect){-10,-20,30,40}, (EpdRect){0}), 0,0,20,20);
    rect_eq(ui_rect_union((EpdRect){680,1210,INT_MAX,INT_MAX}, (EpdRect){0}), 680,1210,4,6);
    rect_eq(ui_rect_union((EpdRect){INT_MIN,INT_MIN,10,10}, (EpdRect){0}), 0,0,0,0);
    uint8_t fb[W * H]; memset(fb, UI_GRAY_WHITE, sizeof(fb));
    ui_draw_pressed_round_rect(fb, (EpdRect){10,10,60,40}, 0);
    assert(fb[30 * W + 40] == UI_GRAY_LIGHT);
    assert(fb[30 * W + 14] == UI_GRAY_BLACK && fb[30 * W + 16] == UI_GRAY_BLACK);
    assert(fb[30 * W + 9] == UI_GRAY_WHITE);
    // 按压底图之后绘制黑字像素，文字不反色。/ Draw black text pixels after the pressed background, without inversion.
    epd_draw_pixel(40,30,UI_GRAY_BLACK,fb); assert(fb[30 * W + 40] == UI_GRAY_BLACK);
    // 禁用态按钮使用浅灰边框与灰字，内部保持白底。/ Disabled buttons use light-gray border and muted text on white fill.
    memset(fb, UI_GRAY_WHITE, sizeof(fb));
    ui_draw_button_disabled(fb, (EpdRect){10, 10, 80, 40}, "下一页");
    assert(fb[10 * W + 50] == UI_GRAY_LIGHT);
    assert(fb[30 * W + 50] == UI_GRAY_WHITE);
    assert(s_last_fg == UI_INK_MUTED && s_last_bg == UI_INK_WHITE && !strcmp(s_last_text, "下一页"));
    puts("ui kit host tests passed");
}
