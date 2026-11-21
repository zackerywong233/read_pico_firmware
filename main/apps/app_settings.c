/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 一级设置入口，集中管理网络、阅读、睡眠、电源与提示选项。
 * Root settings page for network, reading, sleep, power and notification options.
 * 冻结：页面只调用各功能公开接口；PMU 配置由 settings 模块提交。
 * Frozen: Use public feature APIs; the settings module submits PMU configuration.
 */
#include "app.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include "ambient_sync.h"
#include "ota_update.h"
#include "app_registry.h"
#include "read_pico_transfer.h"
#include "read_pico_pmu.h"
#include "settings.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

extern const app_desc_t app_transfer;
extern const app_desc_t app_sleep;
extern const app_desc_t app_home;
extern const app_desc_t app_system_tools;

static char s_networks[5][33];
static size_t s_network_count;
static int s_pressed = -1;
static bool s_update_view;
static char s_update_notice[96];
static EpdRect s_update_area;
static ota_update_state_t s_ota_last_state;
static unsigned s_ota_last_progress;

static EpdRect update_control_rect(int col) {
    return ui_row_rect(col, 3, UI_CONTENT_TOP + 260, UI_BTN_H);
}

static EpdRect settings_row_rect(int row) {
    return ui_row_rect(0, 1, UI_CONTENT_TOP + row * (UI_BTN_H + UI_GAP), UI_BTN_H);
}

static const char *refresh_name(uint8_t value) {
    static const char *const names[] = {"纯净", "均衡", "极速"};
    return names[value % 3];
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    ui_clear_page(fb);
    if (s_update_view) {
        ota_update_status_t status;
        ota_update_get_status(&status);
        ui_draw_header(fb, "系统更新", "GitHub Releases · Wi-Fi OTA");
        char line[120];
        snprintf(line, sizeof(line), "当前版本 %s  ·  最新版本 %s",
                 status.current_version[0] ? status.current_version : "未知",
                 status.latest_version[0] ? status.latest_version : "未检查");
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 90, UI_PX_BODY, line, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 150, UI_PX_BODY,
                s_update_notice[0] ? s_update_notice : (status.message[0] ? status.message : "请连接已有 Wi-Fi 后检查更新"),
                EPD_DRAW_ALIGN_LEFT, false);
        if (status.state == OTA_UPDATE_DOWNLOADING) {
            snprintf(line, sizeof(line), "下载与写入进度：%u%%", status.progress_percent);
            ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 205, UI_PX_BODY, line, EPD_DRAW_ALIGN_LEFT, false);
        }
        ui_draw_button(fb, update_control_rect(0), "检查更新", s_pressed == 0);
        ui_draw_button(fb, update_control_rect(1), "安装更新", s_pressed == 1);
        ui_draw_button(fb, update_control_rect(2), "取消 / 返回", s_pressed == 2);
        ui_draw_menu_handle(fb, false);
        return;
    }
    ui_draw_header(fb, "设置 Settings", "网络、阅读、睡眠与设备选项");
    char line[160];
    snprintf(line, sizeof(line), "网络与时间  ·  Wi-Fi %u  ·  UTC%+d:%02d", (unsigned)s_network_count,
             app_settings_timezone_min() / 60, abs(app_settings_timezone_min()) % 60);
    if (s_network_count) snprintf(line + strlen(line), sizeof(line) - strlen(line), "  ·  %s", s_networks[0]);
    ui_draw_button(fb, settings_row_rect(0), line, s_pressed == 0);
    snprintf(line, sizeof(line), "显示与字体  ·  %u px  ·  刷新 %s  ·  字重 %u",
             app_settings_book_px(), refresh_name(app_settings_book_refresh()), app_settings_book_weight());
    ui_draw_button(fb, settings_row_rect(1), line, s_pressed == 1);
    snprintf(line, sizeof(line), "睡眠与屏保  ·  %u 秒  ·  锁屏 %u",
             app_settings_auto_sleep_sec(), (unsigned)app_settings_saver_style());
    ui_draw_button(fb, settings_row_rect(2), line, s_pressed == 2);
    snprintf(line, sizeof(line), "电源与提示  ·  充电灯 %s  ·  低电 %u%%  ·  静音 %s",
             app_settings_charge_led() ? "开" : "关", app_settings_low_soc_notify_percent(),
             app_settings_muted() ? "开" : "关");
    ui_draw_button(fb, settings_row_rect(3), line, s_pressed == 3);
    ui_draw_button(fb, settings_row_rect(4), "设备信息与系统工具", s_pressed == 4);
    app_ambient_settings_t ambient;
    app_settings_ambient_get(&ambient);
    bool has_result = false, last_success = false;
    uint32_t last_ts = 0;
    app_settings_ambient_result_get(&has_result, &last_success, &last_ts);
    snprintf(line, sizeof(line), "定时看板  ·  %s  ·  每 %u 分钟  ·  上次%s", ambient.enabled ? "开启" : "关闭",
             ambient.interval_min, has_result ? (last_success ? "成功" : "失败") : "暂无");
    ui_draw_button(fb, settings_row_rect(5), line, s_pressed == 5);
    ui_draw_button(fb, settings_row_rect(6), "系统更新  ·  Wi-Fi OTA", s_pressed == 6);
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    time_t now = pmu && pmu->time_ok && pmu->unix_sec ? (time_t)pmu->unix_sec : time(NULL);
    if (ambient.enabled && now >= 946684800) {
        time_t local_now = now + (time_t)app_settings_timezone_min() * 60;
        struct tm local;
        if (gmtime_r(&local_now, &local)) {
            uint32_t seconds = (uint32_t)local.tm_hour * 3600U +
                (uint32_t)local.tm_min * 60U + (uint32_t)local.tm_sec;
            uint32_t delay = ambient_sync_next_delay(seconds, (uint32_t)ambient.interval_min * 60U,
                                                      ambient.quiet_start_h, ambient.quiet_end_h);
            time_t target = local_now + delay;
            if (gmtime_r(&target, &local)) strftime(line, sizeof(line), "下一次计划唤醒：%m-%d %H:%M", &local);
            else snprintf(line, sizeof(line), "下一次计划唤醒：时间不可用");
        }
    } else snprintf(line, sizeof(line), "下一次计划唤醒：未启用或时间未同步");
    ui_text(fb, UI_MARGIN, 850, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_menu_handle(fb, false);
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    (void)read_pico_transfer_get_saved_wifi_list(s_networks, &s_network_count);
    s_pressed = -1;
    s_update_view = false;
    s_update_notice[0] = 0;
}

