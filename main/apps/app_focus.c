/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：桌面时钟与前台番茄钟；使用统一设备时钟并支持快捷校准，蜂鸣提示遵守全局静音。
 * English: Desk clock and foreground pomodoro timer; uses the unified device clock with quick manual setup and mute-aware alerts.
 *
 * 冻结：倒计时仅在页面活动时运行，不在后台唤醒设备。
 * Frozen: The timer runs only while this page is active and never wakes the device.
 */
#include "app.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_timer.h"
#include "nvs.h"
#include "read_pico_buzzer.h"
#include "read_pico_pmu.h"
#include "reading_stats.h"
#include "settings.h"
#include "time_sync.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_time_edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define FOCUS_CLOCK_CARD_Y (UI_CONTENT_TOP + 56)
#define FOCUS_CLOCK_CARD_H 300

static bool s_timer;
static bool s_break;
static bool s_time_edit;
static int s_minutes = 25;
static int64_t s_end;
static int s_done;
static int64_t s_last_draw;
static int64_t s_last_check_ms;
static int s_last_shown_min = -1;
static ui_time_edit_t s_time_editor;

static void done_load(void) {
    nvs_handle_t h;
    if (nvs_open("rp_focus", NVS_READONLY, &h) == ESP_OK) {
        uint32_t n = 0;
        if (nvs_get_u32(h, "done", &n) == ESP_OK) s_done = (int)n;
        nvs_close(h);
    }
}

static void done_save(void) {
    nvs_handle_t h;
    if (nvs_open("rp_focus", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "done", (uint32_t)s_done);
        nvs_commit(h);
        nvs_close(h);
    }
}

static EpdRect clock_card_rect(void) {
    return (EpdRect){
        .x = UI_MARGIN,
        .y = FOCUS_CLOCK_CARD_Y,
        .width = ui_content_width(),
        .height = FOCUS_CLOCK_CARD_H,
    };
}

static int minute_stamp(const struct tm *tmv) {
    return (tmv->tm_year * 400 + tmv->tm_yday) * 1440 + tmv->tm_hour * 60 + tmv->tm_min;
}

static void cycle_minutes(void) {
    s_minutes = s_minutes == 15 ? 25 : (s_minutes == 25 ? 45 : 15);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    if (s_time_edit) {
        ui_time_edit_render(&s_time_editor, fb);
        return;
    }

    ui_clear_page(fb);
    ui_draw_header(fb, "专注时钟 Focus", "时钟 / 番茄钟 Clock / Pomodoro");
    const int cx = epd_rotated_display_width() / 2;

    if (!s_timer) {
        EpdRect card = clock_card_rect();
        ui_draw_round_rect(fb, card, UI_BTN_RADIUS, UI_GRAY_BLACK);
        ui_text(fb, card.x + UI_PAD, card.y + 16, UI_PX_CAPTION,
                "当前日期与时间", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, card.x + card.width - UI_PAD, card.y + 16, UI_PX_CAPTION,
                "点按调时间 ›", EPD_DRAW_ALIGN_RIGHT, false);

        struct tm tmv;
        int tz_min = app_settings_timezone_min();
        char timebuf[16];
        char datebuf[96];
        if (app_time_now_local(&tmv, &tz_min)) {
            s_last_shown_min = minute_stamp(&tmv);
            strftime(timebuf, sizeof(timebuf), "%H:%M", &tmv);
            char date[32];
            strftime(date, sizeof(date), "%Y-%m-%d", &tmv);
            char sign = tz_min < 0 ? '-' : '+';
            int abs_tz = tz_min < 0 ? -tz_min : tz_min;
            snprintf(datebuf, sizeof(datebuf), "%s  ·  UTC%c%d:%02d",
                     date, sign, abs_tz / 60, abs_tz % 60);
        } else {
            s_last_shown_min = -1;
            snprintf(timebuf, sizeof(timebuf), "--:--");
            snprintf(datebuf, sizeof(datebuf), "尚未校准时间，点按此处手动设置");
        }
        ui_text_vc(fb, cx, card.y + 146, UI_PX_HERO, timebuf, EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, cx, card.y + 246, UI_PX_BODY, datebuf, EPD_DRAW_ALIGN_CENTER, false);

        const pmu_snapshot_t *p = read_pico_pmu_get();
        char line[128];
        snprintf(line, sizeof(line), "电量 %u%%  ·  累计阅读 %lu 分钟  ·  已完成 %d 次",
                 p && p->status_ok ? (unsigned)(p->soc_permille / 10) : 0u,
                 (unsigned long)(reading_stats_total_seconds() / 60),
                 s_done);
        ui_text_vc(fb, cx, card.y + card.height + 64, UI_PX_CAPTION, line,
                   EPD_DRAW_ALIGN_CENTER, false);

        char dur_label[32];
        snprintf(dur_label, sizeof(dur_label), "%d 分钟", s_minutes);
        ui_draw_button(fb, ui_bar_rect(0, 3), "开始专注", true);
        ui_draw_button(fb, ui_bar_rect(1, 3), dur_label, false);
        ui_draw_button(fb, ui_bar_rect(2, 3), "调时间", false);
    } else {
        int64_t left = s_end - esp_timer_get_time() / 1000000;
        if (left < 0) left = 0;
        char value[96];
        snprintf(value, sizeof(value), "%02lld:%02lld",
                 (long long)(left / 60), (long long)(left % 60));
        ui_text_vc(fb, cx, UI_CONTENT_TOP + 230, UI_PX_HERO, value,
                   EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, cx, UI_CONTENT_TOP + 330, UI_PX_BODY,
                   s_break ? "休息 Rest" : "专注 Focus",
                   EPD_DRAW_ALIGN_CENTER, false);
        snprintf(value, sizeof(value), "时长 %d 分钟  ·  已完成 %d 次", s_minutes, s_done);
        ui_text_vc(fb, cx, UI_CONTENT_TOP + 420, UI_PX_CAPTION, value,
                   EPD_DRAW_ALIGN_CENTER, false);
        ui_draw_button(fb, ui_bar_rect(0, 2), "停止专注", false);
        ui_draw_button(fb, ui_bar_rect(1, 2), "完成 +1", false);
    }

    ui_draw_menu_handle(fb, false);
}

