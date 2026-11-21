/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pico OS 系统工具索引。集中承载硬件诊断与开发辅助页，不把它们放在一级应用列表。
 *
 * Pico OS system tools index. Keeps hardware diagnostics and development
 * helpers below the root app list.
 *
 * 冻结：此页只负责导航；自检仍从一级应用直接进入。
 * Frozen: This page only routes to tools; self-test stays at the root level.
 */

#include "app.h"

#include <stdio.h>

#include "ui_kit.h"
#include "ui_menu.h"
#include "ttf_font.h"

extern const app_desc_t app_refresh;
extern const app_desc_t app_reading;
extern const app_desc_t app_touch;
extern const app_desc_t app_axis;
extern const app_desc_t app_axis_lab;
extern const app_desc_t app_power;
extern const app_desc_t app_pmu;
extern const app_desc_t app_key;
extern const app_desc_t app_sleep;
extern const app_desc_t app_sd;
extern const app_desc_t app_font_pick;
extern const app_desc_t app_ioe;

#define TOOLS_PER_PAGE 4
#define TOOLS_PAGE_COUNT 3
#define TOOLS_TILE_Y (UI_CONTENT_TOP + 48)
#define TOOLS_TILE_H 330

typedef struct {
    const app_desc_t* app;
    const char* title;
    const char* detail;
} tool_item_t;

static const tool_item_t s_tools[] = {
    { &app_refresh, "刷新策略", "波形、灰阶与刷新耗时" },
    { &app_reading, "内置阅读", "检查字体与翻页效果" },
    { &app_touch, "触摸测试", "连续刷新与触摸唤醒" },
    { &app_axis, "加速度计", "读数、敲击与方向" },
    { &app_axis_lab, "传感器诊断", "采样参数与自测" },
    { &app_power, "电源状态", "电池与屏幕电源轨" },
    { &app_pmu, "PMU 协议", "状态、事件与命令" },
    { &app_key, "按键事件", "查看侧键原始事件" },
    { &app_sleep, "睡眠唤醒", "检查休眠与唤醒路径" },
    { &app_sd, "TF 卡与蜂鸣器", "存储状态与蜂鸣器" },
    { &app_font_pick, "字体管理", "预览并切换卡上字体" },
    { &app_ioe, "扩展口", "查看 IO 扩展状态" },
};

#define TOOLS_COUNT ((int)(sizeof(s_tools) / sizeof(s_tools[0])))
static int s_page;

static EpdRect tile_rect(int index) {
    const int local = index % TOOLS_PER_PAGE;
    return ui_grid_rect(local % 2, 2, local / 2, TOOLS_TILE_Y, TOOLS_TILE_H);
}

static void draw_tile(uint8_t* fb, int index) {
    const EpdRect rect = tile_rect(index);
    ui_draw_round_rect(fb, rect, UI_BTN_RADIUS, UI_GRAY_BLACK);
    ui_text(fb, rect.x + 24, rect.y + 38, UI_PX_LABEL,
            s_tools[index].title, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, rect.x + 24, rect.y + 112, UI_PX_CAPTION,
            s_tools[index].detail, EPD_DRAW_ALIGN_LEFT, false);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    (void)ctx;
    if (!ttf_font_ready()) {
        ui_draw_no_font_page(fb, false, false);
        return;
    }
    const int page = s_page;
    const int first = page * TOOLS_PER_PAGE;
    char subtitle[64];
    snprintf(subtitle, sizeof(subtitle), "硬件诊断与开发辅助  ·  %d / %d",
             page + 1, TOOLS_PAGE_COUNT);
    ui_clear_page(fb);
    ui_draw_header(fb, "系统工具 System Tools", subtitle);
    for (int i = 0; i < TOOLS_PER_PAGE; i++) {
        const int index = first + i;
        if (index < TOOLS_COUNT) draw_tile(fb, index);
    }
    ui_draw_button(fb, ui_bar_rect(0, 2), "上一页 Prev", page > 0);
    ui_draw_button(fb, ui_bar_rect(1, 2), "下一页 Next", page + 1 < TOOLS_PAGE_COUNT);
    ui_text(fb, UI_MARGIN, UI_CONTENT_BOTTOM - 44, UI_PX_CAPTION,
            "KEY1 返回  ·  KEY2 强刷  ·  KEY3 菜单",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_menu_handle(fb, false);
}

static app_redraw_t on_touch(app_ctx_t* ctx, const cst836u_touch_t* touch) {
    for (int i = 0; i < TOOLS_PER_PAGE; i++) {
        const int index = s_page * TOOLS_PER_PAGE + i;
        if (index >= TOOLS_COUNT || !ui_rect_hit(tile_rect(i), touch->x, touch->y)) continue;
        ctx->request_app = s_tools[index].app;
        return APP_REDRAW_NONE;
    }
    const int bar = ui_bar_hit(touch->x, touch->y, 2);
    if (bar == 0 && s_page > 0) {
        s_page--;
        return APP_REDRAW_PAGE;
    }
    if (bar == 1 && s_page + 1 < TOOLS_PAGE_COUNT) {
        s_page++;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    if (s_page < 0 || s_page >= TOOLS_PAGE_COUNT) s_page = 0;
}

const app_desc_t app_system_tools = {
    .title = "系统工具 System Tools",
    .detail = "硬件诊断与开发辅助",
    .render = render,
    .on_enter = on_enter,
    .on_touch = on_touch,
};
