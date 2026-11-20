/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：个人看板及锁屏主题设置。看板文本从 TF 卡读取，绘制回调只画缓存。
 * English: Personal dashboard and lock-screen choices. Dashboard text is read from SD; render only paints cached data.
 * 冻结：图片或摘录不可用时回退内置锁屏；不在锁屏路径启动 Wi-Fi。
 * Frozen: Fall back to the built-in lock face when an image or excerpt is unavailable; never start Wi-Fi from the lock path.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "display.h"
#include "reading_stats.h"
#include "read_pico_pmu.h"
#include "read_pico_sd.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_menu.h"

extern const app_desc_t app_book;

#define PERSONAL_NOTE_LINES 4
#define PERSONAL_NOTE_BYTES 96

static char s_note[PERSONAL_NOTE_LINES][PERSONAL_NOTE_BYTES];
static bool s_note_ready;
static pmu_snapshot_t s_pmu;
static int64_t s_next_refresh_ms;
static uint32_t s_total_read_seconds;
static int64_t s_note_retry_ms;
static bool s_note_sd_checked;
static char s_last_book[128];
static uint8_t s_last_percent;
static bool s_have_last_book;
static char s_time_sync_msg[64];

static void load_note(void) {
    memset(s_note, 0, sizeof(s_note));
    s_note_ready = false;
    read_pico_sd_info_t sd = {0};
    if (read_pico_sd_get_info(&sd) != ESP_OK || !sd.mounted) return;
    FILE* f = fopen("/sdcard/dashboard.txt", "r");
    if (!f) return;
    for (int i = 0; i < PERSONAL_NOTE_LINES && fgets(s_note[i], sizeof(s_note[i]), f); ++i) {
        size_t n = strlen(s_note[i]);
        while (n && (s_note[i][n - 1] == '\n' || s_note[i][n - 1] == '\r')) s_note[i][--n] = '\0';
        s_note_ready |= n != 0;
    }
    fclose(f);
}

static void start_sd_probe_if_needed(void) {
    read_pico_sd_info_t sd = {0};
    esp_err_t err = read_pico_sd_get_info(&sd);
    if (!sd.mounted && err != ESP_ERR_NOT_FINISHED) (void)read_pico_sd_start_probe();
}

static void draw_card(uint8_t* fb, EpdRect r, const char* label) {
    ui_draw_round_rect(fb, r, UI_BTN_RADIUS, UI_GRAY_BLACK);
    ui_text(fb, r.x + UI_PAD, r.y + 12, UI_PX_CAPTION, label, EPD_DRAW_ALIGN_LEFT, false);
}

static void fit_line(char* text, int px, int width) {
    while (*text && ttf_text_width_px(px, text) > width) {
        size_t n = strlen(text) - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = '\0';
    }
}

