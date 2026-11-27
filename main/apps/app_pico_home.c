/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：Pico OS 3 页九宫格桌面（9 + 9 + 6 = 24 格），顶部统一实时状态栏，底部页码圆点。
 *
 * English: Pico OS 3-page 3x3 grid desktop (9 + 9 + 6 = 24 tiles) with unified
 * live top status bar and page indicator dots.
 *
 * 冻结：全部 24 个应用与工具平铺为 3 页九宫格，取消全屏应用列表与二级系统工具页；
 * 顶部状态栏左时间、中「小纸 Pico」（含顶边下拉提示条）、右电量；点按顶栏同样呼出快捷控制中心；
 * on_tick 按分钟与电量变化仅局部 DU 刷新顶栏，不重置空闲自动锁屏；
 * 支持触控、左右滑屏与左/右键翻页；第 1 页长按中键无动作，第 2/3 页长按中键回第 1 页。
 * Frozen: All 24 apps sit flat across 3 pages of the 3x3 grid; no full-screen list
 * menu or System Tools page. Top bar shows time, "小纸 Pico" (with pull-down cue), and
 * battery; tapping the top bar also opens the Quick Control Menu; on_tick locally DU-updates
 * the status bar on minute or battery changes without resetting idle sleep. Supports tap,
 * left/right swipe, and KEY1/KEY3 paging. Long KEY2 on page 1 is a no-op.
 */

#include <stdio.h>
#include <time.h>

#include "app.h"
#include "app_registry.h"
#include "read_pico_pmu.h"
#include "time_sync.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_icons.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define HOME_COLS 3
#define HOME_ROWS 3
#define HOME_PER_PAGE (HOME_COLS * HOME_ROWS)
#define HOME_PAGES 3

#define HOME_STATUS_H 58
#define HOME_GRID_TOP 104
#define HOME_TILE_W 190
#define HOME_TILE_H 275
#define HOME_COL_GAP 17
#define HOME_ROW_GAP 28
#define HOME_DOTS_Y 1076

#define HOME_STATUS_POLL_MS 5000
#define HOME_PMU_POLL_MS 30000

static int s_shown_min = -1;
static int s_shown_pct = -2;
static bool s_shown_charging;
static int64_t s_last_check_ms;
static int64_t s_last_pmu_ms;

static const char* const s_tile_labels[24] = {
    // 第 1 页（高频日常）/ Page 1
    "图书", "传书", "网页阅读",
    "订阅书库", "随手便签", "专注时钟",
    "我的看板", "Agent 收件", "设置",
    // 第 2 页（管理与设备）/ Page 2
    "文件管理", "字体管理", "设备概览",
    "设备自检", "睡眠唤醒", "存储与蜂鸣",
    "电源状态", "屏幕刷新", "触摸测试",
    // 第 3 页（硬件诊断）/ Page 3
    "阅读测试", "加速度计", "传感器诊断",
    "电源管理", "按键测试", "扩展口",
};

static const ui_icon_id_t s_tile_icons[24] = {
    UI_ICON_BOOK, UI_ICON_TRANSFER, UI_ICON_WEB,
    UI_ICON_FEEDS, UI_ICON_SKETCH, UI_ICON_FOCUS,
    UI_ICON_PERSONAL, UI_ICON_AGENT, UI_ICON_SETTINGS,
    UI_ICON_FILES, UI_ICON_FONT, UI_ICON_HOME,
    UI_ICON_SELFTEST, UI_ICON_SLEEP, UI_ICON_SD,
    UI_ICON_POWER, UI_ICON_REFRESH, UI_ICON_TOUCH,
    UI_ICON_READING, UI_ICON_AXIS, UI_ICON_AXIS_LAB,
    UI_ICON_PMU, UI_ICON_KEY, UI_ICON_IOE,
};

static int clamp_page(int leaf) {
    if (leaf < 0) return 0;
    if (leaf >= HOME_PAGES) return HOME_PAGES - 1;
    return leaf;
}

static int minute_stamp(const struct tm* tmv) {
    return (tmv->tm_year * 400 + tmv->tm_yday) * 1440 + tmv->tm_hour * 60 + tmv->tm_min;
}

