/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：全系统顶部下滑快捷控制菜单（控制中心）实现。
 * English: Implementation of the system-wide top pull-down Quick Control Menu.
 *
 * 冻结：render 只做纯帧缓冲绘制；打开/关闭不触发底层页面生命周期回调。
 * Frozen: render only paints the framebuffer; open/close never invoke page lifecycle callbacks.
 */

#include "ui_quick_menu.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app_book.h"
#include "app_registry.h"
#include "app_settings.h"
#include "ble_service.h"
#include "read_pico_pmu.h"
#include "read_pico_sd.h"
#include "read_pico_transfer.h"
#include "settings.h"
#include "time_sync.h"
#include "ttf_font.h"
#include "ui_icons.h"
#include "ui_kit.h"
#include "ui_menu.h"

extern const app_desc_t app_transfer;

enum {
    QM_CTRL_NONE = -1,
    QM_CTRL_TIME = 0,
    QM_CTRL_BLE = 1,
    QM_CTRL_MUTE = 2,
    QM_CTRL_LED = 3,
    QM_CTRL_PICKUP = 4,
    QM_CTRL_AUTOSLEEP = 5,
    QM_CTRL_SAVER = 6,
    QM_CTRL_PX_DEC = 7,
    QM_CTRL_PX_INC = 8,
    QM_CTRL_REFRESH = 9,
    QM_CTRL_HUD = 10,
    QM_CTRL_ROTATION = 11,
    QM_CTRL_CLEAN = 12,
    QM_CTRL_LOCK = 13,
    QM_CTRL_TRANSFER = 14,
    QM_CTRL_NAV = 15,
    QM_CTRL_DISMISS = 16,
    QM_CTRL_COUNT = 17,
};

#define QM_HEADER_RULE_Y 132
#define QM_SEC1_LABEL_Y 144
#define QM_ROW0_Y 180
#define QM_TOGGLE_H 82
#define QM_ROW1_Y (QM_ROW0_Y + QM_TOGGLE_H + UI_GAP)
#define QM_ROW2_Y (QM_ROW1_Y + QM_TOGGLE_H + UI_GAP)

#define QM_SEC2_LABEL_Y 472
#define QM_READ0_Y 508
#define QM_READ_H 80
#define QM_READ1_Y (QM_READ0_Y + QM_READ_H + UI_GAP)

#define QM_SEC3_LABEL_Y 704
#define QM_ACT_Y 740
#define QM_ACT_H 82
#define QM_HANDLE_Y 882

#define QM_DISMISS_Y 988
#define QM_DISMISS_W 308
#define QM_DISMISS_H 76

static const uint16_t k_sleep_timeouts[] = {
    30, 60, 120, 300, 600, 900, 1800, 3600, 0,
};

static EpdRect qm_card_rect(void) {
    return (EpdRect){
        .x = UI_QM_CARD_X,
        .y = UI_QM_CARD_Y,
        .width = UI_QM_CARD_W,
        .height = UI_QM_CARD_H,
    };
}

