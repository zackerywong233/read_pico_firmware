/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：在 4bpp 墨水屏帧缓冲上绘制 5 阶灰度阅读热力图，供个人看板与锁屏复用。
 *
 * English: Paint a 5-level grayscale reading heatmap onto the 4bpp e-ink framebuffer for dashboard and lock screen.
 *
 * 冻结：纯绘制模块，禁止访问 NVS、文件系统、Wi-Fi 或 I2C。
 * Frozen: Pure drawing module; never access NVS, filesystem, Wi-Fi, or I2C.
 */
#include "ui_heatmap.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ttf_font.h"

#define HEATMAP_CARD_WEEKS 26
#define HEATMAP_DETAIL_WEEKS 18

// 5 阶墨水屏灰度：白(0xFF)、浅灰(0xD0)、中灰(0x90)、深灰(0x50)、纯黑(0x00)。
// 5-level e-ink grayscale: white, light, mid, dark, and solid black.
static const uint8_t k_level_fill[5] = { 0xFF, 0xD0, 0x90, 0x50, 0x00 };
static const uint8_t k_level0_border = 0xC0;
static const uint8_t k_level1_border = 0xA0;

static void fit_text(char* text, int px, int max_width) {
    while (*text && ttf_text_width_px(px, text) > max_width) {
        size_t n = strlen(text) - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = '\0';
    }
}

static unsigned rounded_minutes(uint32_t seconds, bool round_up_sub_min) {
    if (seconds == 0) return 0;
    if (round_up_sub_min && seconds < 60U) return 1;
    return (unsigned)(seconds / 60U);
}

static void format_duration(char* buf, size_t cap, uint32_t seconds, bool round_up_sub_min) {
    unsigned mins = rounded_minutes(seconds, round_up_sub_min);
    if (mins < 60U) {
        snprintf(buf, cap, "%u 分钟", mins);
        return;
    }
    unsigned hours = mins / 60U;
    unsigned rem = mins % 60U;
    if (rem == 0) snprintf(buf, cap, "%u 小时", hours);
    else snprintf(buf, cap, "%u 小时 %u 分", hours, rem);
}

static void draw_cell(uint8_t* fb, EpdRect cell, uint8_t level, bool is_today) {
    if (level > 4) level = 4;
    int radius = cell.width <= 18 ? 3 : 4;
    if (level == 0) {
        ui_draw_round_rect(fb, cell, radius, is_today ? UI_GRAY_BLACK : k_level0_border);
        return;
    }
    ui_fill_round_rect(fb, cell, radius, k_level_fill[level]);
    if (is_today && level < 3) {
        ui_draw_round_rect(fb, cell, radius, UI_GRAY_BLACK);
    } else if (level == 1) {
        ui_draw_round_rect(fb, cell, radius, k_level1_border);
    }
}

static void draw_legend_right(
    uint8_t* fb, int right_x, int center_y, int swatch, int gap, int font_px
) {
    ui_text_vc(fb, right_x, center_y, font_px, "多", EPD_DRAW_ALIGN_RIGHT, false);
    int text_pad = font_px + 8;
    int swatches_w = 5 * swatch + 4 * gap;
    int x0 = right_x - text_pad - swatches_w;
    int y0 = center_y - swatch / 2;
    for (uint8_t lvl = 0; lvl < 5; ++lvl) {
        EpdRect r = { x0 + (int)lvl * (swatch + gap), y0, swatch, swatch };
        draw_cell(fb, r, lvl, false);
    }
    ui_text_vc(fb, x0 - 8, center_y, font_px, "少", EPD_DRAW_ALIGN_RIGHT, false);
}

