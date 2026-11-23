/* SPDX-License-Identifier: Apache-2.0
 * 中文：屏下三键命中与底部按键说明带绘制测试。
 * English: Physical key hit-test and bottom key-hint strip drawing tests.
 */
#include "ui_menu.h"
#include "../main/ui/ui_kit.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int clears, hairlines, fills, texts;
static char last_texts[4][64];
static int last_xs[4];

int epd_rotated_display_width(void) { return 684; }
void ui_clear_rect_fast(uint8_t* f, EpdRect r) { (void)f; assert(r.y == UI_KEY_HINT_RULE_Y); clears++; }
void ui_hairline(uint8_t* f, int y, int x, int w, uint8_t c) { (void)f; (void)x; (void)w; (void)c; assert(y == UI_KEY_HINT_RULE_Y); hairlines++; }
void epd_fill_rect(EpdRect r, uint8_t c, uint8_t* f) { (void)r; (void)c; (void)f; fills++; }
void ui_text_vc(uint8_t* f, int x, int y, int p, const char* t, enum EpdFontFlags a, bool i) {
    (void)f; (void)a; (void)i;
    assert(y == UI_KEY_HINT_TOP + UI_KEY_HINT_H / 2);
    assert(p == UI_PX_KEY_HINT);
    if (texts < 4) {
        snprintf(last_texts[texts], sizeof(last_texts[0]), "%s", t);
        last_xs[texts] = x;
    }
    texts++;
}

int main(void) {
    // 屏下三键坐标命中。/ Physical key hit-test.
    assert(ui_key_hit_test(80, 1299) == -1);
    assert(ui_key_hit_test(80, 1300) == UI_KEY_1);
    assert(ui_key_hit_test(240, 1500) == UI_KEY_2);
    assert(ui_key_hit_test(400, 1500) == UI_KEY_3);
    assert(ui_key_hit_test(480, 1500) == -1);

    // 底部按键说明带绘制与隐藏。/ Key hint strip draw and hide.
    uint8_t fb[16] = {0};
    ui_key_hints_t h = { .left = "返回", .center = NULL, .right = "下一页", .hidden = false };
    ui_draw_key_hints(fb, &h);
    assert(clears == 1 && hairlines == 1 && fills == 2 && texts == 3);
    assert(strcmp(last_texts[0], "返回") == 0 && last_xs[0] == 114);
    assert(strcmp(last_texts[1], "刷新 · 长按桌面") == 0 && last_xs[1] == 342);
    assert(strcmp(last_texts[2], "下一页") == 0 && last_xs[2] == 570);

    h.hidden = true;
    ui_draw_key_hints(fb, &h);
    assert(clears == 1 && texts == 3);

    puts("ui_menu: physical key hit-test and key-hint strip passed");
    return 0;
}
