/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pico OS 桌面入口。只负责启动器视觉与应用跳转，不接管底层硬件服务。
 *
 * Pico OS launcher. Owns the home surface and app routing, not board services.
 *
 * 冻结：触控点按与屏下三键都必须能完成主要导航；离线时桌面仍可用。
 * Frozen: touch and the three physical key zones both provide primary navigation;
 * the launcher remains usable while Wi-Fi is unavailable.
 */

#include "app.h"
#include "app_registry.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ttf_font.h"

extern const app_desc_t app_home;
extern const app_desc_t app_book;
extern const app_desc_t app_transfer;

#define HOME_TITLE "小纸 Pico"
#define HOME_SUBTITLE "Pico OS  ·  轻量、专注、联网"
#define HOME_TILE_COUNT 4
#define HOME_TILE_Y (UI_CONTENT_TOP + 48)
#define HOME_TILE_H 340

enum { TILE_BOOKS = 0, TILE_TRANSFER, TILE_DEVICE, TILE_APPS };
static int s_focus;

static EpdRect tile_rect(int index) {
    return ui_grid_rect(index % 2, 2, index / 2, HOME_TILE_Y, HOME_TILE_H);
}

static const char* tile_title(int index) {
    static const char* const titles[HOME_TILE_COUNT] = {
        "图书", "Wi-Fi 传输", "设备状态", "应用与工具"
    };
    return titles[index];
}

static const char* tile_detail(int index) {
    static const char* const details[HOME_TILE_COUNT] = {
        "继续阅读 TXT / EPUB", "连接网络，同步文件",
        "电量、存储与系统信息", "打开 Pico OS 应用菜单"
    };
    return details[index];
}

static void draw_tile(uint8_t* fb, int index) {
    EpdRect r = tile_rect(index);
    ui_draw_choice_round_rect(fb, r, 18, index == s_focus);
    ui_text(fb, r.x + 24, r.y + 34, 48, tile_title(index),
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, r.x + 24, r.y + 112, UI_PX_CAPTION, tile_detail(index),
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, r.x + r.width - 26, r.y + r.height - 52, 40, "›",
            EPD_DRAW_ALIGN_RIGHT, false);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    if (!ttf_font_ready()) {
        ui_draw_no_font_page(fb, false, false);
        return;
    }
    ui_clear_page(fb);
    ui_draw_header(fb, HOME_TITLE, HOME_SUBTITLE);
    ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 12, UI_PX_CAPTION,
            "今天想做什么？", EPD_DRAW_ALIGN_LEFT, false);
    for (int i = 0; i < HOME_TILE_COUNT; i++) draw_tile(fb, i);
    ui_text(fb, UI_MARGIN, UI_CONTENT_BOTTOM - 44, UI_PX_CAPTION,
            "触控选择  ·  KEY1 移动  ·  KEY2 确认  ·  KEY3 菜单",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_menu_handle(fb, false);
}

static void activate(app_ctx_t* ctx, int index) {
    switch (index) {
        case TILE_BOOKS: ctx->request_app = &app_book; break;
        case TILE_TRANSFER: ctx->request_app = &app_transfer; break;
        case TILE_DEVICE: ctx->request_app = &app_home; break;
        case TILE_APPS: ctx->request_menu = true; break;
        default: break;
    }
}

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    for (int i = 0; i < HOME_TILE_COUNT; i++) {
        if (!ui_rect_hit(tile_rect(i), touch->x, touch->y)) continue;
        s_focus = i;
        activate(ctx, i);
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key == UI_KEY_1) {
        s_focus = (s_focus + 1) % HOME_TILE_COUNT;
        return APP_REDRAW_PAGE;
    }
    if (key == UI_KEY_2) {
        activate(ctx, s_focus);
        return APP_REDRAW_PAGE;
    }
    if (key == UI_KEY_3) {
        ctx->request_menu = true;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key_long(app_ctx_t* ctx, int key) {
    (void)ctx;
    // 长按 KEY2 保留原厂约定的整屏 GC16 强刷出口。
    // Long-press KEY2 preserves the factory full-screen GC16 refresh exit.
    return key == UI_KEY_2 ? APP_REDRAW_FULL : APP_REDRAW_NONE;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    if (s_focus < 0 || s_focus >= HOME_TILE_COUNT) s_focus = 0;
}

const app_desc_t app_pico_home = {
    .title = "小纸 Pico",
    .detail = "Pico OS 桌面",
    .owns_keys = true,
    .render = render,
    .on_enter = on_enter,
    .on_touch = on_touch,
    .on_key = on_key,
    .on_key_long = on_key_long,
};