static void draw_dashboard(uint8_t* fb) {
    ui_clear_page(fb);
    ui_draw_header(fb, "我的看板", "个人信息与阅读状态");
    uint8_t items = app_settings_dashboard_items();
    int y = UI_CONTENT_TOP;
    char line[160];
    if (items & APP_DASH_TIME) {
        EpdRect card = {UI_MARGIN, y, ui_content_width(), 142};
        draw_card(fb, card, "日期与时间");
        if (s_pmu.time_ok && s_pmu.unix_sec) {
            time_t raw = (time_t)s_pmu.unix_sec + 8 * 3600;
            struct tm tm_local;
            gmtime_r(&raw, &tm_local);
            strftime(line, sizeof(line), "%H:%M", &tm_local);
            ui_text_vc(fb, card.x + UI_PAD, card.y + 82, UI_PX_HERO, line, EPD_DRAW_ALIGN_LEFT, false);
            strftime(line, sizeof(line), "%Y-%m-%d · UTC+8", &tm_local);
            ui_text_vc(fb, card.x + card.width - UI_PAD, card.y + 92, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_RIGHT, false);
        } else {
            ui_text(fb, card.x + UI_PAD, card.y + 64, UI_PX_BODY, "尚未同步时间", EPD_DRAW_ALIGN_LEFT, false);
        }
        y += card.height + UI_GAP;
    }
    if (items & APP_DASH_BATTERY) {
        EpdRect card = {UI_MARGIN, y, ui_content_width(), 112};
        draw_card(fb, card, "设备状态");
        if (s_pmu.status_ok) {
            snprintf(line, sizeof(line), "%u%%　%u.%03u V", s_pmu.soc_permille / 10,
                     s_pmu.battery_mv / 1000, s_pmu.battery_mv % 1000);
        } else snprintf(line, sizeof(line), "电量暂不可用");
        ui_text_vc(fb, card.x + UI_PAD, card.y + 76, UI_PX_VALUE, line, EPD_DRAW_ALIGN_LEFT, false);
        y += card.height + UI_GAP;
    }
    if (items & APP_DASH_LAST_BOOK) {
        EpdRect card = {UI_MARGIN, y, ui_content_width(), 154};
        draw_card(fb, card, "最近阅读");
        if (s_have_last_book) {
            char title[sizeof(s_last_book)];
            strlcpy(title, s_last_book, sizeof(title));
            fit_line(title, UI_PX_BODY, card.width - 2 * UI_PAD);
            ui_text(fb, card.x + UI_PAD, card.y + 58, UI_PX_BODY, title, EPD_DRAW_ALIGN_LEFT, false);
            snprintf(line, sizeof(line), "读到 %u%%　·　累计 %lu 分钟", s_last_percent,
                     (unsigned long)(s_total_read_seconds / 60));
        } else {
            snprintf(line, sizeof(line), "打开书架开始阅读　·　累计 %lu 分钟",
                     (unsigned long)(s_total_read_seconds / 60));
        }
        ui_text(fb, card.x + UI_PAD, card.y + 108, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
        y += card.height + UI_GAP;
    }
    if (items & APP_DASH_NOTE) {
        EpdRect card = {UI_MARGIN, y, ui_content_width(), 192};
        draw_card(fb, card, "我的信息");
        if (s_note_ready) {
            int ny = card.y + 52;
            for (int i = 0; i < PERSONAL_NOTE_LINES; ++i) {
                if (!s_note[i][0]) continue;
                char row[PERSONAL_NOTE_BYTES];
                strlcpy(row, s_note[i], sizeof(row));
                fit_line(row, UI_PX_CAPTION, card.width - 2 * UI_PAD);
                ui_text(fb, card.x + UI_PAD, ny, UI_PX_CAPTION, row, EPD_DRAW_ALIGN_LEFT, false);
                ny += 32;
            }
        } else {
            ui_text(fb, card.x + UI_PAD, card.y + 66, UI_PX_CAPTION,
                    "在 TF 卡根目录创建 dashboard.txt 添加文字", EPD_DRAW_ALIGN_LEFT, false);
        }
    }
    ui_draw_button(fb, ui_bar_rect(0, 2), "配置看板", false);
    ui_draw_button(fb, ui_bar_rect(1, 2), "继续阅读", false);
    ui_draw_menu_handle(fb, false);
}

static void draw_settings(uint8_t* fb) {
    static const struct { uint8_t bit; const char* label; } cards[] = {
        {APP_DASH_TIME, "日期时间"}, {APP_DASH_BATTERY, "电量状态"},
        {APP_DASH_LAST_BOOK, "最近阅读"}, {APP_DASH_NOTE, "个人信息"},
    };
    ui_clear_page(fb);
    ui_draw_header(fb, "看板与屏保", "点按卡片开关显示内容");
    uint8_t selected = app_settings_dashboard_items();
    for (int i = 0; i < 4; ++i) {
        EpdRect r = ui_grid_rect(i % 2, 2, i / 2, 220, UI_BTN_H);
        ui_draw_choice_round_rect(fb, r, UI_BTN_RADIUS, (selected & cards[i].bit) != 0);
        ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, UI_PX_BTN,
                   cards[i].label, EPD_DRAW_ALIGN_CENTER, false);
    }
    ui_text(fb, UI_MARGIN, 440, UI_PX_SECTION, "锁屏内容", EPD_DRAW_ALIGN_LEFT, false);
    static const struct { app_saver_style_t style; const char* label; } savers[] = {
        {APP_SAVER_BUILTIN, "默认图案"}, {APP_SAVER_QUOTE, "最近摘录"}, {APP_SAVER_IMAGE, "TF 卡图片"},
    };
    for (int i = 0; i < 3; ++i) {
        EpdRect r = ui_row_rect(i, 3, 500, UI_BTN_H);
        ui_draw_choice_round_rect(fb, r, UI_BTN_RADIUS, app_settings_saver_style() == savers[i].style);
        ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, UI_PX_CAPTION,
                   savers[i].label, EPD_DRAW_ALIGN_CENTER, false);
    }
    ui_text(fb, UI_MARGIN, 660, UI_PX_CAPTION,
            "看板文字：TF 卡根目录 dashboard.txt（最多 4 行）", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, 696, UI_PX_CAPTION,
            "图片优先 agent_screensaver，其次 screensaver（JPG/PNG）",
            EPD_DRAW_ALIGN_LEFT, false);
    if (s_time_sync_msg[0])
        ui_text(fb, UI_MARGIN, 732, UI_PX_CAPTION, s_time_sync_msg, EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_button(fb, ui_row_rect(0, 1, 570, UI_BTN_H), "同步设备时间", false);
    ui_draw_button(fb, ui_bar_rect(0, 1), "返回看板", false);
    ui_draw_menu_handle(fb, false);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    if (ctx->leaf == 1) draw_settings(fb);
    else draw_dashboard(fb);
}