static EpdRect qm_ctrl_rect(int id) {
    switch (id) {
        case QM_CTRL_TIME:
            return (EpdRect){ .x = UI_MARGIN, .y = 30, .width = 320, .height = 92 };
        case QM_CTRL_BLE:
            return ui_row_rect(0, 2, QM_ROW0_Y, QM_TOGGLE_H);
        case QM_CTRL_MUTE:
            return ui_row_rect(1, 2, QM_ROW0_Y, QM_TOGGLE_H);
        case QM_CTRL_LED:
            return ui_row_rect(0, 2, QM_ROW1_Y, QM_TOGGLE_H);
        case QM_CTRL_PICKUP:
            return ui_row_rect(1, 2, QM_ROW1_Y, QM_TOGGLE_H);
        case QM_CTRL_AUTOSLEEP:
            return ui_row_rect(0, 2, QM_ROW2_Y, QM_TOGGLE_H);
        case QM_CTRL_SAVER:
            return ui_row_rect(1, 2, QM_ROW2_Y, QM_TOGGLE_H);
        case QM_CTRL_PX_DEC:
            return ui_row_rect(0, 3, QM_READ0_Y, QM_READ_H);
        case QM_CTRL_PX_INC:
            return ui_row_rect(1, 3, QM_READ0_Y, QM_READ_H);
        case QM_CTRL_REFRESH:
            return ui_row_rect(2, 3, QM_READ0_Y, QM_READ_H);
        case QM_CTRL_HUD:
            return ui_row_rect(0, 2, QM_READ1_Y, QM_READ_H);
        case QM_CTRL_ROTATION:
            return ui_row_rect(1, 2, QM_READ1_Y, QM_READ_H);
        case QM_CTRL_CLEAN:
            return ui_row_rect(0, 4, QM_ACT_Y, QM_ACT_H);
        case QM_CTRL_LOCK:
            return ui_row_rect(1, 4, QM_ACT_Y, QM_ACT_H);
        case QM_CTRL_TRANSFER:
            return ui_row_rect(2, 4, QM_ACT_Y, QM_ACT_H);
        case QM_CTRL_NAV:
            return ui_row_rect(3, 4, QM_ACT_Y, QM_ACT_H);
        case QM_CTRL_DISMISS:
            return (EpdRect){
                .x = (UI_LOCK_WIDTH - QM_DISMISS_W) / 2,
                .y = QM_DISMISS_Y,
                .width = QM_DISMISS_W,
                .height = QM_DISMISS_H,
            };
        default:
            return (EpdRect){ 0 };
    }
}

static int qm_hit_ctrl(uint16_t x, uint16_t y, EpdRect *out_rect) {
    for (int i = 0; i < QM_CTRL_COUNT; ++i) {
        EpdRect r = qm_ctrl_rect(i);
        if (ui_rect_hit(r, x, y)) {
            if (out_rect) *out_rect = r;
            return i;
        }
    }
    return QM_CTRL_NONE;
}

static void fmt_timeout_short(char *buf, size_t cap, uint16_t sec) {
    if (sec == 0) snprintf(buf, cap, "关闭");
    else if (sec < 60) snprintf(buf, cap, "%u 秒", (unsigned)sec);
    else snprintf(buf, cap, "%u 分钟", (unsigned)(sec / 60));
}

static const char *saver_short_name(app_saver_style_t s) {
    switch (s) {
        case APP_SAVER_QUOTE: return "最近摘录";
        case APP_SAVER_IMAGE: return "TF 图片";
        case APP_SAVER_BOOK_COVER: return "当前书封";
        case APP_SAVER_HEATMAP: return "阅读热力";
        case APP_SAVER_BUILTIN:
        default: return "默认图案";
    }
}

static const char *refresh_short_name(uint8_t r) {
    static const char *const names[] = { "纯净", "均衡", "极速" };
    return names[r % 3];
}

static const char *rotation_short_name(uint8_t r) {
    static const char *const names[] = { "竖屏", "横屏", "自动" };
    return names[r % 3];
}

static const char *hud_mode_name(bool top, bool bot) {
    if (top && bot) return "全显";
    if (top) return "仅顶栏";
    if (bot) return "仅底栏";
    return "纯全屏";
}

static void draw_scrim(uint8_t *fb, int y_top, int y_bottom) {
    const int w = epd_rotated_display_width();
    ui_clear_rect_fast(fb, (EpdRect){ .x = 0, .y = y_top, .width = w, .height = y_bottom - y_top });
    for (int y = y_top + 6; y < y_bottom; y += 8) {
        for (int x = (y & 8) ? 6 : 2; x < w; x += 8) {
            epd_fill_rect((EpdRect){ .x = x, .y = y, .width = 2, .height = 2 }, UI_GRAY_LIGHT, fb);
        }
    }
}