static uint16_t draw_grid(
    uint8_t* fb, const reading_heatmap_t* hm, int weeks,
    int grid_x, int grid_y, int cell, int gap,
    int label_x, int label_px,
    bool show_months, int month_y
) {
    static const char* const k_wd[7] = { "一", NULL, "三", NULL, "五", NULL, "日" };
    const int step = cell + gap;
    const uint32_t anchor_day = hm ? hm->anchor_day : 0;
    const int anchor_row = anchor_day != 0 ? (int)((anchor_day + 3U) % 7U) : 6;

    for (int row = 0; row < 7; ++row) {
        if (!k_wd[row]) continue;
        int cy = grid_y + row * step + cell / 2;
        ui_text_vc(fb, label_x, cy, label_px, k_wd[row], EPD_DRAW_ALIGN_LEFT, false);
    }

    if (show_months && anchor_day != 0) {
        int last_month = 0;
        int last_x = -1000;
        for (int col = 0; col < weeks; ++col) {
            uint32_t days_back = (uint32_t)((weeks - 1 - col) * 7 + anchor_row);
            if (anchor_day <= days_back) continue;
            int y = 0, m = 0, d = 0;
            reading_stats_ymd_from_day(anchor_day - days_back, &y, &m, &d);
            if (m != last_month) {
                int mx = grid_x + col * step;
                if (mx - last_x >= 2 * step) {
                    char mbuf[16];
                    snprintf(mbuf, sizeof(mbuf), "%d月", m);
                    ui_text(fb, mx, month_y, label_px, mbuf, EPD_DRAW_ALIGN_LEFT, false);
                    last_x = mx;
                }
                last_month = m;
            }
        }
    }

    uint16_t visible_active = 0;
    for (int col = 0; col < weeks; ++col) {
        for (int row = 0; row < 7; ++row) {
            int days_back = (weeks - 1 - col) * 7 + (anchor_row - row);
            if (days_back < 0) continue;
            uint32_t sec = 0;
            if (hm && (uint32_t)days_back < READING_HEATMAP_DAYS) {
                sec = hm->daily_sec[READING_HEATMAP_DAYS - 1U - (uint32_t)days_back];
            }
            if (sec > 0) visible_active++;
            uint8_t lvl = reading_stats_heatmap_level(sec);
            bool is_today = hm && hm->has_date && days_back == 0;
            EpdRect r = { grid_x + col * step, grid_y + row * step, cell, cell };
            draw_cell(fb, r, lvl, is_today);
        }
    }
    return visible_active;
}

void ui_heatmap_draw_card(uint8_t* framebuffer, EpdRect card, const reading_heatmap_t* hm) {
    if (!framebuffer) return;
    ui_draw_round_rect(framebuffer, card, UI_BTN_RADIUS, UI_GRAY_BLACK);
    ui_text(framebuffer, card.x + UI_PAD, card.y + 12, UI_PX_CAPTION,
            "阅读热力图", EPD_DRAW_ALIGN_LEFT, false);

    char right_text[64];
    if (hm && hm->has_date) {
        snprintf(right_text, sizeof(right_text), "连续 %u 天 · 今日 %u 分 ›",
                 (unsigned)hm->current_streak, rounded_minutes(hm->today_sec, true));
    } else if (hm && hm->active_days > 0) {
        snprintf(right_text, sizeof(right_text), "已读 %u 天 ›", (unsigned)hm->active_days);
    } else {
        snprintf(right_text, sizeof(right_text), "时间未校准 ›");
    }
    ui_text(framebuffer, card.x + card.width - UI_PAD, card.y + 12, UI_PX_CAPTION,
            right_text, EPD_DRAW_ALIGN_RIGHT, false);

    // 26 周 × 7 行，16px 方格 + 4px 间距，宽 516px、高 136px。
    // 26 weeks x 7 rows, 16px cells + 4px gap: 516px wide, 136px tall.
    const int cell = 16;
    const int gap = 4;
    const int grid_w = HEATMAP_CARD_WEEKS * (cell + gap) - gap;
    const int grid_x = card.x + card.width - UI_PAD - grid_w;
    const int grid_y = card.y + 48;
    uint16_t visible_active = draw_grid(
        framebuffer, hm, HEATMAP_CARD_WEEKS, grid_x, grid_y, cell, gap,
        card.x + UI_PAD, 20, false, 0
    );

    const int footer_cy = card.y + card.height - 18;
    char left_footer[64];
    if (visible_active > 0) {
        snprintf(left_footer, sizeof(left_footer), "近 26 周 · 已读 %u 天", (unsigned)visible_active);
    } else if (hm && hm->has_date) {
        snprintf(left_footer, sizeof(left_footer), "近 26 周 · 暂无记录");
    } else {
        snprintf(left_footer, sizeof(left_footer), "校准时间后自动记录");
    }
    ui_text_vc(framebuffer, card.x + UI_PAD, footer_cy, 22, left_footer, EPD_DRAW_ALIGN_LEFT, false);
    draw_legend_right(framebuffer, card.x + card.width - UI_PAD, footer_cy, 14, 4, 22);
}

