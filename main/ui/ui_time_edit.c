/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：可复用的年月日时分与时区手动校准界面实现。
 * English: Implementation of the reusable manual date, time and timezone editor.
 */
#include "ui_time_edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "settings.h"
#include "time_sync.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TIME_EDIT_YEAR_MIN 2020
#define TIME_EDIT_YEAR_MAX 2099
#define TIME_EDIT_TZ_MIN (-720)
#define TIME_EDIT_TZ_MAX 840
#define TIME_EDIT_TZ_STEP 30

#define TIME_EDIT_PREVIEW_Y (UI_CONTENT_TOP + 42)
#define TIME_EDIT_ROW_Y0 268
#define TIME_EDIT_ROW_H 78
#define TIME_EDIT_ROW_GAP 10
#define TIME_EDIT_ROW_STRIDE (TIME_EDIT_ROW_H + TIME_EDIT_ROW_GAP)
#define TIME_EDIT_QUICK_Y 808
#define TIME_EDIT_QUICK_H 78
#define TIME_EDIT_HINT_Y 912

#define TIME_EDIT_MINUS_X 156
#define TIME_EDIT_BTN_W 120
#define TIME_EDIT_PLUS_X 524
#define TIME_EDIT_VAL_CX 400

enum {
    TE_ROW_YEAR = 0,
    TE_ROW_MONTH,
    TE_ROW_DAY,
    TE_ROW_HOUR,
    TE_ROW_MINUTE,
    TE_ROW_TZ,
    TE_ROW_COUNT,
};

enum {
    TE_HIT_NONE = -1,
    TE_HIT_MINUS_BASE = 0,
    TE_HIT_PLUS_BASE = 10,
    TE_HIT_QUICK_BASE = 20,
    TE_HIT_CANCEL = 30,
    TE_HIT_SAVE = 31,
};

static EpdRect row_minus_rect(int row) {
    return (EpdRect){
        .x = TIME_EDIT_MINUS_X,
        .y = TIME_EDIT_ROW_Y0 + row * TIME_EDIT_ROW_STRIDE,
        .width = TIME_EDIT_BTN_W,
        .height = TIME_EDIT_ROW_H,
    };
}

static EpdRect row_plus_rect(int row) {
    return (EpdRect){
        .x = TIME_EDIT_PLUS_X,
        .y = TIME_EDIT_ROW_Y0 + row * TIME_EDIT_ROW_STRIDE,
        .width = TIME_EDIT_BTN_W,
        .height = TIME_EDIT_ROW_H,
    };
}

static EpdRect quick_rect(int col) {
    return ui_row_rect(col, 4, TIME_EDIT_QUICK_Y, TIME_EDIT_QUICK_H);
}

static void clamp_day(ui_time_edit_t *ed) {
    int max_day = app_time_days_in_month(ed->year, ed->month);
    if (ed->day < 1) ed->day = 1;
    if (ed->day > max_day) ed->day = max_day;
}

static void fmt_tz(char *buf, size_t n, int tz_min) {
    char sign = tz_min < 0 ? '-' : '+';
    int abs_min = tz_min < 0 ? -tz_min : tz_min;
    snprintf(buf, n, "UTC%c%d:%02d", sign, abs_min / 60, abs_min % 60);
}

void ui_time_edit_open(ui_time_edit_t *ed) {
    if (!ed) return;
    memset(ed, 0, sizeof(*ed));
    struct tm local;
    int tz_min = app_settings_timezone_min();
    if (app_time_now_local(&local, &tz_min)) {
        ed->year = local.tm_year + 1900;
        ed->month = local.tm_mon + 1;
        ed->day = local.tm_mday;
        ed->hour = local.tm_hour;
        ed->minute = local.tm_min;
    } else {
        ed->year = 2026;
        ed->month = 1;
        ed->day = 1;
        ed->hour = 12;
        ed->minute = 0;
    }
    if (ed->year < TIME_EDIT_YEAR_MIN) ed->year = TIME_EDIT_YEAR_MIN;
    if (ed->year > TIME_EDIT_YEAR_MAX) ed->year = TIME_EDIT_YEAR_MAX;
    ed->tz_min = tz_min;
    clamp_day(ed);
}