static void draw_toggle_tile(
    uint8_t *fb, EpdRect r, const char *title, const char *state, bool active, bool pressed
) {
    if (pressed) {
        ui_draw_pressed_round_rect(fb, r, UI_BTN_RADIUS);
    } else if (active) {
        ui_draw_selected_round_rect(fb, r, UI_BTN_RADIUS);
    } else {
        ui_draw_round_rect(fb, r, UI_BTN_RADIUS, UI_GRAY_BLACK);
    }
    const int cy = r.y + r.height / 2;
    ui_text_vc(fb, r.x + 18, cy, 32, title, EPD_DRAW_ALIGN_LEFT, false);
    if (state && state[0]) {
        ui_text_vc(fb, r.x + r.width - 18, cy, 26, state, EPD_DRAW_ALIGN_RIGHT, false);
    }
}

static void draw_cell_btn(uint8_t *fb, EpdRect r, const char *label, bool pressed) {
    if (pressed) {
        ui_draw_pressed_round_rect(fb, r, UI_BTN_RADIUS);
    } else {
        ui_draw_round_rect(fb, r, UI_BTN_RADIUS, UI_GRAY_BLACK);
    }
    ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, 30, label, EPD_DRAW_ALIGN_CENTER, false);
}

void ui_quick_menu_reset(ui_quick_menu_t *qm) {
    if (!qm) return;
    memset(qm, 0, sizeof(*qm));
    qm->pressed_ctrl = QM_CTRL_NONE;
}

void ui_quick_menu_open(ui_quick_menu_t *qm) {
    if (!qm) return;
    qm->open = true;
    qm->pressed_ctrl = QM_CTRL_NONE;
    qm->px_changed = false;
    qm->book_dirty = false;
    qm->dirty_area = (EpdRect){ 0 };
    if (read_pico_pmu_ready()) {
        (void)read_pico_pmu_refresh();
    }
    app_time_sync_from_pmu();
}

void ui_quick_menu_close(ui_quick_menu_t *qm, app_ctx_t *ctx, const app_desc_t *current) {
    if (!qm || !qm->open) return;
    const bool book_dirty = qm->book_dirty;
    const bool px_changed = qm->px_changed;
    ui_quick_menu_reset(qm);
    if (book_dirty && current == &app_book) {
        app_book_sync_runtime_settings(ctx, px_changed);
    }
}

void ui_quick_menu_key_hints(ui_key_hints_t *out) {
    if (!out) return;
    out->left = "收起";
    out->center = "刷新 · 长按桌面";
    out->right = "设置";
    out->hidden = false;
}

