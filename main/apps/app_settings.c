/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 一级设置入口，提供分类子页以查看并调整网络、时间、阅读、睡眠、电源、设备与更新选项。
 * Root settings app with dedicated sub-pages for network, time, reading, sleep, power, device and updates.
 * 冻结：页面只调用各功能公开接口；PMU 配置由 settings 模块提交。
 * Frozen: Use public feature APIs; the settings module submits PMU configuration.
 */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ambient_sync.h"
#include "app_registry.h"
#include "esp_app_desc.h"
#include "ota_update.h"
#include "read_pico_pmu.h"
#include "read_pico_transfer.h"
#include "settings.h"
#include "time_sync.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_time_edit.h"

extern const app_desc_t app_transfer;
extern const app_desc_t app_sleep;
extern const app_desc_t app_home;
extern const app_desc_t app_system_tools;

typedef enum {
    SETTINGS_VIEW_ROOT = 0,
    SETTINGS_VIEW_NETWORK_TIME,
    SETTINGS_VIEW_TIME_EDIT,
    SETTINGS_VIEW_DISPLAY,
    SETTINGS_VIEW_SLEEP,
    SETTINGS_VIEW_POWER,
    SETTINGS_VIEW_DEVICE,
    SETTINGS_VIEW_UPDATE,
} settings_view_t;

enum {
    CTRL_NONE = -1,
    CTRL_BACK = 100,
};

static settings_view_t s_view;
static char s_networks[5][33];
static size_t s_network_count;
static int s_pressed = CTRL_NONE;
static EpdRect s_dirty_area;
static char s_notice[96];
static ui_time_edit_t s_time_editor;
static ota_update_state_t s_ota_last_state;
static unsigned s_ota_last_progress;

static const uint16_t k_sleep_timeouts[] = {
    30, 60, 120, 300, 600, 900, 1800, 3600, 0,
};

static EpdRect row_rect(int row) {
    return ui_row_rect(0, 1, UI_CONTENT_TOP + row * (UI_BTN_H + UI_GAP), UI_BTN_H);
}

static EpdRect sub_row_rect(int y0, int row) {
    return ui_row_rect(0, 1, y0 + row * (UI_BTN_H + UI_GAP), UI_BTN_H);
}

static EpdRect update_control_rect(int col) {
    return ui_row_rect(col, 3, UI_CONTENT_TOP + 260, UI_BTN_H);
}

static const char *refresh_name(uint8_t value) {
    static const char *const names[] = { "纯净", "均衡", "极速" };
    return names[value % 3];
}

static const char *line_height_name(uint8_t value) {
    static const char *const names[] = { "紧凑", "标准", "宽松" };
    return names[value % 3];
}

static const char *margin_name(uint8_t value) {
    static const char *const names[] = { "窄", "标准", "宽" };
    return names[value % 3];
}

static const char *saver_name(app_saver_style_t style) {
    switch (style) {
        case APP_SAVER_QUOTE: return "最近摘录";
        case APP_SAVER_IMAGE: return "TF 卡图片";
        case APP_SAVER_BUILTIN:
        default: return "默认图案";
    }
}

static const char *sleep_mode_zh(app_sleep_mode_t mode) {
    switch (mode) {
        case APP_SLEEP_LIGHT: return "浅睡";
        case APP_SLEEP_OFF: return "关机";
        case APP_SLEEP_DEEP:
        default: return "深睡";
    }
}

static void fmt_timeout(char *buf, size_t n, uint16_t sec) {
    if (sec == 0) snprintf(buf, n, "关闭");
    else if (sec < 60) snprintf(buf, n, "%u 秒", (unsigned)sec);
    else snprintf(buf, n, "%u 分钟", (unsigned)(sec / 60));
}

static void fmt_tz(char *buf, size_t n, int tz_min) {
    char sign = tz_min < 0 ? '-' : '+';
    int abs_tz = tz_min < 0 ? -tz_min : tz_min;
    snprintf(buf, n, "UTC%c%d:%02d", sign, abs_tz / 60, abs_tz % 60);
}

static const char *running_version(void) {
    const esp_app_desc_t *desc = esp_app_get_description();
    return (desc && desc->version[0]) ? desc->version : "未知";
}

static void draw_action_btn(uint8_t *fb, EpdRect r, const char *label, bool pressed) {
    if (pressed) ui_draw_pressed_round_rect(fb, r, UI_BTN_RADIUS);
    else ui_draw_round_rect(fb, r, UI_BTN_RADIUS, UI_GRAY_BLACK);
    ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, UI_PX_BTN,
               label, EPD_DRAW_ALIGN_CENTER, false);
}