static int hit_test(uint16_t x, uint16_t y, EpdRect *out_rect) {
    for (int r = 0; r < TE_ROW_COUNT; ++r) {
        EpdRect mr = row_minus_rect(r);
        if (ui_rect_hit(mr, x, y)) {
            if (out_rect) *out_rect = mr;
            return TE_HIT_MINUS_BASE + r;
        }
        EpdRect pr = row_plus_rect(r);
        if (ui_rect_hit(pr, x, y)) {
            if (out_rect) *out_rect = pr;
            return TE_HIT_PLUS_BASE + r;
        }
    }
    for (int c = 0; c < 4; ++c) {
        EpdRect qr = quick_rect(c);
        if (ui_rect_hit(qr, x, y)) {
            if (out_rect) *out_rect = qr;
            return TE_HIT_QUICK_BASE + c;
        }
    }
    EpdRect cancel = ui_bar_rect(0, 2);
    if (ui_rect_hit(cancel, x, y)) {
        if (out_rect) *out_rect = cancel;
        return TE_HIT_CANCEL;
    }
    EpdRect save = ui_bar_rect(1, 2);
    if (ui_rect_hit(save, x, y)) {
        if (out_rect) *out_rect = save;
        return TE_HIT_SAVE;
    }
    return TE_HIT_NONE;
}

bool ui_time_edit_hit_rect(uint16_t x, uint16_t y, EpdRect *out_rect) {
    return hit_test(x, y, out_rect) != TE_HIT_NONE;
}

void ui_time_edit_render(const ui_time_edit_t *ed, uint8_t *fb) {
    if (!ed || !fb) return;
    ui_clear_page(fb);
    ui_draw_header(fb, "校准日期与时间", "手动调整年月日、时刻与时区");

    char tz_buf[32];
    fmt_tz(tz_buf, sizeof(tz_buf), ed->tz_min);
    char preview[128];
    snprintf(preview, sizeof(preview), "%04d-%02d-%02d  %02d:%02d  %s",
             ed->year, ed->month, ed->day, ed->hour, ed->minute, tz_buf);
    ui_text_vc(fb, epd_rotated_display_width() / 2, TIME_EDIT_PREVIEW_Y,
               UI_PX_BODY, preview, EPD_DRAW_ALIGN_CENTER, false);

    static const char *const labels[TE_ROW_COUNT] = {
        "年", "月", "日", "时", "分", "时区",
    };
    char values[TE_ROW_COUNT][32];
    snprintf(values[TE_ROW_YEAR], sizeof(values[0]), "%04d", ed->year);
    snprintf(values[TE_ROW_MONTH], sizeof(values[0]), "%02d", ed->month);
    snprintf(values[TE_ROW_DAY], sizeof(values[0]), "%02d", ed->day);
    snprintf(values[TE_ROW_HOUR], sizeof(values[0]), "%02d", ed->hour);
    snprintf(values[TE_ROW_MINUTE], sizeof(values[0]), "%02d", ed->minute);
    snprintf(values[TE_ROW_TZ], sizeof(values[0]), "%s", tz_buf);

    for (int r = 0; r < TE_ROW_COUNT; ++r) {
        int cy = TIME_EDIT_ROW_Y0 + r * TIME_EDIT_ROW_STRIDE + TIME_EDIT_ROW_H / 2;
        ui_text_vc(fb, UI_MARGIN, cy, UI_PX_LABEL, labels[r], EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, row_minus_rect(r), "-", false);
        ui_text_vc(fb, TIME_EDIT_VAL_CX, cy,
                   r == TE_ROW_TZ ? UI_PX_VALUE_SM : UI_PX_VALUE,
                   values[r], EPD_DRAW_ALIGN_CENTER, false);
        ui_draw_button(fb, row_plus_rect(r), "+", false);
    }

    static const char *const quick_labels[4] = { "时 -6", "分 -10", "分 +10", "UTC+8" };
    for (int c = 0; c < 4; ++c) {
        EpdRect qr = quick_rect(c);
        ui_draw_choice_round_rect(fb, qr, UI_BTN_RADIUS, c == 3 && ed->tz_min == 480);
        ui_text_vc(fb, qr.x + qr.width / 2, qr.y + qr.height / 2,
                   UI_PX_CAPTION, quick_labels[c], EPD_DRAW_ALIGN_CENTER, false);
    }

    ui_text(fb, UI_MARGIN, TIME_EDIT_HINT_Y, UI_PX_CAPTION,
            ed->notice[0] ? ed->notice : "保存后将同步到主板 RTC，专注时钟与看板共用",
            EPD_DRAW_ALIGN_LEFT, false);

    ui_draw_button(fb, ui_bar_rect(0, 2), "返回", false);
    ui_draw_button(fb, ui_bar_rect(1, 2), "保存时间", true);
}

