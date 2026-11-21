/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pico OS 桌面入口。只负责启动器视觉与应用跳转，不接管底层硬件服务。
 *
 * Pico OS launcher. Owns the home surface and app routing, not board services.
 *
 * 决策变更：网页阅读成为桌面直达应用；桌面扩为六格，保留离线页面与完整应用菜单。
 * 决策变更：统一全系统三键逻辑，桌面不再接管三键；触控直达六格应用，KEY2 整屏强刷、KEY3 开关应用菜单。
 * Decision change: make Web Reader a direct launcher tile; use a six-tile grid while keeping offline pages and the full app menu.
 * Decision change: align with the unified physical key contract; home no longer overrides keys—touch opens tiles while KEY2 refreshes and KEY3 toggles the app menu.
 * 冻结：离线桌面保留设置入口和应用菜单；屏下三键遵循主循环统一契约。
 * Frozen: Offline home retains Settings and the app menu; physical keys follow the shared loop contract.
 */

#include "app.h"
#include "app_registry.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ttf_font.h"

extern const app_desc_t app_home;
extern const app_desc_t app_book;
extern const app_desc_t app_transfer;
extern const app_desc_t app_web;
extern const app_desc_t app_personal;
extern const app_desc_t app_sketch;
extern const app_desc_t app_focus;

#define HOME_TITLE "小纸 Pico"
#define HOME_SUBTITLE "Pico OS  ·  轻量、专注、联网"
#define HOME_TILE_COUNT 6
#define HOME_TILE_Y (UI_CONTENT_TOP + 48)
#define HOME_TILE_H 250

enum { TILE_BOOKS = 0, TILE_TRANSFER, TILE_WEB, TILE_FOCUS, TILE_SKETCH, TILE_SETTINGS };

static EpdRect tile_rect(int index) {
    return ui_grid_rect(index % 2, 2, index / 2, HOME_TILE_Y, HOME_TILE_H);
}

static const char* tile_title(int index) {
    static const char* const titles[HOME_TILE_COUNT] = {
        "图书", "Wi-Fi 传输", "网页阅读", "专注时钟", "随手便签", "设置 Settings"
    };
    return titles[index];
}

static const char* tile_detail(int index) {
    static const char* const details[HOME_TILE_COUNT] = {
        "继续阅读多种格式的图书", "连接网络，同步文件", "在线阅读或剪藏网页",
        "桌面时钟与番茄钟", "绘制并保存手写便签", "网络、阅读与设备设置"
    };
    return details[index];
}

static void draw_tile(uint8_t* fb, int index) {
    EpdRect r = tile_rect(index);
    ui_draw_round_rect(fb, r, 18, UI_GRAY_BLACK);
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
            "触控选择  ·  KEY2 强刷  ·  KEY3 菜单",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_menu_handle(fb, false);
}

static void activate(app_ctx_t* ctx, int index) {
    switch (index) {
        case TILE_BOOKS: ctx->request_app = &app_book; break;
        case TILE_TRANSFER: ctx->request_app = &app_transfer; break;
        case TILE_WEB: ctx->request_app = &app_web; break;
        case TILE_FOCUS: ctx->request_app = &app_focus; break;
        case TILE_SKETCH: ctx->request_app = &app_sketch; break;
        case TILE_SETTINGS: ctx->request_app = app_settings_page(); break;
        default: break;
    }
}

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    for (int i = 0; i < HOME_TILE_COUNT; i++) {
        if (!ui_rect_hit(tile_rect(i), touch->x, touch->y)) continue;
        activate(ctx, i);
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_NONE;
}

const app_desc_t app_pico_home = {
    .title = "小纸 Pico",
    .detail = "Pico OS 桌面",
    .render = render,
    .on_touch = on_touch,
};