static int hit_xy(uint16_t x, uint16_t y) {
    if (y < UI_CONTENT_TOP || y >= UI_CONTENT_TOP + 7 * (UI_BTN_H + UI_GAP)) return -1;
    int row_h = UI_BTN_H + UI_GAP;
    int row = (y - UI_CONTENT_TOP) / row_h;
    if (row >= 0 && row < 7 && ui_rect_hit(settings_row_rect(row), x, y)) return row;
    return -1;
}

static int update_hit_xy(uint16_t x, uint16_t y) {
    for (int i = 0; i < 3; ++i) {
        if (ui_rect_hit(update_control_rect(i), x, y)) return i;
    }
    return -1;
}

// 主循环仅在按下沿派发 on_touch，抬手提交必须走 on_gesture。
// The loop only dispatches on_touch on press; release-to-commit uses on_gesture.
static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    if (!event) return APP_REDRAW_NONE;
    if (event->type == UI_GESTURE_CANCEL) {
        if (s_pressed < 0) return APP_REDRAW_NONE;
        s_pressed = -1;
        return APP_REDRAW_PAGE;
    }
    if (s_update_view) {
        int col = update_hit_xy(event->x, event->y);
        if (event->type == UI_GESTURE_PRESS) {
            s_pressed = col;
            return col >= 0 ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
        }
        if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        int was_pressed = s_pressed;
        s_pressed = -1;
        if (col < 0) return was_pressed >= 0 ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
        if (col == 0) { s_update_notice[0] = 0; (void)ota_update_check_start(); }
        else if (col == 1) {
            (void)read_pico_pmu_refresh();
            const pmu_snapshot_t *pmu = read_pico_pmu_get();
            bool charging = pmu && pmu->charge_state == PMU_CHARGE_CHARGING;
            if (!pmu || !ota_update_battery_allowed(pmu->soc_permille, charging)) {
                snprintf(s_update_notice, sizeof(s_update_notice), "电量低于 20%%，请连接充电器后升级");
                return APP_REDRAW_PAGE;
            }
            s_update_notice[0] = 0;
            ota_update_status_t status; ota_update_get_status(&status);
            if (status.state == OTA_UPDATE_AVAILABLE) (void)ota_update_install_start();
        } else {
            ota_update_status_t status; ota_update_get_status(&status);
            if (status.state == OTA_UPDATE_DOWNLOADING || status.state == OTA_UPDATE_CHECKING) ota_update_cancel();
            else s_update_view = false;
        }
        return APP_REDRAW_PAGE;
    }
    if (event->type == UI_GESTURE_PRESS) {
        s_pressed = hit_xy(event->x, event->y);
        return s_pressed >= 0 ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    int was_pressed = s_pressed;
    s_pressed = -1;
    int row = hit_xy(event->x, event->y);
    if (row < 0) return was_pressed >= 0 ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    switch (row) {
        case 0:
            if (event->x < UI_MARGIN + ui_content_width() / 2) ctx->request_app = &app_transfer;
            else {
                int next = app_settings_timezone_min() + 30;
                if (next > 840) next = -720;
                app_settings_set_timezone_min((int16_t)next);
            }
            break;
        case 1:
            if (event->x < UI_MARGIN + ui_content_width() / 3) ctx->request_app = app_font_page();
            else if (event->x < UI_MARGIN + ui_content_width() * 2 / 3)
                app_settings_set_book_px((uint8_t)(app_settings_book_px() >= 72 ? 36 : app_settings_book_px() + 4));
            else {
                app_settings_set_book_refresh((app_settings_book_refresh() + 1) % 3);
                uint16_t weight = app_settings_book_weight();
                app_settings_set_book_weight(weight >= 800 ? 0 : weight ? weight + 100 : 400);
            }
            break;
        case 2:
            if (event->x < UI_MARGIN + ui_content_width() / 2) ctx->request_app = &app_sleep;
            else app_settings_set_saver_style((app_saver_style_t)((app_settings_saver_style() + 1) % 3));
            break;
        case 3:
            if (event->x < UI_MARGIN + ui_content_width() / 3) app_settings_set_charge_led(!app_settings_charge_led());
            else if (event->x < UI_MARGIN + ui_content_width() * 2 / 3) {
                uint8_t n = app_settings_low_soc_notify_percent();
                app_settings_set_low_soc_notify_percent(n >= 30 ? 0 : n + 5);
            } else app_settings_set_muted(!app_settings_muted());
            break;
        case 4:
            ctx->request_app = event->x < UI_MARGIN + ui_content_width() / 2 ? &app_home : &app_system_tools;
            break;
        case 5:
            ctx->request_app = app_personal_page();
            break;
        case 6:
            s_update_view = true;
            s_ota_last_state = OTA_UPDATE_IDLE;
            s_ota_last_progress = UINT32_MAX;
            break;
    }
    return ctx->request_app ? APP_REDRAW_NONE : APP_REDRAW_PAGE;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    if (!s_update_view) return APP_REDRAW_NONE;
    ota_update_status_t status; ota_update_get_status(&status);
    if (status.state != s_ota_last_state) {
        s_ota_last_state = status.state;
        s_ota_last_progress = status.progress_percent;
        return APP_REDRAW_PAGE;
    }
    if (status.state == OTA_UPDATE_DOWNLOADING && status.progress_percent != s_ota_last_progress) {
        s_ota_last_progress = status.progress_percent;
        s_update_area = (EpdRect){UI_MARGIN, UI_CONTENT_TOP + 180, ui_content_width(), 52};
        ui_clear_rect_fast(ctx->fb, s_update_area);
        char line[64];
        snprintf(line, sizeof(line), "下载与写入进度：%u%%", status.progress_percent);
        ui_text(ctx->fb, UI_MARGIN, UI_CONTENT_TOP + 205, UI_PX_BODY, line, EPD_DRAW_ALIGN_LEFT, false);
        return APP_REDRAW_AREA;
    }
    return APP_REDRAW_NONE;
}

static EpdRect area_hint(app_ctx_t *ctx) { (void)ctx; return s_update_area; }

const app_desc_t app_settings = {
    .title = "设置 Settings",
    .detail = "网络、阅读、电源与设备选项",
    .on_enter = on_enter,
    .render = render,
    .on_gesture = on_gesture,
    .on_tick = on_tick,
    .blocks_idle_sleep = true,
    .area_hint = area_hint,
};