static void draw_stat_box(uint8_t* fb, EpdRect box, const char* label, const char* value) {
    ui_draw_round_rect(fb, box, UI_BTN_RADIUS, UI_GRAY_BLACK);
    ui_text(fb, box.x + UI_PAD, box.y + 14, UI_PX_CAPTION, label, EPD_DRAW_ALIGN_LEFT, false);
    ui_text_vc(fb, box.x + UI_PAD, box.y + 78, UI_PX_VALUE, value, EPD_DRAW_ALIGN_LEFT, false);
}

void ui_heatmap_draw_detail_body(
    uint8_t* framebuffer, int top_y, const reading_heatmap_t* hm, uint32_t total_read_sec
) {
    if (!framebuffer) return;
    const int box_h = 116;
    char val[48];

    snprintf(val, sizeof(val), "%u 天", hm ? (unsigned)hm->current_streak : 0U);
    draw_stat_box(framebuffer, ui_grid_rect(0, 2, 0, top_y, box_h), "连续阅读", val);

    snprintf(val, sizeof(val), "%u 天", hm ? (unsigned)hm->longest_streak : 0U);
    draw_stat_box(framebuffer, ui_grid_rect(1, 2, 0, top_y, box_h), "最长连续", val);

    format_duration(val, sizeof(val), hm ? hm->today_sec : 0U, true);
    draw_stat_box(framebuffer, ui_grid_rect(0, 2, 1, top_y, box_h), "今日阅读", val);

    format_duration(val, sizeof(val), total_read_sec, false);
    draw_stat_box(framebuffer, ui_grid_rect(1, 2, 1, top_y, box_h), "累计阅读", val);

    EpdRect card = { UI_MARGIN, top_y + 2 * box_h + UI_GAP + 20, ui_content_width(), 352 };
    ui_draw_round_rect(framebuffer, card, UI_BTN_RADIUS, UI_GRAY_BLACK);
    ui_text(framebuffer, card.x + UI_PAD, card.y + 14, UI_PX_CAPTION,
            "近 18 周阅读分布", EPD_DRAW_ALIGN_LEFT, false);

    // 18 周 × 7 行，25px 方格 + 5px 间距，宽 535px、高 205px。
    // 18 weeks x 7 rows, 25px cells + 5px gap: 535px wide, 205px tall.
    const int cell = 25;
    const int gap = 5;
    const int grid_w = HEATMAP_DETAIL_WEEKS * (cell + gap) - gap;
    const int grid_x = card.x + card.width - UI_PAD - grid_w;
    const int month_y = card.y + 52;
    const int grid_y = card.y + 84;
    uint16_t visible_active = draw_grid(
        framebuffer, hm, HEATMAP_DETAIL_WEEKS, grid_x, grid_y, cell, gap,
        card.x + UI_PAD, 22, true, month_y
    );

    char active_str[32];
    snprintf(active_str, sizeof(active_str), "已读 %u 天", (unsigned)visible_active);
    ui_text(framebuffer, card.x + card.width - UI_PAD, card.y + 14, UI_PX_CAPTION,
            active_str, EPD_DRAW_ALIGN_RIGHT, false);

    const int footer_cy = card.y + card.height - 26;
    char span[64];
    if (hm && hm->anchor_day != 0) {
        int anchor_row = (int)((hm->anchor_day + 3U) % 7U);
        uint32_t span_days = (uint32_t)((HEATMAP_DETAIL_WEEKS - 1) * 7 + anchor_row);
        uint32_t start_day = hm->anchor_day > span_days ? hm->anchor_day - span_days : 0;
        int sy = 0, sm = 0, sd = 0, ey = 0, em = 0, ed = 0;
        reading_stats_ymd_from_day(start_day, &sy, &sm, &sd);
        reading_stats_ymd_from_day(hm->anchor_day, &ey, &em, &ed);
        snprintf(span, sizeof(span), "%04d-%02d-%02d ~ %02d-%02d", sy, sm, sd, em, ed);
    } else {
        snprintf(span, sizeof(span), "校准时间后按本地日期统计");
    }
    ui_text_vc(framebuffer, card.x + UI_PAD, footer_cy, 22, span, EPD_DRAW_ALIGN_LEFT, false);
    draw_legend_right(framebuffer, card.x + card.width - UI_PAD, footer_cy, 18, 5, 22);
}