static void on_enter(app_ctx_t* ctx) {
    ctx->leaf = 0;
    (void)read_pico_pmu_refresh();
    s_pmu = *read_pico_pmu_get();
    load_note();
    s_total_read_seconds = reading_stats_total_seconds();
    s_have_last_book = reading_stats_get_last_book(s_last_book, sizeof(s_last_book), &s_last_percent);
    s_note_sd_checked = false;
    s_note_retry_ms = ctx->now_ms;
    start_sd_probe_if_needed();
    s_next_refresh_ms = ctx->now_ms + 60000;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (ctx->leaf != 0) return APP_REDRAW_NONE;
    if (!s_note_sd_checked && ctx->now_ms >= s_note_retry_ms) {
        read_pico_sd_info_t sd = {0};
        esp_err_t err = read_pico_sd_get_info(&sd);
        if (sd.mounted) {
            load_note();
            s_note_sd_checked = true;
            return APP_REDRAW_PAGE;
        }
        if (err != ESP_ERR_NOT_FINISHED) (void)read_pico_sd_start_probe();
        s_note_retry_ms = ctx->now_ms + 2000;
    }
    if (ctx->now_ms < s_next_refresh_ms) return APP_REDRAW_NONE;
    s_next_refresh_ms = ctx->now_ms + 60000;
    (void)read_pico_pmu_refresh();
    s_pmu = *read_pico_pmu_get();
    s_total_read_seconds = reading_stats_total_seconds();
    s_have_last_book = reading_stats_get_last_book(s_last_book, sizeof(s_last_book), &s_last_percent);
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    if (ctx->leaf == 0) {
        const int bar = ui_bar_hit(touch->x, touch->y, 2);
        if (bar == 0) { ctx->leaf = 1; return APP_REDRAW_PAGE; }
        if (bar == 1) { ctx->request_app = &app_book; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (ui_bar_hit(touch->x, touch->y, 1) == 0) { ctx->leaf = 0; return APP_REDRAW_PAGE; }
    if (ui_rect_hit(ui_row_rect(0, 1, 570, UI_BTN_H), touch->x, touch->y)) {
        time_t now = time(NULL);
        if (now < 946684800) {
            snprintf(s_time_sync_msg, sizeof(s_time_sync_msg), "设备时间不可用，请先校准主机时间");
        } else {
            uint32_t unix_sec = (uint32_t)now;
            uint8_t buf[4] = {
                (uint8_t)unix_sec, (uint8_t)(unix_sec >> 8),
                (uint8_t)(unix_sec >> 16), (uint8_t)(unix_sec >> 24),
            };
            esp_err_t err = read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, buf, sizeof(buf));
            uint16_t status = read_pico_pmu_get()->last_op_status;
            bool synced = err == ESP_OK
                && (status == PMU_STATUS_OK || status == PMU_STATUS_ACCEPTED);
            if (!synced) {
                snprintf(s_time_sync_msg, sizeof(s_time_sync_msg), "同步失败：请检查 PMU 连接");
                return APP_REDRAW_PAGE;
            }
            if (err == ESP_OK) err = read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
            status = read_pico_pmu_get()->last_op_status;
            synced = err == ESP_OK
                && (status == PMU_STATUS_OK || status == PMU_STATUS_ACCEPTED);
            snprintf(s_time_sync_msg, sizeof(s_time_sync_msg), "%s",
                     synced && read_pico_pmu_get()->time_synced
                         ? "设备时间已同步" : "同步失败：请检查 PMU 连接");
            s_pmu = *read_pico_pmu_get();
        }
        return APP_REDRAW_PAGE;
    }
    uint8_t items = app_settings_dashboard_items();
    static const uint8_t bits[] = {APP_DASH_TIME, APP_DASH_BATTERY, APP_DASH_LAST_BOOK, APP_DASH_NOTE};
    for (int i = 0; i < 4; ++i) {
        if (ui_rect_hit(ui_grid_rect(i % 2, 2, i / 2, 220, UI_BTN_H), touch->x, touch->y)) {
            app_settings_set_dashboard_items(items ^ bits[i]);
            return APP_REDRAW_PAGE;
        }
    }
    static const app_saver_style_t styles[] = {APP_SAVER_BUILTIN, APP_SAVER_QUOTE, APP_SAVER_IMAGE};
    for (int i = 0; i < 3; ++i) {
        if (ui_rect_hit(ui_row_rect(i, 3, 500, UI_BTN_H), touch->x, touch->y)) {
            app_settings_set_saver_style(styles[i]);
            return APP_REDRAW_PAGE;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    if (ctx->leaf == 0) {
        ctx->leaf = 1;
        return APP_REDRAW_PAGE;
    }
    ctx->leaf = 0;
    return APP_REDRAW_PAGE;
}

const app_desc_t app_personal = {
    .title = "我的看板",
    .detail = "个人信息与屏保",
    .render = render,
    .on_enter = on_enter,
    .on_touch = on_touch,
    .on_key = on_key,
    .on_tick = on_tick,
};
