/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：阅读热力图共用绘制模块，提供看板卡片、详情正文与全屏锁屏渲染。
 *
 * English: Shared reading-heatmap renderer for the dashboard card, detail view, and full-screen lock face.
 *
 * 冻结：只做纯绘制，不读写 NVS、TF 卡或外设。
 * Frozen: Pure rendering only; never touch NVS, SD card, or hardware peripherals.
 */
#pragma once

#include <stdint.h>

#include "reading_stats.h"
#include "ui_kit.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 看板卡片内紧凑 26 周热力图（推荐高度 222 px）。
/// Compact 26-week heatmap inside a dashboard card (recommended height 222 px).
void ui_heatmap_draw_card(uint8_t* framebuffer, EpdRect card, const reading_heatmap_t* hm);

/// 详情页与锁屏共用的统计四格 + 18 周大格热力图正文（总高度约 616 px）。
/// Shared 2x2 metrics + 18-week large heatmap body for detail and lock pages (~616 px tall).
void ui_heatmap_draw_detail_body(
    uint8_t* framebuffer, int top_y, const reading_heatmap_t* hm, uint32_t total_read_sec
);

/// 全屏阅读热力图锁屏（684x1216 竖屏），包含统计卡片、18 周热力图与最近在读进度。
/// Full-screen reading heatmap lock face (684x1216 portrait) with stats, 18-week grid, and recent book.
void ui_heatmap_render_lockscreen(
    uint8_t* framebuffer, const reading_heatmap_t* hm, uint32_t total_read_sec,
    const char* last_book, uint8_t last_percent
);

#ifdef __cplusplus
}
#endif