void ui_heatmap_render_lockscreen(
    uint8_t* framebuffer, const reading_heatmap_t* hm, uint32_t total_read_sec,
    const char* last_book, uint8_t last_percent
) {
    if (!framebuffer) return;
    ui_clear_page(framebuffer);

    const int cx = UI_LOCK_WIDTH / 2;
    ui_text_vc(framebuffer, cx, 78, UI_PX_TITLE, "阅读热力图", EPD_DRAW_ALIGN_CENTER, false);

    char sub[64];
    if (hm && hm->has_date && hm->anchor_day != 0) {
        int y = 0, m = 0, d = 0;
        reading_stats_ymd_from_day(hm->anchor_day, &y, &m, &d);
        snprintf(sub, sizeof(sub), "%04d-%02d-%02d", y, m, d);
    } else {
        snprintf(sub, sizeof(sub), "时间未校准");
    }
    ui_text_vc(framebuffer, cx, 134, UI_PX_CAPTION, sub, EPD_DRAW_ALIGN_CENTER, false);
    ui_hairline(framebuffer, 164, UI_MARGIN, ui_content_width(), UI_GRAY_BLACK);

    ui_heatmap_draw_detail_body(framebuffer, 188, hm, total_read_sec);

    // 底部最近在读卡片：y = 832..1016，与锁屏封面视觉语言一致。
    // Bottom recent-book card (y = 832..1016) matching the lock-screen design language.
    EpdRect book_card = { UI_MARGIN, 832, ui_content_width(), 184 };
    ui_draw_round_rect(framebuffer, book_card, UI_BTN_RADIUS, UI_GRAY_BLACK);
    ui_text(framebuffer, book_card.x + UI_PAD, book_card.y + 14, UI_PX_CAPTION,
            "正在阅读", EPD_DRAW_ALIGN_LEFT, false);

    if (last_book && last_book[0]) {
        char title[128];
        strlcpy(title, last_book, sizeof(title));
        char* dot = strrchr(title, '.');
        if (dot && dot > title && strlen(dot) <= 6) *dot = '\0';
        fit_text(title, UI_PX_BODY, book_card.width - 2 * UI_PAD);
        ui_text(framebuffer, book_card.x + UI_PAD, book_card.y + 56, UI_PX_BODY,
                title, EPD_DRAW_ALIGN_LEFT, false);

        if (last_percent > 100) last_percent = 100;
        const int bar_x = book_card.x + UI_PAD;
        const int bar_y = book_card.y + 122;
        const int bar_w = book_card.width - 2 * UI_PAD - 132;
        epd_fill_rect((EpdRect){ bar_x, bar_y, bar_w, 12 }, UI_GRAY_LIGHT, framebuffer);
        epd_fill_rect((EpdRect){ bar_x, bar_y, bar_w * (int)last_percent / 100, 12 },
                      UI_GRAY_BLACK, framebuffer);
        char prog[32];
        snprintf(prog, sizeof(prog), "已读 %u%%", (unsigned)last_percent);
        ui_text_vc(framebuffer, book_card.x + book_card.width - UI_PAD, bar_y + 6,
                   UI_PX_CAPTION, prog, EPD_DRAW_ALIGN_RIGHT, false);
    } else {
        ui_text(framebuffer, book_card.x + UI_PAD, book_card.y + 78, UI_PX_CAPTION,
                "打开书架开始阅读，每日自动记录阅读热力图", EPD_DRAW_ALIGN_LEFT, false);
    }

    ui_text_vc(framebuffer, cx, UI_LOCK_HEIGHT - 84, UI_PX_CAPTION,
               "小纸 Pico", EPD_DRAW_ALIGN_CENTER, false);
}