void ui_quick_menu_render(
    const ui_quick_menu_t *qm, app_ctx_t *ctx, const app_desc_t *current, uint8_t *fb
) {
    (void)ctx;
    if (!qm || !fb) return;

    ui_clear_page(fb);
    draw_scrim(fb, UI_QM_CARD_BOTTOM, UI_KEY_HINT_RULE_Y);

    // 绘制顶部控制卡片双层描边与白底。/ Paint card white fill and crisp double border.
    EpdRect card = qm_card_rect();
    ui_fill_round_rect(fb, card, 20, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, card, 20, UI_GRAY_BLACK);
    ui_draw_round_rect(fb, ui_inset_rect(card, 1), 19, UI_GRAY_BLACK);

    // 顶栏左侧：实时本地时间与时区（支持点按直达时间校准）。
    // Header left: live local clock & timezone (tap jumps to Time Calibration).
    EpdRect time_rect = qm_ctrl_rect(QM_CTRL_TIME);
    if (qm->pressed_ctrl == QM_CTRL_TIME) {
        ui_draw_pressed_round_rect(fb, time_rect, 12);
    }
    struct tm local = { 0 };
    int tz_min = app_settings_timezone_min();
    char clock_buf[16] = "--:--";
    char date_buf[64];
    if (app_time_now_local(&local, &tz_min)) {
        snprintf(clock_buf, sizeof(clock_buf), "%02d:%02d", local.tm_hour, local.tm_min);
        char sign = tz_min < 0 ? '-' : '+';
        int abs_tz = tz_min < 0 ? -tz_min : tz_min;
        snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d · UTC%c%d:%02d ›",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 sign, abs_tz / 60, abs_tz % 60);
    } else {
        snprintf(date_buf, sizeof(date_buf), "时间未校准 · 点按设置 ›");
    }
    ui_text_vc(fb, UI_MARGIN + 6, 58, 44, clock_buf, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_vc(fb, UI_MARGIN + 6, 104, 24, date_buf, EPD_DRAW_ALIGN_LEFT, false);

    // 顶栏右侧：电池图标 + 百分比/电压 + 存储与当前页面摘要。
    // Header right: battery icon + %/voltage + storage & current app summary.
    const int right_x = UI_LOCK_WIDTH - UI_MARGIN;
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    int pct = -1;
    bool charging = false;
    char bat_line[48];
    if (pmu && pmu->present && pmu->status_ok) {
        pct = (int)(pmu->soc_permille / 10U);
        if (pct > 100) pct = 100;
        charging = (pmu->charge_state == PMU_CHARGE_CHARGING);
        snprintf(bat_line, sizeof(bat_line), "%s%d%% · %u.%02uV",
                 charging ? "充电 " : "", pct,
                 (unsigned)(pmu->battery_mv / 1000U),
                 (unsigned)((pmu->battery_mv % 1000U) / 10U));
    } else {
        snprintf(bat_line, sizeof(bat_line), "电量 --%%");
    }
    ui_text_vc(fb, right_x, 58, 26, bat_line, EPD_DRAW_ALIGN_RIGHT, false);
    const int bat_tw = ttf_text_width_px(26, bat_line);
    ui_draw_battery_icon(fb, right_x - bat_tw - 42, 49, 34, 18, pct >= 0 ? pct : 0, charging);

    read_pico_sd_info_t sd = { 0 };
    const bool sd_ok = (read_pico_sd_get_info(&sd) == ESP_OK && sd.mounted);
    char sub_right[80];
    snprintf(sub_right, sizeof(sub_right), "%s · %s",
             sd_ok ? "TF 卡就绪" : "内置存储",
             (current && current->title) ? current->title : "桌面");
    ui_text_vc(fb, right_x, 104, 24, sub_right, EPD_DRAW_ALIGN_RIGHT, false);

    ui_hairline(fb, QM_HEADER_RULE_Y, UI_MARGIN, ui_content_width(), UI_GRAY_LIGHT);

    // 分组 1：快捷开关（2×3 网格）。/ Group 1: Quick toggles (2x3 grid).
    ui_text(fb, UI_MARGIN, QM_SEC1_LABEL_Y, 26, "快捷控制", EPD_DRAW_ALIGN_LEFT, false);

    const bool ble_on = app_settings_ble_enabled();
    const bool muted = app_settings_muted();
    const bool led_on = app_settings_charge_led();
    const bool pickup_on = app_settings_pickup_wake();
    char timeout_buf[24];
    fmt_timeout_short(timeout_buf, sizeof(timeout_buf), app_settings_auto_sleep_sec());

    draw_toggle_tile(fb, qm_ctrl_rect(QM_CTRL_BLE), "蓝牙伴侣",
                     ble_on ? "开启" : "关闭", ble_on, qm->pressed_ctrl == QM_CTRL_BLE);
    draw_toggle_tile(fb, qm_ctrl_rect(QM_CTRL_MUTE), "全局静音",
                     muted ? "已静音" : "有声", muted, qm->pressed_ctrl == QM_CTRL_MUTE);
    draw_toggle_tile(fb, qm_ctrl_rect(QM_CTRL_LED), "充电指示",
                     led_on ? "开启" : "关闭", led_on, qm->pressed_ctrl == QM_CTRL_LED);
    draw_toggle_tile(fb, qm_ctrl_rect(QM_CTRL_PICKUP), "拿起唤醒",
                     pickup_on ? "开启" : "关闭", pickup_on, qm->pressed_ctrl == QM_CTRL_PICKUP);
    draw_toggle_tile(fb, qm_ctrl_rect(QM_CTRL_AUTOSLEEP), "自动锁屏",
                     timeout_buf, app_settings_auto_sleep_sec() != 0,
                     qm->pressed_ctrl == QM_CTRL_AUTOSLEEP);
    draw_toggle_tile(fb, qm_ctrl_rect(QM_CTRL_SAVER), "锁屏样式",
                     saver_short_name(app_settings_saver_style()), false,
                     qm->pressed_ctrl == QM_CTRL_SAVER);

    // 分组 2：阅读与显示即时调节。/ Group 2: Live reading & display controls.
    ui_text(fb, UI_MARGIN, QM_SEC2_LABEL_Y, 26, "阅读与显示", EPD_DRAW_ALIGN_LEFT, false);

    char px_dec[32], px_inc[32], ref_lbl[32], hud_lbl[40], rot_lbl[40];
    const unsigned px = (unsigned)app_settings_book_px();
    snprintf(px_dec, sizeof(px_dec), "字号 - (%u)", px);
    snprintf(px_inc, sizeof(px_inc), "字号 + (%u)", px);
    snprintf(ref_lbl, sizeof(ref_lbl), "刷新：%s", refresh_short_name(app_settings_book_refresh()));
    snprintf(hud_lbl, sizeof(hud_lbl), "阅读栏：%s",
             hud_mode_name(app_settings_book_top_bar(), app_settings_book_bottom_bar()));
    snprintf(rot_lbl, sizeof(rot_lbl), "方向：%s",
             rotation_short_name(app_settings_book_rotation()));

    draw_cell_btn(fb, qm_ctrl_rect(QM_CTRL_PX_DEC), px_dec, qm->pressed_ctrl == QM_CTRL_PX_DEC);
    draw_cell_btn(fb, qm_ctrl_rect(QM_CTRL_PX_INC), px_inc, qm->pressed_ctrl == QM_CTRL_PX_INC);
    draw_cell_btn(fb, qm_ctrl_rect(QM_CTRL_REFRESH), ref_lbl, qm->pressed_ctrl == QM_CTRL_REFRESH);
    draw_cell_btn(fb, qm_ctrl_rect(QM_CTRL_HUD), hud_lbl, qm->pressed_ctrl == QM_CTRL_HUD);
    draw_cell_btn(fb, qm_ctrl_rect(QM_CTRL_ROTATION), rot_lbl, qm->pressed_ctrl == QM_CTRL_ROTATION);

    // 分组 3：系统快捷操作。/ Group 3: Quick system actions.
    ui_text(fb, UI_MARGIN, QM_SEC3_LABEL_Y, 26, "快捷操作", EPD_DRAW_ALIGN_LEFT, false);

    const bool on_home = (current == app_home_page());
    draw_cell_btn(fb, qm_ctrl_rect(QM_CTRL_CLEAN), "全屏强刷", qm->pressed_ctrl == QM_CTRL_CLEAN);
    draw_cell_btn(fb, qm_ctrl_rect(QM_CTRL_LOCK), "立即锁屏", qm->pressed_ctrl == QM_CTRL_LOCK);
    draw_cell_btn(fb, qm_ctrl_rect(QM_CTRL_TRANSFER), "无线传书", qm->pressed_ctrl == QM_CTRL_TRANSFER);
    draw_cell_btn(fb, qm_ctrl_rect(QM_CTRL_NAV), on_home ? "打开图书" : "回到桌面",
                  qm->pressed_ctrl == QM_CTRL_NAV);

    // 卡片底部上推指示横条。/ Bottom pull-up pill indicator inside card.
    ui_fill_round_rect(fb, (EpdRect){
        .x = (UI_LOCK_WIDTH - 76) / 2,
        .y = QM_HANDLE_Y,
        .width = 76,
        .height = 6,
    }, 3, UI_GRAY_BLACK);

    // 遮罩区收起按钮。/ Scrim dismiss pill button.
    EpdRect dismiss = qm_ctrl_rect(QM_CTRL_DISMISS);
    ui_fill_round_rect(fb, dismiss, UI_BTN_RADIUS, UI_GRAY_WHITE);
    draw_cell_btn(fb, dismiss, "▲ 上滑或点此收起", qm->pressed_ctrl == QM_CTRL_DISMISS);
}

static app_redraw_t commit_qm_ctrl(
    ui_quick_menu_t *qm, app_ctx_t *ctx, const app_desc_t *current, int ctrl, bool *out_close
) {
    switch (ctrl) {
        case QM_CTRL_TIME:
            app_settings_open_time_edit_next();
            *out_close = true;
            if (current != app_settings_page()) {
                ctx->request_app = app_settings_page();
            } else if (current->on_enter != NULL) {
                current->on_enter(ctx);
            }
            return APP_REDRAW_NONE;

        case QM_CTRL_BLE:
            app_ble_set_enabled(!app_settings_ble_enabled());
            return APP_REDRAW_PAGE;

        case QM_CTRL_MUTE:
            app_settings_set_muted(!app_settings_muted());
            return APP_REDRAW_PAGE;

        case QM_CTRL_LED:
            app_settings_set_charge_led(!app_settings_charge_led());
            return APP_REDRAW_PAGE;

        case QM_CTRL_PICKUP:
            app_settings_set_pickup_wake(!app_settings_pickup_wake());
            return APP_REDRAW_PAGE;

        case QM_CTRL_AUTOSLEEP: {
            uint16_t cur = app_settings_auto_sleep_sec();
            size_t count = sizeof(k_sleep_timeouts) / sizeof(k_sleep_timeouts[0]);
            size_t idx = 0;
            while (idx + 1 < count && k_sleep_timeouts[idx] != cur) ++idx;
            app_settings_set_auto_sleep_sec(k_sleep_timeouts[(idx + 1) % count]);
            return APP_REDRAW_PAGE;
        }

        case QM_CTRL_SAVER:
            app_settings_set_saver_style(
                (app_saver_style_t)((app_settings_saver_style() + 1) % 5));
            return APP_REDRAW_PAGE;

        case QM_CTRL_PX_DEC: {
            uint8_t px = app_settings_book_px();
            app_settings_set_book_px(px <= 36 ? 72 : (uint8_t)(px - 4));
            qm->px_changed = true;
            qm->book_dirty = true;
            return APP_REDRAW_PAGE;
        }

        case QM_CTRL_PX_INC: {
            uint8_t px = app_settings_book_px();
            app_settings_set_book_px(px >= 72 ? 36 : (uint8_t)(px + 4));
            qm->px_changed = true;
            qm->book_dirty = true;
            return APP_REDRAW_PAGE;
        }

        case QM_CTRL_REFRESH:
            app_settings_set_book_refresh((uint8_t)((app_settings_book_refresh() + 1) % 3));
            qm->book_dirty = true;
            return APP_REDRAW_PAGE;

        case QM_CTRL_HUD: {
            const bool top = app_settings_book_top_bar();
            const bool bot = app_settings_book_bottom_bar();
            if (top && bot) {
                app_settings_set_book_top_bar(true);
                app_settings_set_book_bottom_bar(false);
            } else if (top) {
                app_settings_set_book_top_bar(false);
                app_settings_set_book_bottom_bar(true);
            } else if (bot) {
                app_settings_set_book_top_bar(false);
                app_settings_set_book_bottom_bar(false);
            } else {
                app_settings_set_book_top_bar(true);
                app_settings_set_book_bottom_bar(true);
            }
            qm->book_dirty = true;
            return APP_REDRAW_PAGE;
        }

        case QM_CTRL_ROTATION:
            app_settings_set_book_rotation((uint8_t)((app_settings_book_rotation() + 1) % 3));
            qm->book_dirty = true;
            return APP_REDRAW_PAGE;

        case QM_CTRL_CLEAN:
            *out_close = true;
            return APP_REDRAW_NONE;

        case QM_CTRL_LOCK:
            *out_close = true;
            ctx->request_lock = true;
            return APP_REDRAW_NONE;

        case QM_CTRL_TRANSFER:
            *out_close = true;
            if (current != &app_transfer) {
                ctx->request_app = &app_transfer;
            }
            return APP_REDRAW_NONE;

        case QM_CTRL_NAV:
            *out_close = true;
            if (current == app_home_page()) {
                ctx->request_app = &app_book;
            } else {
                ctx->request_app = app_home_page();
            }
            return APP_REDRAW_NONE;

        case QM_CTRL_DISMISS:
            *out_close = true;
            return APP_REDRAW_NONE;

        default:
            return APP_REDRAW_NONE;
    }
}

app_redraw_t ui_quick_menu_on_gesture(
    ui_quick_menu_t *qm, app_ctx_t *ctx, const app_desc_t *current,
    const ui_gesture_event_t *event, bool *out_close
) {
    if (out_close) *out_close = false;
    if (!qm || !qm->open || !ctx || !event || !out_close) return APP_REDRAW_NONE;

    if (event->type == UI_GESTURE_SWIPE_U) {
        qm->pressed_ctrl = QM_CTRL_NONE;
        *out_close = true;
        return APP_REDRAW_NONE;
    }

    if (event->type == UI_GESTURE_PRESS) {
        EpdRect rect = { 0 };
        int hit = qm_hit_ctrl(event->x0, event->y0, &rect);
        qm->pressed_ctrl = hit;
        if (hit != QM_CTRL_NONE && ctx->fb) {
            qm->dirty_area = rect;
            ui_quick_menu_render(qm, ctx, current, ctx->fb);
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }

    if (event->type == UI_GESTURE_CANCEL) {
        if (qm->pressed_ctrl == QM_CTRL_NONE) return APP_REDRAW_NONE;
        qm->pressed_ctrl = QM_CTRL_NONE;
        if (ctx->fb && qm->dirty_area.width > 0) {
            ui_quick_menu_render(qm, ctx, current, ctx->fb);
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }

    if (event->type != UI_GESTURE_TAP) {
        if (qm->pressed_ctrl != QM_CTRL_NONE) {
            qm->pressed_ctrl = QM_CTRL_NONE;
            if (ctx->fb && qm->dirty_area.width > 0) {
                ui_quick_menu_render(qm, ctx, current, ctx->fb);
                return APP_REDRAW_AREA;
            }
        }
        return APP_REDRAW_NONE;
    }

    const int prev = qm->pressed_ctrl;
    qm->pressed_ctrl = QM_CTRL_NONE;
    EpdRect r0 = { 0 }, r1 = { 0 };
    const int h0 = qm_hit_ctrl(event->x0, event->y0, &r0);
    const int h1 = qm_hit_ctrl(event->x, event->y, &r1);
    if (h0 != QM_CTRL_NONE && h0 == h1) {
        return commit_qm_ctrl(qm, ctx, current, h1, out_close);
    }

    // 点按卡片外遮罩空白区域同样直接收起控制中心。
    // Tapping anywhere in the scrim outside the card dismisses the overlay.
    if (!ui_rect_hit(qm_card_rect(), event->x0, event->y0) &&
        !ui_rect_hit(qm_card_rect(), event->x, event->y)) {
        *out_close = true;
        return APP_REDRAW_NONE;
    }

    if (prev != QM_CTRL_NONE && ctx->fb && qm->dirty_area.width > 0) {
        ui_quick_menu_render(qm, ctx, current, ctx->fb);
        return APP_REDRAW_AREA;
    }
    return APP_REDRAW_NONE;
}

app_redraw_t ui_quick_menu_on_key(
    ui_quick_menu_t *qm, app_ctx_t *ctx, const app_desc_t *current,
    int key, bool *out_close
) {
    if (out_close) *out_close = false;
    if (!qm || !qm->open || !ctx || !out_close) return APP_REDRAW_NONE;
    qm->pressed_ctrl = QM_CTRL_NONE;
    if (key == UI_KEY_1) {
        ctx->key_handled = true;
        *out_close = true;
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_3) {
        ctx->key_handled = true;
        *out_close = true;
        if (current != app_settings_page()) {
            ctx->request_app = app_settings_page();
        }
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_NONE;
}