static void read_status_snapshot(int* out_min, int* out_pct, bool* out_charging, struct tm* out_tm) {
    struct tm local = { 0 };
    if (app_time_now_local(&local, NULL)) {
        *out_min = minute_stamp(&local);
        if (out_tm) *out_tm = local;
    } else {
        *out_min = -1;
    }
    const pmu_snapshot_t* pmu = read_pico_pmu_get();
    if (pmu && pmu->present && pmu->status_ok) {
        int pct = (int)(pmu->soc_permille / 10U);
        if (pct > 100) pct = 100;
        *out_pct = pct;
        *out_charging = (pmu->charge_state == PMU_CHARGE_CHARGING);
    } else {
        *out_pct = -1;
        *out_charging = false;
    }
}

static EpdRect tile_rect(int local_index) {
    const int col = local_index % HOME_COLS;
    const int row = local_index / HOME_COLS;
    return (EpdRect){
        .x = UI_MARGIN + col * (HOME_TILE_W + HOME_COL_GAP),
        .y = HOME_GRID_TOP + row * (HOME_TILE_H + HOME_ROW_GAP),
        .width = HOME_TILE_W,
        .height = HOME_TILE_H,
    };
}

static void draw_status_bar(uint8_t* fb) {
    const int w = epd_rotated_display_width();
    const int cy = HOME_STATUS_H / 2 + 2;

    ui_clear_rect_fast(fb, (EpdRect){ .x = 0, .y = 0, .width = w, .height = HOME_STATUS_H });

    // 顶部下拉指示细条。/ Subtle top pull-down handle cue.
    ui_fill_round_rect(fb, (EpdRect){
        .x = w / 2 - 22,
        .y = 5,
        .width = 44,
        .height = 3,
    }, 1, UI_GRAY_LIGHT);

    int cur_min = -1;
    int pct = -1;
    bool charging = false;
    struct tm local = { 0 };
    read_status_snapshot(&cur_min, &pct, &charging, &local);
    s_shown_min = cur_min;
    s_shown_pct = pct;
    s_shown_charging = charging;

    // 左侧时间。/ Left: local time.
    char time_buf[16] = "--:--";
    if (cur_min >= 0) {
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d", local.tm_hour, local.tm_min);
    }
    ui_text_vc(fb, UI_MARGIN, cy, 28, time_buf, EPD_DRAW_ALIGN_LEFT, false);

    // 中间标题。/ Center: product name.
    ui_text_vc(fb, w / 2, cy, 32, "小纸 Pico", EPD_DRAW_ALIGN_CENTER, false);

    // 右侧电池图标与百分比。/ Right: battery icon and percentage.
    char bat_buf[16];
    if (pct >= 0) {
        snprintf(bat_buf, sizeof(bat_buf), "%d%%", pct);
    } else {
        snprintf(bat_buf, sizeof(bat_buf), "--%%");
    }
    ui_text_vc(fb, w - UI_MARGIN, cy, 26, bat_buf, EPD_DRAW_ALIGN_RIGHT, false);
    const int text_w = ttf_text_width_px(26, bat_buf);
    const int icon_w = 34;
    const int icon_h = 18;
    ui_draw_battery_icon(
        fb, w - UI_MARGIN - text_w - icon_w - 8, cy - icon_h / 2,
        icon_w, icon_h, pct >= 0 ? pct : 0, charging
    );

    ui_hairline(fb, HOME_STATUS_H, UI_MARGIN, ui_content_width(), UI_GRAY_LIGHT);
}

static void draw_tile(uint8_t* fb, int global_index, int local_index) {
    const EpdRect r = tile_rect(local_index);
    ui_draw_round_rect(fb, r, 18, UI_GRAY_BLACK);
    ui_draw_icon(fb, r.x + r.width / 2, r.y + 108, 72, s_tile_icons[global_index]);
    ui_text_vc(fb, r.x + r.width / 2, r.y + 216, 34,
               s_tile_labels[global_index], EPD_DRAW_ALIGN_CENTER, false);
}

static void draw_page_dots(uint8_t* fb, int page) {
    const int w = epd_rotated_display_width();
    const int gap = 28;
    const int start_x = w / 2 - gap;
    for (int i = 0; i < HOME_PAGES; i++) {
        const int cx = start_x + i * gap;
        if (i == page) {
            epd_fill_circle(cx, HOME_DOTS_Y, 7, UI_GRAY_BLACK, fb);
        } else {
            epd_draw_circle(cx, HOME_DOTS_Y, 6, UI_GRAY_BLACK, fb);
        }
    }
}