static void draw_kv_btn(uint8_t *fb, EpdRect r, const char *title, const char *detail, bool pressed) {
    if (pressed) ui_draw_pressed_round_rect(fb, r, UI_BTN_RADIUS);
    else ui_draw_round_rect(fb, r, UI_BTN_RADIUS, UI_GRAY_BLACK);
    int cy = r.y + r.height / 2;
    ui_text_vc(fb, r.x + 20, cy, UI_PX_BTN, title, EPD_DRAW_ALIGN_LEFT, false);
    if (detail && detail[0]) {
        ui_text_vc(fb, r.x + r.width - 20, cy, UI_PX_CAPTION, detail, EPD_DRAW_ALIGN_RIGHT, false);
    }
}

static void render_root(uint8_t *fb) {
    ui_draw_header(fb, "设置 Settings", "点按分类查看与调整设备选项");
    char detail[96];
    char tz[24];
    fmt_tz(tz, sizeof(tz), app_settings_timezone_min());

    snprintf(detail, sizeof(detail), "Wi-Fi %u · %s ›", (unsigned)s_network_count, tz);
    draw_kv_btn(fb, row_rect(0), "网络与时间", detail, s_pressed == 0);

    snprintf(detail, sizeof(detail), "%u px · %s ›",
             (unsigned)app_settings_book_px(), refresh_name(app_settings_book_refresh()));
    draw_kv_btn(fb, row_rect(1), "显示与阅读", detail, s_pressed == 1);

    char to_buf[24];
    fmt_timeout(to_buf, sizeof(to_buf), app_settings_auto_sleep_sec());
    snprintf(detail, sizeof(detail), "%s · %s ›",
             to_buf, saver_name(app_settings_saver_style()));
    draw_kv_btn(fb, row_rect(2), "睡眠与屏保", detail, s_pressed == 2);

    char low_buf[16];
    uint8_t low_pct = app_settings_low_soc_notify_percent();
    if (low_pct == 0) snprintf(low_buf, sizeof(low_buf), "关");
    else snprintf(low_buf, sizeof(low_buf), "%u%%", (unsigned)low_pct);
    snprintf(detail, sizeof(detail), "灯%s · 低电%s · 静音%s ›",
             app_settings_charge_led() ? "开" : "关", low_buf,
             app_settings_muted() ? "开" : "关");
    draw_kv_btn(fb, row_rect(3), "电源与提示", detail, s_pressed == 3);

    snprintf(detail, sizeof(detail), "%s ›", running_version());
    draw_kv_btn(fb, row_rect(4), "设备与系统工具", detail, s_pressed == 4);

    app_ambient_settings_t ambient;
    app_settings_ambient_get(&ambient);
    bool has_result = false, last_success = false;
    uint32_t last_ts = 0;
    app_settings_ambient_result_get(&has_result, &last_success, &last_ts);
    snprintf(detail, sizeof(detail), "%s · %u 分 · %s ›",
             ambient.enabled ? "开启" : "关闭", (unsigned)ambient.interval_min,
             has_result ? (last_success ? "成功" : "失败") : "暂无");
    draw_kv_btn(fb, row_rect(5), "定时看板", detail, s_pressed == 5);

    snprintf(detail, sizeof(detail), "Wi-Fi OTA ›");
    draw_kv_btn(fb, row_rect(6), "系统更新", detail, s_pressed == 6);

    char wake_line[96];
    time_t now = 0;
    if (ambient.enabled && app_time_now_utc(&now)) {
        time_t local_now = now + (time_t)app_settings_timezone_min() * 60;
        struct tm local;
        if (gmtime_r(&local_now, &local)) {
            uint32_t seconds = (uint32_t)local.tm_hour * 3600U
                + (uint32_t)local.tm_min * 60U + (uint32_t)local.tm_sec;
            uint32_t delay = ambient_sync_next_delay(seconds, (uint32_t)ambient.interval_min * 60U,
                                                     ambient.quiet_start_h, ambient.quiet_end_h);
            time_t target = local_now + delay;
            if (gmtime_r(&target, &local)) {
                strftime(wake_line, sizeof(wake_line), "下一次计划唤醒：%m-%d %H:%M", &local);
            } else {
                snprintf(wake_line, sizeof(wake_line), "下一次计划唤醒：时间不可用");
            }
        }
    } else {
        snprintf(wake_line, sizeof(wake_line), "下一次计划唤醒：未启用或时间未同步");
    }
    ui_text(fb, UI_MARGIN, 868, UI_PX_CAPTION, wake_line, EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_menu_handle(fb, false);
}

static void render_network_time(uint8_t *fb) {
    ui_draw_header(fb, "网络与时间", "Wi-Fi、时区与手动校准时钟");
    char val[96];
    int y = UI_CONTENT_TOP;
    y = ui_draw_section(fb, y, "当前状态");

    struct tm local;
    int tz_min = app_settings_timezone_min();
    if (app_time_now_local(&local, &tz_min)) {
        strftime(val, sizeof(val), "%Y-%m-%d  %H:%M", &local);
    } else {
        snprintf(val, sizeof(val), "尚未校准");
    }
    y = ui_draw_row(fb, y, "日期与时间", val);

    fmt_tz(val, sizeof(val), tz_min);
    y = ui_draw_row(fb, y, "当前时区", val);

    if (s_network_count > 0) {
        snprintf(val, sizeof(val), "%u 个 · %.24s", (unsigned)s_network_count, s_networks[0]);
    } else {
        snprintf(val, sizeof(val), "尚未保存网络");
    }
    y = ui_draw_row(fb, y, "已保存 Wi-Fi", val);

    int btn_y = y + UI_SECTION_GAP;
    draw_kv_btn(fb, sub_row_rect(btn_y, 0), "手动校准日期与时间", "设置 ›", s_pressed == 0);
    draw_action_btn(fb, ui_row_rect(0, 2, btn_y + (UI_BTN_H + UI_GAP), UI_BTN_H), "时区 -30分", s_pressed == 1);
    draw_action_btn(fb, ui_row_rect(1, 2, btn_y + (UI_BTN_H + UI_GAP), UI_BTN_H), "时区 +30分", s_pressed == 2);
    draw_kv_btn(fb, sub_row_rect(btn_y, 2), "Wi-Fi 传书与网络管理", "打开 ›", s_pressed == 3);

    if (s_notice[0]) {
        ui_text(fb, UI_MARGIN, btn_y + 3 * (UI_BTN_H + UI_GAP) + 8, UI_PX_CAPTION,
                s_notice, EPD_DRAW_ALIGN_LEFT, false);
    }
    draw_action_btn(fb, ui_bar_rect(0, 1), "返回设置", s_pressed == CTRL_BACK);
    ui_draw_menu_handle(fb, false);
}

static void render_display(uint8_t *fb) {
    ui_draw_header(fb, "显示与阅读", "字体、默认字号、刷新与排版");
    char detail[64];
    char btn_l[32], btn_r[32];

    draw_kv_btn(fb, row_rect(0), "字体管理与切换", "打开 ›", s_pressed == 0);

    snprintf(btn_l, sizeof(btn_l), "字号 - (%u)", (unsigned)app_settings_book_px());
    snprintf(btn_r, sizeof(btn_r), "字号 + (%u)", (unsigned)app_settings_book_px());
    int y1 = UI_CONTENT_TOP + (UI_BTN_H + UI_GAP);
    draw_action_btn(fb, ui_row_rect(0, 2, y1, UI_BTN_H), btn_l, s_pressed == 1);
    draw_action_btn(fb, ui_row_rect(1, 2, y1, UI_BTN_H), btn_r, s_pressed == 2);

    snprintf(detail, sizeof(detail), "%s", refresh_name(app_settings_book_refresh()));
    draw_kv_btn(fb, row_rect(2), "阅读刷新模式", detail, s_pressed == 3);

    uint16_t weight = app_settings_book_weight();
    if (weight == 0) snprintf(detail, sizeof(detail), "字体默认");
    else snprintf(detail, sizeof(detail), "%u", (unsigned)weight);
    draw_kv_btn(fb, row_rect(3), "阅读字重", detail, s_pressed == 4);

    snprintf(detail, sizeof(detail), "%s", line_height_name(app_settings_book_line_height()));
    draw_kv_btn(fb, row_rect(4), "阅读行距", detail, s_pressed == 5);

    snprintf(detail, sizeof(detail), "%s", margin_name(app_settings_book_margin()));
    draw_kv_btn(fb, row_rect(5), "阅读页边距", detail, s_pressed == 6);

    draw_action_btn(fb, ui_bar_rect(0, 1), "返回设置", s_pressed == CTRL_BACK);
    ui_draw_menu_handle(fb, false);
}

static void render_sleep(uint8_t *fb) {
    ui_draw_header(fb, "睡眠与屏保", "锁屏档位、自动休眠与锁屏内容");
    char detail[64];

    snprintf(detail, sizeof(detail), "%s", sleep_mode_zh(app_settings_sleep_mode()));
    draw_kv_btn(fb, row_rect(0), "锁屏方式", detail, s_pressed == 0);

    fmt_timeout(detail, sizeof(detail), app_settings_auto_sleep_sec());
    draw_kv_btn(fb, row_rect(1), "自动锁屏时长", detail, s_pressed == 1);

    snprintf(detail, sizeof(detail), "%s", saver_name(app_settings_saver_style()));
    draw_kv_btn(fb, row_rect(2), "锁屏内容", detail, s_pressed == 2);

    draw_kv_btn(fb, row_rect(3), "睡眠与唤醒详细设置", "打开 ›", s_pressed == 3);
    draw_kv_btn(fb, row_rect(4), "我的看板与定时刷新", "打开 ›", s_pressed == 4);

    draw_action_btn(fb, ui_bar_rect(0, 1), "返回设置", s_pressed == CTRL_BACK);
    ui_draw_menu_handle(fb, false);
}

static void render_power(uint8_t *fb) {
    ui_draw_header(fb, "电源与提示", "电池状态、充电灯、低电提醒与静音");
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    char val[64];
    int y = UI_CONTENT_TOP;
    y = ui_draw_section(fb, y, "电池状态");
    if (pmu && pmu->status_ok) {
        snprintf(val, sizeof(val), "%u%%  ·  %u.%03u V",
                 (unsigned)(pmu->soc_permille / 10),
                 (unsigned)(pmu->battery_mv / 1000),
                 (unsigned)(pmu->battery_mv % 1000));
    } else {
        snprintf(val, sizeof(val), "暂不可用");
    }
    y = ui_draw_row(fb, y, "当前电量", val);

    int btn_y = y + UI_SECTION_GAP;
    draw_kv_btn(fb, sub_row_rect(btn_y, 0), "充电指示灯",
                app_settings_charge_led() ? "开启" : "关闭", s_pressed == 0);

    uint8_t low = app_settings_low_soc_notify_percent();
    if (low == 0) snprintf(val, sizeof(val), "关闭");
    else snprintf(val, sizeof(val), "%u%%", (unsigned)low);
    draw_kv_btn(fb, sub_row_rect(btn_y, 1), "低电提醒阈值", val, s_pressed == 1);

    draw_kv_btn(fb, sub_row_rect(btn_y, 2), "全局蜂鸣静音",
                app_settings_muted() ? "开启" : "关闭", s_pressed == 2);

    draw_action_btn(fb, ui_bar_rect(0, 1), "返回设置", s_pressed == CTRL_BACK);
    ui_draw_menu_handle(fb, false);
}

static void render_device(uint8_t *fb) {
    ui_draw_header(fb, "设备与系统工具", "固件版本、设备信息与硬件诊断");
    const esp_app_desc_t *desc = esp_app_get_description();
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    char val[64];
    int y = UI_CONTENT_TOP;
    y = ui_draw_section(fb, y, "固件与硬件");
    y = ui_draw_row(fb, y, "固件版本", running_version());
    snprintf(val, sizeof(val), "%s %s", desc ? desc->date : "-", desc ? desc->time : "");
    y = ui_draw_row(fb, y, "构建 UTC", val);
    if (pmu && pmu->identity_ok) {
        snprintf(val, sizeof(val), "%u.%u.%u", pmu->fw_major, pmu->fw_minor, pmu->fw_patch);
    } else {
        snprintf(val, sizeof(val), "未连上");
    }
    y = ui_draw_row(fb, y, "PMU 固件", val);

    int btn_y = y + UI_SECTION_GAP;
    draw_kv_btn(fb, sub_row_rect(btn_y, 0), "概览与设备识别码", "打开 ›", s_pressed == 0);
    draw_kv_btn(fb, sub_row_rect(btn_y, 1), "系统工具与硬件诊断", "打开 ›", s_pressed == 1);
    draw_kv_btn(fb, sub_row_rect(btn_y, 2), "设备自检", "打开 ›", s_pressed == 2);

    draw_action_btn(fb, ui_bar_rect(0, 1), "返回设置", s_pressed == CTRL_BACK);
    ui_draw_menu_handle(fb, false);
}

static void render_update(uint8_t *fb) {
    ota_update_status_t status;
    ota_update_get_status(&status);
    ui_draw_header(fb, "系统更新", "GitHub Releases · Wi-Fi OTA");
    const char *cur = status.current_version[0] ? status.current_version : running_version();
    char line[128];
    snprintf(line, sizeof(line), "当前版本 %s  ·  最新版本 %s",
             cur, status.latest_version[0] ? status.latest_version : "未检查");
    ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 90, UI_PX_BODY, line, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 150, UI_PX_BODY,
            s_notice[0] ? s_notice : (status.message[0] ? status.message : "请连接已有 Wi-Fi 后检查更新"),
            EPD_DRAW_ALIGN_LEFT, false);
    if (status.state == OTA_UPDATE_DOWNLOADING) {
        snprintf(line, sizeof(line), "下载与写入进度：%u%%", status.progress_percent);
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 205, UI_PX_BODY, line, EPD_DRAW_ALIGN_LEFT, false);
    }
    draw_action_btn(fb, update_control_rect(0), "检查更新", s_pressed == 0);
    draw_action_btn(fb, update_control_rect(1), "安装更新", s_pressed == 1);
    draw_action_btn(fb, update_control_rect(2), "取消 / 返回", s_pressed == 2);
    draw_action_btn(fb, ui_bar_rect(0, 1), "返回设置", s_pressed == CTRL_BACK);
    ui_draw_menu_handle(fb, false);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    if (s_view == SETTINGS_VIEW_TIME_EDIT) {
        ui_time_edit_render(&s_time_editor, fb);
        return;
    }
    ui_clear_page(fb);
    switch (s_view) {
        case SETTINGS_VIEW_NETWORK_TIME: render_network_time(fb); break;
        case SETTINGS_VIEW_DISPLAY: render_display(fb); break;
        case SETTINGS_VIEW_SLEEP: render_sleep(fb); break;
        case SETTINGS_VIEW_POWER: render_power(fb); break;
        case SETTINGS_VIEW_DEVICE: render_device(fb); break;
        case SETTINGS_VIEW_UPDATE: render_update(fb); break;
        case SETTINGS_VIEW_ROOT:
        default: render_root(fb); break;
    }
}

