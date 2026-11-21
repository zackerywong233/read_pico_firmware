/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：可复用的年月日时分与时区手动校准界面组件。
 * English: Reusable manual date, time and timezone editor UI component.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "epdiy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_TIME_EDIT_NONE = 0,      ///< 无命中 / No hit
    UI_TIME_EDIT_CHANGED,       ///< 数值已调整，需重绘 / Value changed, redraw
    UI_TIME_EDIT_SAVED,         ///< 已保存并同步时间 / Saved and synced
    UI_TIME_EDIT_CANCELLED,     ///< 取消并返回 / Cancelled and returned
} ui_time_edit_result_t;

typedef struct {
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int tz_min;
    bool pmu_synced;
    char notice[80];
} ui_time_edit_t;

/// 从当前统一时钟初始化编辑器（未校准时默认 2026-01-01 12:00）。
/// Initialize the editor from the unified clock (defaults to 2026-01-01 12:00 when unset).
void ui_time_edit_open(ui_time_edit_t *ed);

/// 绘制手动时间校准页面（含底栏返回/保存与菜单把手）。
/// Render the manual date/time editor page (including bottom bar and menu handle).
void ui_time_edit_render(const ui_time_edit_t *ed, uint8_t *fb);

/// 查询坐标命中的按钮矩形，供手势按下态局部刷新使用；未命中返回 false。
/// Query the button rectangle hit at (x, y) for local press feedback; false if none.
bool ui_time_edit_hit_rect(uint16_t x, uint16_t y, EpdRect *out_rect);

/// 处理一次点按并更新状态；保存时写入全局时区、主机时钟与 PMU RTC。
/// Handle a tap and update state; Save writes timezone, host clock and PMU RTC.
ui_time_edit_result_t ui_time_edit_tap(ui_time_edit_t *ed, uint16_t x, uint16_t y);

#ifdef __cplusplus
}
#endif
