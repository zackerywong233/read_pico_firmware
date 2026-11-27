/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：极简 2px 线框图标实现，仅依赖基础矩形与像素绘制原语，适用于墨水屏清晰显示。
 *
 * English: Minimalist 2px wireframe icons built on basic rect/pixel primitives for crisp e-ink display.
 */

#include "ui_icons.h"

#include <stdlib.h>

#include "epdiy.h"
#include "ui_kit.h"

static void fill_box(uint8_t* fb, int x, int y, int w, int h, uint8_t c) {
    if (w <= 0 || h <= 0) return;
    epd_fill_rect((EpdRect){ .x = x, .y = y, .width = w, .height = h }, c, fb);
}

static void stroke_rect(uint8_t* fb, int x, int y, int w, int h, int t) {
    if (w <= 0 || h <= 0 || t <= 0) return;
    fill_box(fb, x, y, w, t, UI_GRAY_BLACK);
    fill_box(fb, x, y + h - t, w, t, UI_GRAY_BLACK);
    fill_box(fb, x, y, t, h, UI_GRAY_BLACK);
    fill_box(fb, x + w - t, y, t, h, UI_GRAY_BLACK);
}

static void stroke_round(uint8_t* fb, int x, int y, int w, int h, int r) {
    EpdRect outer = { .x = x, .y = y, .width = w, .height = h };
    ui_draw_round_rect(fb, outer, r, UI_GRAY_BLACK);
    EpdRect inner = ui_inset_rect(outer, 1);
    if (inner.width > 4 && inner.height > 4) {
        ui_draw_round_rect(fb, inner, r > 1 ? r - 1 : 0, UI_GRAY_BLACK);
    }
}