static void on_enter(app_ctx_t* ctx) {
    ctx->leaf = clamp_page(ctx->leaf);
    s_last_check_ms = ctx->now_ms;
    s_last_pmu_ms = ctx->now_ms;
    if (read_pico_pmu_ready()) {
        (void)read_pico_pmu_refresh();
    }
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    if (!ttf_font_ready()) {
        ui_draw_no_font_page(fb, false, false);
        return;
    }
    const int page = clamp_page(ctx->leaf);
    const int total = app_count();
    const int first = page * HOME_PER_PAGE;

    ui_clear_page(fb);
    draw_status_bar(fb);
    for (int i = 0; i < HOME_PER_PAGE; i++) {
        const int idx = first + i;
        if (idx >= total || idx >= 24) break;
        draw_tile(fb, idx, i);
    }
    draw_page_dots(fb, page);
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    const int page = clamp_page(ctx->leaf);
    if (ev->type == UI_GESTURE_SWIPE_L) {
        if (page + 1 < HOME_PAGES) {
            ctx->leaf = page + 1;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (ev->type == UI_GESTURE_SWIPE_R) {
        if (page > 0) {
            ctx->leaf = page - 1;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;

    if (ev->y0 < HOME_STATUS_H && ev->y < HOME_STATUS_H) {
        ctx->request_quick_menu = true;
        return APP_REDRAW_NONE;
    }

    const int total = app_count();
    const int first = page * HOME_PER_PAGE;
    for (int i = 0; i < HOME_PER_PAGE; i++) {
        const int idx = first + i;
        if (idx >= total) break;
        if (!ui_rect_hit(tile_rect(i), ev->x, ev->y)) continue;
        const app_desc_t* target = app_at(idx);
        if (target != NULL) ctx->request_app = target;
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    const int page = clamp_page(ctx->leaf);
    if (key == UI_KEY_1) {
        ctx->key_handled = true;
        if (page > 0) {
            ctx->leaf = page - 1;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_3) {
        ctx->key_handled = true;
        if (page + 1 < HOME_PAGES) {
            ctx->leaf = page + 1;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key_long(app_ctx_t* ctx, int key) {
    (void)key;
    ctx->key_handled = true;
    return APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (!ttf_font_ready() || ctx->fb == NULL) return APP_REDRAW_NONE;
    if (ctx->now_ms - s_last_check_ms < HOME_STATUS_POLL_MS) return APP_REDRAW_NONE;
    s_last_check_ms = ctx->now_ms;
    if (read_pico_pmu_ready() && ctx->now_ms - s_last_pmu_ms >= HOME_PMU_POLL_MS) {
        s_last_pmu_ms = ctx->now_ms;
        (void)read_pico_pmu_refresh();
    }
    int cur_min = -1;
    int cur_pct = -1;
    bool cur_charging = false;
    read_status_snapshot(&cur_min, &cur_pct, &cur_charging, NULL);
    if (cur_min != s_shown_min || cur_pct != s_shown_pct || cur_charging != s_shown_charging) {
        draw_status_bar(ctx->fb);
        return APP_REDRAW_AREA;
    }
    return APP_REDRAW_NONE;
}

static EpdRect area_hint(app_ctx_t* ctx) {
    (void)ctx;
    return (EpdRect){
        .x = 0,
        .y = 0,
        .width = epd_rotated_display_width(),
        .height = HOME_STATUS_H + 2,
    };
}

static void key_hints(app_ctx_t* ctx, ui_key_hints_t* out) {
    const int page = clamp_page(ctx->leaf);
    out->left = page > 0 ? "上一页" : NULL;
    out->center = page == 0 ? "刷新" : "刷新 · 长按首页";
    out->right = (page + 1 < HOME_PAGES) ? "下一页" : NULL;
}

const app_desc_t app_pico_home = {
    .title = "小纸 Pico",
    .detail = "小纸 Pico 桌面",
    .on_enter = on_enter,
    .render = render,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_key_long = on_key_long,
    .on_tick = on_tick,
    .area_hint = area_hint,
    .key_hints = key_hints,
};