static int hit_control(uint16_t x, uint16_t y, EpdRect *out_rect) {
    if (s_view == SETTINGS_VIEW_ROOT) {
        for (int i = 0; i < 7; ++i) {
            EpdRect r = row_rect(i);
            if (ui_rect_hit(r, x, y)) {
                if (out_rect) *out_rect = r;
                return i;
            }
        }
        return CTRL_NONE;
    }

    if (s_view == SETTINGS_VIEW_TIME_EDIT) {
        return ui_time_edit_hit_rect(x, y, out_rect) ? 0 : CTRL_NONE;
    }

    EpdRect back = ui_bar_rect(0, 1);
    if (ui_rect_hit(back, x, y)) {
        if (out_rect) *out_rect = back;
        return CTRL_BACK;
    }

    if (s_view == SETTINGS_VIEW_NETWORK_TIME) {
        int btn_y = UI_CONTENT_TOP + UI_SEC_HEAD + 3 * UI_ROW_H + UI_SECTION_GAP;
        EpdRect r0 = sub_row_rect(btn_y, 0);
        EpdRect r1 = ui_row_rect(0, 2, btn_y + (UI_BTN_H + UI_GAP), UI_BTN_H);
        EpdRect r2 = ui_row_rect(1, 2, btn_y + (UI_BTN_H + UI_GAP), UI_BTN_H);
        EpdRect r3 = sub_row_rect(btn_y, 2);
        EpdRect rects[4] = { r0, r1, r2, r3 };
        for (int i = 0; i < 4; ++i) {
            if (ui_rect_hit(rects[i], x, y)) {
                if (out_rect) *out_rect = rects[i];
                return i;
            }
        }
        return CTRL_NONE;
    }

    if (s_view == SETTINGS_VIEW_DISPLAY) {
        int y1 = UI_CONTENT_TOP + (UI_BTN_H + UI_GAP);
        EpdRect rects[7] = {
            row_rect(0),
            ui_row_rect(0, 2, y1, UI_BTN_H),
            ui_row_rect(1, 2, y1, UI_BTN_H),
            row_rect(2),
            row_rect(3),
            row_rect(4),
            row_rect(5),
        };
        for (int i = 0; i < 7; ++i) {
            if (ui_rect_hit(rects[i], x, y)) {
                if (out_rect) *out_rect = rects[i];
                return i;
            }
        }
        return CTRL_NONE;
    }

    if (s_view == SETTINGS_VIEW_SLEEP) {
        for (int i = 0; i < 5; ++i) {
            EpdRect r = row_rect(i);
            if (ui_rect_hit(r, x, y)) {
                if (out_rect) *out_rect = r;
                return i;
            }
        }
        return CTRL_NONE;
    }

    if (s_view == SETTINGS_VIEW_POWER) {
        int btn_y = UI_CONTENT_TOP + UI_SEC_HEAD + UI_ROW_H + UI_SECTION_GAP;
        for (int i = 0; i < 3; ++i) {
            EpdRect r = sub_row_rect(btn_y, i);
            if (ui_rect_hit(r, x, y)) {
                if (out_rect) *out_rect = r;
                return i;
            }
        }
        return CTRL_NONE;
    }

    if (s_view == SETTINGS_VIEW_DEVICE) {
        int btn_y = UI_CONTENT_TOP + UI_SEC_HEAD + 3 * UI_ROW_H + UI_SECTION_GAP;
        for (int i = 0; i < 3; ++i) {
            EpdRect r = sub_row_rect(btn_y, i);
            if (ui_rect_hit(r, x, y)) {
                if (out_rect) *out_rect = r;
                return i;
            }
        }
        return CTRL_NONE;
    }

    if (s_view == SETTINGS_VIEW_UPDATE) {
        for (int i = 0; i < 3; ++i) {
            EpdRect r = update_control_rect(i);
            if (ui_rect_hit(r, x, y)) {
                if (out_rect) *out_rect = r;
                return i;
            }
        }
        return CTRL_NONE;
    }

    return CTRL_NONE;
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    (void)read_pico_transfer_get_saved_wifi_list(s_networks, &s_network_count);
    (void)read_pico_pmu_refresh();
    app_time_sync_from_pmu();
    s_view = SETTINGS_VIEW_ROOT;
    s_pressed = CTRL_NONE;
    s_dirty_area = (EpdRect){ 0 };
    s_notice[0] = '\0';
}

