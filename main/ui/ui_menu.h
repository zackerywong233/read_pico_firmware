/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：屏下三颗电容键坐标命中与底部按键说明带绘制。全屏列表菜单已取消。
 *
 * English: Hit-test for the three capacitive keys below the display and drawing
 * for the bottom key-hint strip. The full-screen menu list has been removed.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

// 触摸面板比显示区长，屏幕下方那条感应区里有三个按键。它们不在画面里，
// 没有东西可画，只能按坐标识别：实测中心 x=80/240/400，y≈1500。
// 显示区最大 y 是 1216，所以用 1300 当分界就不会和底部按键说明带抢点击。
// The touch panel is taller than the display; three keys sit in the strip
// below the image. Hit-test is by coordinate: measured centers x=80/240/400,
// y≈1500. Display y max is 1216, so 1300 as split avoids the bottom hint strip.
#define UI_KEY_COUNT 3
#define UI_KEY_AREA_TOP 1300
#define UI_KEY_PITCH 160
#define UI_KEY_1 0
#define UI_KEY_2 1
#define UI_KEY_3 2

/// 屏幕下方触摸按键区的命中测试，返回 UI_KEY_1/2/3，不在按键区时返回 -1。
/// Hit-test for the keys below the display. Returns UI_KEY_1/2/3, or -1 outside the key strip.
int ui_key_hit_test(uint16_t x, uint16_t y);

/// 绘制底部按键说明带（y=1170..1214，顶部分隔线 y=1168，三等分对齐屏下三键）。
/// Draw the bottom key-hint strip (y=1170..1214, top rule at y=1168, 3 equal columns aligned to physical keys).
void ui_draw_key_hints(uint8_t* framebuffer, const ui_key_hints_t* hints);

#ifdef __cplusplus
}
#endif
