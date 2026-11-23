/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：极简线框图标库与状态栏电池图标绘制，供九宫格桌面与阅读顶栏共用。
 *
 * English: Minimalist wireframe icon library and status-bar battery icon,
 * shared by the 3x3 grid home and reader top status bar.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_ICON_BOOK = 0,
    UI_ICON_TRANSFER,
    UI_ICON_WEB,
    UI_ICON_FEEDS,
    UI_ICON_SKETCH,
    UI_ICON_FOCUS,
    UI_ICON_PERSONAL,
    UI_ICON_AGENT,
    UI_ICON_SETTINGS,
    UI_ICON_FILES,
    UI_ICON_FONT,
    UI_ICON_HOME,
    UI_ICON_SELFTEST,
    UI_ICON_SLEEP,
    UI_ICON_SD,
    UI_ICON_POWER,
    UI_ICON_REFRESH,
    UI_ICON_TOUCH,
    UI_ICON_READING,
    UI_ICON_AXIS,
    UI_ICON_AXIS_LAB,
    UI_ICON_PMU,
    UI_ICON_KEY,
    UI_ICON_IOE,
    UI_ICON_COUNT,
} ui_icon_id_t;

/// 在以 (cx, cy) 为中心、边长 size 像素的方框内绘制极简 2px 线框图标。
/// Draw a minimalist 2px wireframe icon centered at (cx, cy) within a size x size box.
void ui_draw_icon(uint8_t* fb, int cx, int cy, int size, ui_icon_id_t id);

/// 绘制状态栏极简电池图标（外框 + 极柱 + 电量填充 + 充电标记）。
/// Draw a minimalist status-bar battery icon (body + cap + fill + charging mark).
void ui_draw_battery_icon(
    uint8_t* fb, int x, int y, int w, int h, int pct, bool charging
);

#ifdef __cplusplus
}
#endif