static app_redraw_t commit_action(app_ctx_t *ctx, int hit, uint16_t x, uint16_t y) {
    if (s_view == SETTINGS_VIEW_TIME_EDIT) {
        ui_time_edit_result_t res = ui_time_edit_tap(&s_time_editor, x, y);
        if (res == UI_TIME_EDIT_SAVED) {
            snprintf(s_notice, sizeof(s_notice), "%s",
                     s_time_editor.pmu_synced ? "时间已保存并同步主板 RTC" : "时间已保存至主机");
            s_view = SETTINGS_VIEW_NETWORK_TIME;
            return APP_REDRAW_FULL;
        }
        if (res == UI_TIME_EDIT_CANCELLED) {
            s_view = SETTINGS_VIEW_NETWORK_TIME;
            return APP_REDRAW_FULL;
        }
        return res == UI_TIME_EDIT_CHANGED ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }

    if (hit == CTRL_BACK) {
        if (s_view == SETTINGS_VIEW_UPDATE) {
            ota_update_status_t status;
            ota_update_get_status(&status);
            if (status.state == OTA_UPDATE_DOWNLOADING || status.state == OTA_UPDATE_CHECKING) {
                ota_update_cancel();
                return APP_REDRAW_PAGE;
            }
        }
        s_notice[0] = '\0';
        s_view = SETTINGS_VIEW_ROOT;
        return APP_REDRAW_PAGE;
    }

    if (s_view == SETTINGS_VIEW_ROOT) {
        s_notice[0] = '\0';
        switch (hit) {
            case 0:
                (void)read_pico_transfer_get_saved_wifi_list(s_networks, &s_network_count);
                s_view = SETTINGS_VIEW_NETWORK_TIME;
                return APP_REDRAW_PAGE;
            case 1:
                s_view = SETTINGS_VIEW_DISPLAY;
                return APP_REDRAW_PAGE;
            case 2:
                s_view = SETTINGS_VIEW_SLEEP;
                return APP_REDRAW_PAGE;
            case 3:
                (void)read_pico_pmu_refresh();
                s_view = SETTINGS_VIEW_POWER;
                return APP_REDRAW_PAGE;
            case 4:
                (void)read_pico_pmu_refresh();
                s_view = SETTINGS_VIEW_DEVICE;
                return APP_REDRAW_PAGE;
            case 5:
                ctx->request_app = app_personal_page();
                return APP_REDRAW_NONE;
            case 6:
                s_view = SETTINGS_VIEW_UPDATE;
                s_ota_last_state = OTA_UPDATE_IDLE;
                s_ota_last_progress = UINT32_MAX;
                return APP_REDRAW_PAGE;
            default:
                return APP_REDRAW_NONE;
        }
    }

    if (s_view == SETTINGS_VIEW_NETWORK_TIME) {
        if (hit == 0) {
            ui_time_edit_open(&s_time_editor);
            s_view = SETTINGS_VIEW_TIME_EDIT;
            return APP_REDRAW_FULL;
        }
        if (hit == 1 || hit == 2) {
            int next = app_settings_timezone_min() + (hit == 1 ? -30 : 30);
            if (next < -720) next = 840;
            if (next > 840) next = -720;
            app_settings_set_timezone_min((int16_t)next);
            s_notice[0] = '\0';
            return APP_REDRAW_PAGE;
        }
        if (hit == 3) {
            ctx->request_app = &app_transfer;
            return APP_REDRAW_NONE;
        }
        return APP_REDRAW_NONE;
    }

    if (s_view == SETTINGS_VIEW_DISPLAY) {
        if (hit == 0) {
            ctx->request_app = app_font_page();
            return APP_REDRAW_NONE;
        }
        if (hit == 1) {
            uint8_t px = app_settings_book_px();
            app_settings_set_book_px(px <= 36 ? 72 : (uint8_t)(px - 4));
        } else if (hit == 2) {
            uint8_t px = app_settings_book_px();
            app_settings_set_book_px(px >= 72 ? 36 : (uint8_t)(px + 4));
        } else if (hit == 3) {
            app_settings_set_book_refresh((uint8_t)((app_settings_book_refresh() + 1) % 3));
        } else if (hit == 4) {
            uint16_t w = app_settings_book_weight();
            app_settings_set_book_weight(w >= 800 ? 0 : (w == 0 ? 300 : (uint16_t)(w + 100)));
        } else if (hit == 5) {
            app_settings_set_book_line_height((uint8_t)((app_settings_book_line_height() + 1) % 3));
        } else if (hit == 6) {
            app_settings_set_book_margin((uint8_t)((app_settings_book_margin() + 1) % 3));
        }
        return APP_REDRAW_PAGE;
    }

    if (s_view == SETTINGS_VIEW_SLEEP) {
        if (hit == 0) {
            app_settings_set_sleep_mode((app_sleep_mode_t)((app_settings_sleep_mode() + 1) % 3));
            return APP_REDRAW_PAGE;
        }
        if (hit == 1) {
            uint16_t cur = app_settings_auto_sleep_sec();
            size_t count = sizeof(k_sleep_timeouts) / sizeof(k_sleep_timeouts[0]);
            size_t idx = 0;
            while (idx + 1 < count && k_sleep_timeouts[idx] != cur) ++idx;
            app_settings_set_auto_sleep_sec(k_sleep_timeouts[(idx + 1) % count]);
            return APP_REDRAW_PAGE;
        }
        if (hit == 2) {
            app_settings_set_saver_style((app_saver_style_t)((app_settings_saver_style() + 1) % 3));
            return APP_REDRAW_PAGE;
        }
        if (hit == 3) {
            ctx->request_app = &app_sleep;
            return APP_REDRAW_NONE;
        }
        if (hit == 4) {
            ctx->request_app = app_personal_page();
            return APP_REDRAW_NONE;
        }
        return APP_REDRAW_NONE;
    }

    if (s_view == SETTINGS_VIEW_POWER) {
        if (hit == 0) {
            app_settings_set_charge_led(!app_settings_charge_led());
        } else if (hit == 1) {
            uint8_t n = app_settings_low_soc_notify_percent();
            app_settings_set_low_soc_notify_percent(n >= 30 ? 0 : (uint8_t)(n + 5));
        } else if (hit == 2) {
            app_settings_set_muted(!app_settings_muted());
        }
        return APP_REDRAW_PAGE;
    }

    if (s_view == SETTINGS_VIEW_DEVICE) {
        if (hit == 0) ctx->request_app = &app_home;
        else if (hit == 1) ctx->request_app = &app_system_tools;
        else if (hit == 2) ctx->request_app = app_selftest_page();
        return APP_REDRAW_NONE;
    }

    if (s_view == SETTINGS_VIEW_UPDATE) {
        if (hit == 0) {
            s_notice[0] = '\0';
            (void)ota_update_check_start();
            return APP_REDRAW_PAGE;
        }
        if (hit == 1) {
            (void)read_pico_pmu_refresh();
            const pmu_snapshot_t *pmu = read_pico_pmu_get();
            bool charging = pmu && pmu->charge_state == PMU_CHARGE_CHARGING;
            if (!pmu || !ota_update_battery_allowed(pmu->soc_permille, charging)) {
                snprintf(s_notice, sizeof(s_notice), "电量低于 20%%，请连接充电器后升级");
                return APP_REDRAW_PAGE;
            }
            s_notice[0] = '\0';
            ota_update_status_t status;
            ota_update_get_status(&status);
            if (status.state == OTA_UPDATE_AVAILABLE) (void)ota_update_install_start();
            return APP_REDRAW_PAGE;
        }
        if (hit == 2) {
            ota_update_status_t status;
            ota_update_get_status(&status);
            if (status.state == OTA_UPDATE_DOWNLOADING || status.state == OTA_UPDATE_CHECKING) {
                ota_update_cancel();
            } else {
                s_view = SETTINGS_VIEW_ROOT;
                s_notice[0] = '\0';
            }
            return APP_REDRAW_PAGE;
        }
    }

    return APP_REDRAW_NONE;
}