static app_redraw_t touch(app_ctx_t *ctx, const cst836u_touch_t *t) {
    if (s_time_edit) {
        ui_time_edit_result_t res = ui_time_edit_tap(&s_time_editor, t->x, t->y);
        if (res == UI_TIME_EDIT_SAVED || res == UI_TIME_EDIT_CANCELLED) {
            s_time_edit = false;
            s_last_draw = ctx->now_ms;
            return APP_REDRAW_FULL;
        }
        return res == UI_TIME_EDIT_CHANGED ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }

    if (!s_timer) {
        int bar = ui_bar_hit(t->x, t->y, 3);
        if (bar == 0) {
            s_timer = true;
            s_break = false;
            s_end = esp_timer_get_time() / 1000000 + s_minutes * 60;
            s_last_draw = ctx->now_ms;
            return APP_REDRAW_FULL;
        }
        if (bar == 1) {
            cycle_minutes();
            return APP_REDRAW_PAGE;
        }
        if (bar == 2 || ui_rect_hit(clock_card_rect(), t->x, t->y)) {
            ui_time_edit_open(&s_time_editor);
            s_time_edit = true;
            return APP_REDRAW_FULL;
        }
        return APP_REDRAW_NONE;
    }

    int bar = ui_bar_hit(t->x, t->y, 2);
    if (bar == 0) {
        s_timer = false;
        s_break = false;
        s_last_draw = ctx->now_ms;
        return APP_REDRAW_FULL;
    }
    if (bar == 1) {
        s_timer = false;
        s_break = false;
        s_done++;
        done_save();
        s_last_draw = ctx->now_ms;
        return APP_REDRAW_FULL;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t key(app_ctx_t *ctx, int k) {
    (void)ctx;
    if (k != UI_KEY_1) return APP_REDRAW_NONE;
    if (s_time_edit) {
        s_time_edit = false;
        return APP_REDRAW_FULL;
    }
    if (!s_timer) {
        cycle_minutes();
        return APP_REDRAW_PAGE;
    }
    s_timer = false;
    s_break = false;
    return APP_REDRAW_FULL;
}

static app_redraw_t tick(app_ctx_t *ctx) {
    if (s_time_edit) return APP_REDRAW_NONE;
    if (!s_timer) {
        if (ctx->now_ms - s_last_check_ms < 1000) return APP_REDRAW_NONE;
        s_last_check_ms = ctx->now_ms;
        struct tm tmv;
        int cur_min = app_time_now_local(&tmv, NULL) ? minute_stamp(&tmv) : -1;
        if (cur_min != s_last_shown_min) {
            s_last_draw = ctx->now_ms;
            render(ctx, ctx->fb);
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }

    int64_t now = esp_timer_get_time() / 1000000;
    if (now >= s_end) {
        if (!s_break) {
            s_break = true;
            s_end = now + 5 * 60;
            s_done++;
            done_save();
            if (!app_settings_muted()) (void)read_pico_buzzer_tone(880, 120);
        } else {
            s_break = false;
            s_timer = false;
        }
        s_last_draw = ctx->now_ms;
        return APP_REDRAW_FULL;
    }
    if (ctx->now_ms - s_last_draw >= 60000) {
        s_last_draw = ctx->now_ms;
        render(ctx, ctx->fb);
        return APP_REDRAW_AREA;
    }
    return APP_REDRAW_NONE;
}

static EpdRect area(app_ctx_t *ctx) {
    (void)ctx;
    if (!s_timer) return clock_card_rect();
    int w = epd_rotated_display_width();
    return (EpdRect){ .x = w / 2 - 230, .y = UI_CONTENT_TOP + 105, .width = 460, .height = 240 };
}

static void enter(app_ctx_t *ctx) {
    (void)ctx;
    done_load();
    s_time_edit = false;
    s_last_draw = ctx->now_ms;
    s_last_check_ms = ctx->now_ms;
    s_last_shown_min = -1;
    (void)read_pico_pmu_refresh();
    app_time_sync_from_pmu();
}

const app_desc_t app_focus = {
    .title = "专注时钟 Focus",
    .detail = "桌面时钟与番茄钟 Clock & Pomodoro",
    .blocks_idle_sleep = true,
    .render = render,
    .on_enter = enter,
    .on_touch = touch,
    .on_key = key,
    .on_tick = tick,
    .area_hint = area,
};