ui_time_edit_result_t ui_time_edit_tap(ui_time_edit_t *ed, uint16_t x, uint16_t y) {
    if (!ed) return UI_TIME_EDIT_NONE;
    int hit = hit_test(x, y, NULL);
    if (hit == TE_HIT_NONE) return UI_TIME_EDIT_NONE;
    ed->notice[0] = '\0';

    if (hit == TE_HIT_CANCEL) return UI_TIME_EDIT_CANCELLED;
    if (hit == TE_HIT_SAVE) {
        uint32_t utc = 0;
        if (!app_time_make_utc(ed->year, ed->month, ed->day, ed->hour, ed->minute, ed->tz_min, &utc)) {
            snprintf(ed->notice, sizeof(ed->notice), "时间超出支持范围（2020-2099）");
            return UI_TIME_EDIT_CHANGED;
        }
        app_settings_set_timezone_min((int16_t)ed->tz_min);
        bool synced = false;
        if (!app_time_set_utc(utc, &synced)) {
            snprintf(ed->notice, sizeof(ed->notice), "写入时间失败");
            return UI_TIME_EDIT_CHANGED;
        }
        ed->pmu_synced = synced;
        return UI_TIME_EDIT_SAVED;
    }

    if (hit >= TE_HIT_MINUS_BASE && hit < TE_HIT_MINUS_BASE + TE_ROW_COUNT) {
        int r = hit - TE_HIT_MINUS_BASE;
        switch (r) {
            case TE_ROW_YEAR:
                ed->year = ed->year > TIME_EDIT_YEAR_MIN ? ed->year - 1 : TIME_EDIT_YEAR_MAX;
                clamp_day(ed);
                break;
            case TE_ROW_MONTH:
                ed->month = ed->month > 1 ? ed->month - 1 : 12;
                clamp_day(ed);
                break;
            case TE_ROW_DAY: {
                int max_day = app_time_days_in_month(ed->year, ed->month);
                ed->day = ed->day > 1 ? ed->day - 1 : max_day;
                break;
            }
            case TE_ROW_HOUR:
                ed->hour = (ed->hour + 23) % 24;
                break;
            case TE_ROW_MINUTE:
                ed->minute = (ed->minute + 59) % 60;
                break;
            case TE_ROW_TZ:
                ed->tz_min -= TIME_EDIT_TZ_STEP;
                if (ed->tz_min < TIME_EDIT_TZ_MIN) ed->tz_min = TIME_EDIT_TZ_MAX;
                break;
            default: break;
        }
        return UI_TIME_EDIT_CHANGED;
    }

    if (hit >= TE_HIT_PLUS_BASE && hit < TE_HIT_PLUS_BASE + TE_ROW_COUNT) {
        int r = hit - TE_HIT_PLUS_BASE;
        switch (r) {
            case TE_ROW_YEAR:
                ed->year = ed->year < TIME_EDIT_YEAR_MAX ? ed->year + 1 : TIME_EDIT_YEAR_MIN;
                clamp_day(ed);
                break;
            case TE_ROW_MONTH:
                ed->month = ed->month < 12 ? ed->month + 1 : 1;
                clamp_day(ed);
                break;
            case TE_ROW_DAY: {
                int max_day = app_time_days_in_month(ed->year, ed->month);
                ed->day = ed->day < max_day ? ed->day + 1 : 1;
                break;
            }
            case TE_ROW_HOUR:
                ed->hour = (ed->hour + 1) % 24;
                break;
            case TE_ROW_MINUTE:
                ed->minute = (ed->minute + 1) % 60;
                break;
            case TE_ROW_TZ:
                ed->tz_min += TIME_EDIT_TZ_STEP;
                if (ed->tz_min > TIME_EDIT_TZ_MAX) ed->tz_min = TIME_EDIT_TZ_MIN;
                break;
            default: break;
        }
        return UI_TIME_EDIT_CHANGED;
    }

    if (hit >= TE_HIT_QUICK_BASE && hit < TE_HIT_QUICK_BASE + 4) {
        int q = hit - TE_HIT_QUICK_BASE;
        if (q == 0) ed->hour = (ed->hour + 18) % 24;
        else if (q == 1) ed->minute = (ed->minute + 50) % 60;
        else if (q == 2) ed->minute = (ed->minute + 10) % 60;
        else if (q == 3) ed->tz_min = 480;
        return UI_TIME_EDIT_CHANGED;
    }

    return UI_TIME_EDIT_NONE;
}
