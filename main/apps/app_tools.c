/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Second-level home for board demos and diagnostics.
 */

#include "app.h"
#include <stdio.h>
#include "app_registry.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TOOL_ROWS 6
#define TOOL_ROW_H ((UI_CONTENT_BOTTOM - UI_CONTENT_TOP - 92) / TOOL_ROWS)

static int s_focus;

static EpdRect row_rect(int row) {
    return (EpdRect){ UI_MARGIN, UI_CONTENT_TOP + row * TOOL_ROW_H,
        ui_content_width(), TOOL_ROW_H - UI_GAP };
}

static EpdRect nav_rect(bool next) {
    return (EpdRect){ next ? epd_rotated_display_width() / 2 : UI_MARGIN,
        UI_CONTENT_BOTTOM - 76, epd_rotated_display_width() / 2 - UI_MARGIN,
        64 };
}

static int page_count(void) {
    return (app_tool_count() + TOOL_ROWS - 1) / TOOL_ROWS;
}

static int clamp_page(int page) {
    if (page < 0) return 0;
    if (page >= page_count()) return page_count() - 1;
    return page;
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    int page = clamp_page(ctx->leaf);
    ctx->leaf = page;
    ui_clear_page(fb);
    char subtitle[48];
    snprintf(subtitle, sizeof(subtitle), "第 %d / %d 页  ·  底层诊断与演示", page + 1, page_count());
    ui_draw_header(fb, "开发者工具", subtitle);
    int first = page * TOOL_ROWS;
    int count = app_tool_count() - first;
    if (count > TOOL_ROWS) count = TOOL_ROWS;
    for (int row = 0; row < count; row++) {
        const app_desc_t* item = app_tool_at(first + row);
        EpdRect rect = row_rect(row);
        ui_draw_choice_round_rect(fb, rect, UI_BTN_RADIUS, s_focus == first + row);
        ui_text(fb, rect.x + 24, rect.y + 24, 36, item->title, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, rect.x + 24, rect.y + 70, UI_PX_CAPTION, item->detail, EPD_DRAW_ALIGN_LEFT, false);
    }
    ui_draw_button(fb, nav_rect(false), "上一页", page > 0);
    ui_draw_button(fb, nav_rect(true), "下一页", page + 1 < page_count());
    ui_draw_menu_handle(fb, false);
}

static app_redraw_t activate(app_ctx_t* ctx, int index) {
    const app_desc_t* next = app_tool_at(index);
    if (next) ctx->request_app = next;
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    if (ui_menu_handle_hit_test(touch->x, touch->y)) {
        ctx->request_menu = true;
        return APP_REDRAW_PAGE;
    }
    int page = clamp_page(ctx->leaf);
    if (ui_rect_hit(nav_rect(false), touch->x, touch->y) && page > 0) {
        ctx->leaf = page - 1;
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(nav_rect(true), touch->x, touch->y) && page + 1 < page_count()) {
        ctx->leaf = page + 1;
        return APP_REDRAW_PAGE;
    }
    int first = page * TOOL_ROWS;
    int count = app_tool_count() - first;
    if (count > TOOL_ROWS) count = TOOL_ROWS;
    for (int row = 0; row < count; row++) {
        if (ui_rect_hit(row_rect(row), touch->x, touch->y)) {
            s_focus = first + row;
            return activate(ctx, s_focus);
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key == UI_KEY_1) {
        s_focus = (s_focus + 1) % app_tool_count();
        ctx->leaf = s_focus / TOOL_ROWS;
        return APP_REDRAW_PAGE;
    }
    if (key == UI_KEY_2) return activate(ctx, s_focus);
    if (key == UI_KEY_3) {
        ctx->request_menu = true;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key_long(app_ctx_t* ctx, int key) {
    (void)ctx;
    return key == UI_KEY_2 ? APP_REDRAW_FULL : APP_REDRAW_NONE;
}

static void on_enter(app_ctx_t* ctx) {
    ctx->leaf = clamp_page(ctx->leaf);
    if (s_focus < 0 || s_focus >= app_tool_count()) s_focus = 0;
}

const app_desc_t app_tools = {
    .title = "开发者工具",
    .detail = "硬件诊断与演示页面",
    .enter_full = true,
    .owns_keys = true,
    .render = render,
    .on_enter = on_enter,
    .on_touch = on_touch,
    .on_key = on_key,
    .on_key_long = on_key_long,
};