static void draw_line_thick(uint8_t* fb, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0);
    int dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    while (true) {
        fill_box(fb, x0, y0, 2, 2, UI_GRAY_BLACK);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void stroke_circle(uint8_t* fb, int cx, int cy, int r) {
    int f = 1 - r;
    int dx = 1;
    int dy = -2 * r;
    int x = 0;
    int y = r;
    while (x <= y) {
        fill_box(fb, cx + x, cy + y, 2, 2, UI_GRAY_BLACK);
        fill_box(fb, cx - x, cy + y, 2, 2, UI_GRAY_BLACK);
        fill_box(fb, cx + x, cy - y, 2, 2, UI_GRAY_BLACK);
        fill_box(fb, cx - x, cy - y, 2, 2, UI_GRAY_BLACK);
        fill_box(fb, cx + y, cy + x, 2, 2, UI_GRAY_BLACK);
        fill_box(fb, cx - y, cy + x, 2, 2, UI_GRAY_BLACK);
        fill_box(fb, cx + y, cy - x, 2, 2, UI_GRAY_BLACK);
        fill_box(fb, cx - y, cy - x, 2, 2, UI_GRAY_BLACK);
        if (f >= 0) { y--; dy += 2; f += dy; }
        x++; dx += 2; f += dx;
    }
}

void ui_draw_battery_icon(
    uint8_t* fb, int x, int y, int w, int h, int pct, bool charging
) {
    if (!fb || w < 14 || h < 10) return;
    const int cap_w = 3;
    const int body_w = w - cap_w - 1;
    stroke_rect(fb, x, y, body_w, h, 2);
    fill_box(fb, x + body_w + 1, y + h / 4, cap_w, h - (h / 4) * 2, UI_GRAY_BLACK);

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    const int inner_w = body_w - 6;
    const int inner_h = h - 6;
    const int fill_w = (inner_w * pct + 50) / 100;
    if (fill_w > 0 && inner_h > 0) {
        fill_box(fb, x + 3, y + 3, fill_w, inner_h,
                 charging ? UI_GRAY_LIGHT : UI_GRAY_BLACK);
    }
    if (charging) {
        // 充电闪电斜线。/ Charging bolt stroke.
        int mx = x + body_w / 2;
        int my = y + h / 2;
        draw_line_thick(fb, mx + 2, y + 2, mx - 2, my);
        draw_line_thick(fb, mx - 2, my, mx + 2, my);
        draw_line_thick(fb, mx + 2, my, mx - 2, y + h - 3);
    }
}

void ui_draw_icon(uint8_t* fb, int cx, int cy, int size, ui_icon_id_t id) {
    if (!fb || size < 24) return;
    const int half = size / 2;
    const int x0 = cx - half;
    const int y0 = cy - half;
    const int s = size;

    switch (id) {
        case UI_ICON_BOOK:
            // 打开的书本。/ Open book.
            stroke_round(fb, x0 + 6, y0 + 10, s / 2 - 6, s - 20, 4);
            stroke_round(fb, cx, y0 + 10, s / 2 - 6, s - 20, 4);
            fill_box(fb, x0 + 13, y0 + 22, s / 2 - 20, 2, UI_GRAY_BLACK);
            fill_box(fb, x0 + 13, y0 + 32, s / 2 - 20, 2, UI_GRAY_BLACK);
            fill_box(fb, cx + 7, y0 + 22, s / 2 - 20, 2, UI_GRAY_BLACK);
            fill_box(fb, cx + 7, y0 + 32, s / 2 - 20, 2, UI_GRAY_BLACK);
            break;

        case UI_ICON_TRANSFER:
            // Wi-Fi 弧与上下传输箭头。/ Transfer arrows in box.
            stroke_round(fb, x0 + 8, y0 + 10, s - 16, s - 20, 10);
            draw_line_thick(fb, cx - 10, cy + 12, cx - 10, cy - 12);
            draw_line_thick(fb, cx - 10, cy - 12, cx - 17, cy - 5);
            draw_line_thick(fb, cx - 10, cy - 12, cx - 3, cy - 5);
            draw_line_thick(fb, cx + 10, cy - 12, cx + 10, cy + 12);
            draw_line_thick(fb, cx + 10, cy + 12, cx + 3, cy + 5);
            draw_line_thick(fb, cx + 10, cy + 12, cx + 17, cy + 5);
            break;

        case UI_ICON_WEB:
            // 地球经纬网。/ Globe.
            stroke_circle(fb, cx, cy, half - 8);
            fill_box(fb, x0 + 8, cy - 1, s - 16, 2, UI_GRAY_BLACK);
            fill_box(fb, cx - 1, y0 + 8, 2, s - 16, UI_GRAY_BLACK);
            stroke_round(fb, cx - half / 2 + 2, y0 + 8, half - 4, s - 16, half / 2);
            break;

        case UI_ICON_FEEDS:
            // RSS 电波与书签点。/ RSS waves.
            stroke_round(fb, x0 + 8, y0 + 8, s - 16, s - 16, 10);
            fill_box(fb, x0 + 18, y0 + s - 24, 7, 7, UI_GRAY_BLACK);
            draw_line_thick(fb, x0 + 19, y0 + s - 34, x0 + 34, y0 + s - 19);
            draw_line_thick(fb, x0 + 19, y0 + s - 46, x0 + 46, y0 + s - 19);
            break;

        case UI_ICON_SKETCH:
            // 手写笔与纸张。/ Pen and pad.
            stroke_round(fb, x0 + 10, y0 + 8, s - 24, s - 16, 6);
            draw_line_thick(fb, cx - 2, cy + 8, x0 + s - 8, y0 + 14);
            draw_line_thick(fb, cx - 2, cy + 8, cx - 8, cy + 14);
            fill_box(fb, x0 + 18, y0 + 22, 16, 2, UI_GRAY_BLACK);
            fill_box(fb, x0 + 18, y0 + 32, 12, 2, UI_GRAY_BLACK);
            break;

        case UI_ICON_FOCUS:
            // 极简时钟。/ Clock.
            stroke_circle(fb, cx, cy, half - 8);
            draw_line_thick(fb, cx, cy, cx, cy - half + 18);
            draw_line_thick(fb, cx, cy, cx + half - 20, cy + 4);
            fill_box(fb, cx - 2, cy - 2, 5, 5, UI_GRAY_BLACK);
            break;

        case UI_ICON_PERSONAL:
            // 看板卡片布局。/ Dashboard cards.
            stroke_round(fb, x0 + 8, y0 + 8, s - 16, s - 16, 8);
            fill_box(fb, x0 + 14, y0 + 24, s - 28, 2, UI_GRAY_BLACK);
            stroke_rect(fb, x0 + 16, y0 + 32, 16, 18, 2);
            fill_box(fb, x0 + 38, y0 + 34, 14, 2, UI_GRAY_BLACK);
            fill_box(fb, x0 + 38, y0 + 42, 14, 2, UI_GRAY_BLACK);
            fill_box(fb, x0 + 38, y0 + 48, 10, 2, UI_GRAY_BLACK);
            break;

        case UI_ICON_AGENT:
            // 信封/收件箱。/ Inbox envelope.
            stroke_round(fb, x0 + 6, y0 + 14, s - 12, s - 28, 6);
            draw_line_thick(fb, x0 + 9, y0 + 17, cx, cy + 2);
            draw_line_thick(fb, x0 + s - 9, y0 + 17, cx, cy + 2);
            break;

        case UI_ICON_SETTINGS:
            // 齿轮（内外圆 + 8 齿）。/ Gear.
            stroke_circle(fb, cx, cy, half - 14);
            stroke_circle(fb, cx, cy, 8);
            fill_box(fb, cx - 3, y0 + 6, 6, 8, UI_GRAY_BLACK);
            fill_box(fb, cx - 3, y0 + s - 14, 6, 8, UI_GRAY_BLACK);
            fill_box(fb, x0 + 6, cy - 3, 8, 6, UI_GRAY_BLACK);
            fill_box(fb, x0 + s - 14, cy - 3, 8, 6, UI_GRAY_BLACK);
            break;

        case UI_ICON_FILES:
            // 文件夹。/ Folder.
            stroke_round(fb, x0 + 6, y0 + 18, s - 12, s - 28, 6);
            stroke_rect(fb, x0 + 10, y0 + 12, 22, 8, 2);
            fill_box(fb, x0 + 14, cy + 4, s - 28, 2, UI_GRAY_BLACK);
            break;

        case UI_ICON_FONT:
            // 字形 “A”。/ Glyph "A".
            stroke_round(fb, x0 + 8, y0 + 8, s - 16, s - 16, 8);
            draw_line_thick(fb, cx, y0 + 18, x0 + 20, y0 + s - 18);
            draw_line_thick(fb, cx, y0 + 18, x0 + s - 20, y0 + s - 18);
            fill_box(fb, x0 + 25, cy + 4, s - 50, 2, UI_GRAY_BLACK);
            break;

        case UI_ICON_HOME:
            // 设备信息卡片。/ Device info badge.
            stroke_round(fb, x0 + 12, y0 + 6, s - 24, s - 12, 8);
            fill_box(fb, cx - 2, y0 + 18, 4, 4, UI_GRAY_BLACK);
            fill_box(fb, cx - 2, y0 + 28, 4, 22, UI_GRAY_BLACK);
            break;

        case UI_ICON_SELFTEST:
            // 勾选盾牌/方框。/ Checkmark box.
            stroke_round(fb, x0 + 8, y0 + 8, s - 16, s - 16, 10);
            draw_line_thick(fb, x0 + 20, cy + 2, cx - 4, cy + 14);
            draw_line_thick(fb, cx - 4, cy + 14, x0 + s - 18, y0 + 22);
            break;

        case UI_ICON_SLEEP:
            // 弯月休眠。/ Crescent moon.
            stroke_circle(fb, cx, cy, half - 10);
            fill_box(fb, cx + 2, y0 + 10, half - 8, half - 4, UI_GRAY_WHITE);
            draw_line_thick(fb, cx + 2, y0 + 12, cx + 2, cy + 8);
            draw_line_thick(fb, cx + 2, cy + 8, cx + 16, cy + 8);
            break;

        case UI_ICON_SD:
            // TF 卡外形。/ MicroSD card.
            stroke_rect(fb, x0 + 14, y0 + 8, s - 28, s - 16, 2);
            fill_box(fb, x0 + 22, y0 + 14, 3, 10, UI_GRAY_BLACK);
            fill_box(fb, x0 + 29, y0 + 14, 3, 10, UI_GRAY_BLACK);
            fill_box(fb, x0 + 36, y0 + 14, 3, 10, UI_GRAY_BLACK);
            fill_box(fb, x0 + 43, y0 + 14, 3, 10, UI_GRAY_BLACK);
            break;

        case UI_ICON_POWER:
            // 电池大图标。/ Battery icon.
            ui_draw_battery_icon(fb, x0 + 8, cy - 14, s - 16, 28, 70, false);
            break;

        case UI_ICON_REFRESH:
            // 灰阶梯刷新块。/ Grayscale refresh bars.
            stroke_round(fb, x0 + 8, y0 + 10, s - 16, s - 20, 6);
            fill_box(fb, x0 + 14, y0 + 16, 12, s - 32, UI_GRAY_BLACK);
            fill_box(fb, x0 + 28, y0 + 16, 12, s - 32, UI_GRAY_LIGHT);
            stroke_rect(fb, x0 + 42, y0 + 16, 12, s - 32, 2);
            break;

        case UI_ICON_TOUCH:
            // 十字准星与圆。/ Touch crosshair.
            stroke_circle(fb, cx, cy, half - 12);
            stroke_circle(fb, cx, cy, 8);
            fill_box(fb, cx - 1, y0 + 6, 2, s - 12, UI_GRAY_BLACK);
            fill_box(fb, x0 + 6, cy - 1, s - 12, 2, UI_GRAY_BLACK);
            break;

        case UI_ICON_READING:
            // 多行正文页。/ Text lines page.
            stroke_round(fb, x0 + 12, y0 + 8, s - 24, s - 16, 6);
            fill_box(fb, x0 + 20, y0 + 20, s - 40, 2, UI_GRAY_BLACK);
            fill_box(fb, x0 + 20, y0 + 30, s - 40, 2, UI_GRAY_BLACK);
            fill_box(fb, x0 + 20, y0 + 40, s - 40, 2, UI_GRAY_BLACK);
            fill_box(fb, x0 + 20, y0 + 50, s - 50, 2, UI_GRAY_BLACK);
            break;

        case UI_ICON_AXIS:
            // 三轴箭头。/ 3-axis arrows.
            draw_line_thick(fb, cx, cy, cx, y0 + 10);
            draw_line_thick(fb, cx, cy, x0 + s - 10, cy + 12);
            draw_line_thick(fb, cx, cy, x0 + 10, cy + 12);
            fill_box(fb, cx - 3, cy - 3, 6, 6, UI_GRAY_BLACK);
            break;

        case UI_ICON_AXIS_LAB:
            // 波形诊断折线。/ Sensor waveform.
            stroke_round(fb, x0 + 6, y0 + 12, s - 12, s - 24, 6);
            draw_line_thick(fb, x0 + 12, cy, x0 + 22, cy);
            draw_line_thick(fb, x0 + 22, cy, x0 + 30, cy - 14);
            draw_line_thick(fb, x0 + 30, cy - 14, x0 + 40, cy + 14);
            draw_line_thick(fb, x0 + 40, cy + 14, x0 + 48, cy);
            draw_line_thick(fb, x0 + 48, cy, x0 + s - 12, cy);
            break;

        case UI_ICON_PMU:
            // 芯片与引脚。/ Chip with pins.
            stroke_rect(fb, cx - 14, cy - 14, 28, 28, 2);
            for (int i = -8; i <= 8; i += 8) {
                fill_box(fb, cx + i - 1, cy - 22, 2, 8, UI_GRAY_BLACK);
                fill_box(fb, cx + i - 1, cy + 14, 2, 8, UI_GRAY_BLACK);
                fill_box(fb, cx - 22, cy + i - 1, 8, 2, UI_GRAY_BLACK);
                fill_box(fb, cx + 14, cy + i - 1, 8, 2, UI_GRAY_BLACK);
            }
            break;

        case UI_ICON_KEY:
            // 实体按键。/ Physical key.
            stroke_round(fb, x0 + 10, cy - 16, s - 20, 32, 10);
            stroke_circle(fb, cx, cy, 8);
            break;

        case UI_ICON_IOE:
            // 2x4 排针。/ 2x4 GPIO header.
            stroke_round(fb, x0 + 8, y0 + 14, s - 16, s - 28, 6);
            for (int c = 0; c < 4; c++) {
                int px = x0 + 18 + c * 11;
                fill_box(fb, px, cy - 9, 5, 5, UI_GRAY_BLACK);
                fill_box(fb, px, cy + 4, 5, 5, UI_GRAY_BLACK);
            }
            break;

        default:
            stroke_round(fb, x0 + 10, y0 + 10, s - 20, s - 20, 8);
            break;
    }
}