// 按下仅局部 DU 刷新，避免主循环因整页刷新提前取消手势；抬手时提交跳转或设置修改。
// Press uses local DU only so the loop does not cancel the active gesture; release commits navigation or changes.
static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    if (!event) return APP_REDRAW_NONE;

    if (event->type == UI_GESTURE_PRESS) {
        EpdRect rect = { 0 };
        int hit = hit_control(event->x0, event->y0, &rect);
        if (hit == CTRL_NONE) {
            s_pressed = CTRL_NONE;
            return APP_REDRAW_NONE;
        }
        s_pressed = hit;
        s_dirty_area = rect;
        if (ctx->fb && s_view != SETTINGS_VIEW_TIME_EDIT) {
            render(ctx, ctx->fb);
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }

    if (event->type == UI_GESTURE_CANCEL) {
        if (s_pressed == CTRL_NONE) return APP_REDRAW_NONE;
        s_pressed = CTRL_NONE;
        if (ctx->fb && s_view != SETTINGS_VIEW_TIME_EDIT && s_dirty_area.width > 0) {
            render(ctx, ctx->fb);
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }

    if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;

    int prev_pressed = s_pressed;
    s_pressed = CTRL_NONE;
    EpdRect start_rect = { 0 }, end_rect = { 0 };
    int start_hit = hit_control(event->x0, event->y0, &start_rect);
    int end_hit = hit_control(event->x, event->y, &end_rect);
    bool same_rect = start_rect.x == end_rect.x && start_rect.y == end_rect.y
        && start_rect.width == end_rect.width && start_rect.height == end_rect.height;
    if (start_hit == CTRL_NONE || start_hit != end_hit || !same_rect) {
        if (prev_pressed != CTRL_NONE && ctx->fb && s_view != SETTINGS_VIEW_TIME_EDIT && s_dirty_area.width > 0) {
            render(ctx, ctx->fb);
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }
    return commit_action(ctx, end_hit, event->x, event->y);
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    (void)ctx;
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    s_pressed = CTRL_NONE;
    if (s_view == SETTINGS_VIEW_TIME_EDIT) {
        s_view = SETTINGS_VIEW_NETWORK_TIME;
        return APP_REDRAW_FULL;
    }
    if (s_view != SETTINGS_VIEW_ROOT) {
        if (s_view == SETTINGS_VIEW_UPDATE) {
            ota_update_status_t status;
            ota_update_get_status(&status);
            if (status.state == OTA_UPDATE_DOWNLOADING || status.state == OTA_UPDATE_CHECKING) {
                ota_update_cancel();
                return APP_REDRAW_PAGE;
            }
        }
        s_view = SETTINGS_VIEW_ROOT;
        s_notice[0] = '\0';
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    if (s_view != SETTINGS_VIEW_UPDATE) return APP_REDRAW_NONE;
    ota_update_status_t status;
    ota_update_get_status(&status);
    if (status.state != s_ota_last_state) {
        s_ota_last_state = status.state;
        s_ota_last_progress = status.progress_percent;
        return APP_REDRAW_PAGE;
    }
    if (status.state == OTA_UPDATE_DOWNLOADING && status.progress_percent != s_ota_last_progress) {
        s_ota_last_progress = status.progress_percent;
        s_dirty_area = (EpdRect){ UI_MARGIN, UI_CONTENT_TOP + 180, ui_content_width(), 52 };
        ui_clear_rect_fast(ctx->fb, s_dirty_area);
        char line[64];
        snprintf(line, sizeof(line), "下载与写入进度：%u%%", status.progress_percent);
        ui_text(ctx->fb, UI_MARGIN, UI_CONTENT_TOP + 205, UI_PX_BODY, line, EPD_DRAW_ALIGN_LEFT, false);
        return APP_REDRAW_AREA;
    }
    return APP_REDRAW_NONE;
}

static EpdRect area_hint(app_ctx_t *ctx) {
    (void)ctx;
    return s_dirty_area;
}

const app_desc_t app_settings = {
    .title = "设置 Settings",
    .detail = "网络、阅读、电源与设备选项",
    .on_enter = on_enter,
    .render = render,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_tick = on_tick,
    .blocks_idle_sleep = true,
    .area_hint = area_hint,
};
