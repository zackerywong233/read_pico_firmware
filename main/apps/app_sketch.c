/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：将触摸笔迹保存在 PSRAM，使用连续 DU 实现跟手滑动书写，支持撤销、清空与 PNG 导出。
 * English: Keep touch strokes in PSRAM with continuous DU for live drag drawing, undo, clear and PNG export.
 *
 * 冻结：页面离开时释放画布；仅写入挂载的 SD 卡。
 * Frozen: Release the canvas on exit and write only to a mounted SD card.
 */
#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "continuous_du.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "read_pico_sd.h"
#include "settings.h"
#include "time_sync.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define SKETCH_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define SKETCH_TOOLS_COUNT 6
#define SKETCH_TOOLS_H 76
#define SKETCH_TOOLS_Y (UI_CONTENT_BOTTOM - SKETCH_TOOLS_H)
#define SKETCH_CANVAS_TOP UI_CONTENT_TOP
#define SKETCH_CANVAS_BOTTOM (SKETCH_TOOLS_Y - UI_GAP)
#define SKETCH_PLAIN_INTERVAL_MS 35

static uint8_t *s_canvas;
static uint8_t *s_undo;
static size_t s_bytes;
static int s_brush = 4;
static bool s_erase;
static bool s_stale_touch;
static bool s_drawing;
static bool s_du_active;
static bool s_plain_dirty;
static int s_last_x;
static int s_last_y;
static int64_t s_last_plain_ms;
static EpdRect s_plain_rect;
static unsigned s_save_seq;
static char s_status[64];

static EpdRect canvas_rect(void) {
    return (EpdRect){
        .x = 0,
        .y = SKETCH_CANVAS_TOP,
        .width = epd_rotated_display_width(),
        .height = SKETCH_CANVAS_BOTTOM - SKETCH_CANVAS_TOP,
    };
}

static EpdRect tool_rect(int idx) {
    return ui_row_rect(idx, SKETCH_TOOLS_COUNT, SKETCH_TOOLS_Y, SKETCH_TOOLS_H);
}

static const char *brush_label(void) {
    if (s_brush <= 2) return "细";
    if (s_brush <= 5) return "中";
    return "粗";
}

static uint32_t crc_step(uint32_t c, const uint8_t *p, size_t n) {
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) {
            c = (c >> 1) ^ (0xedb88320u & -(c & 1u));
        }
    }
    return c;
}

static void be32(uint8_t *p, uint32_t x) {
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)x;
}

static bool png_chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t n) {
    uint8_t h[8], c[4];
    be32(h, n);
    memcpy(h + 4, type, 4);
    if (fwrite(h, 1, 8, f) != 8 || (n && fwrite(data, 1, n, f) != n)) return false;
    uint32_t crc = crc_step(crc_step(~0u, (const uint8_t *)type, 4), data, n) ^ ~0u;
    be32(c, crc);
    return fwrite(c, 1, 4, f) == 4;
}

static uint8_t canvas_read_logical_nibble(int lx, int ly) {
    int px = 0, py = 0;
    continuous_du_from_logical(lx, ly, &px, &py);
    const int pw = epd_width();
    const int ph = epd_height();
    if (px < 0 || px >= pw || py < 0 || py >= ph) return 15;
    uint8_t byte = s_canvas[(size_t)py * (size_t)(pw / 2) + (size_t)(px / 2)];
    return (px & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
}

// 导出竖屏正向灰度 PNG，使相册与锁屏方向一致。/ Export upright portrait grayscale PNG matching screen and lock orientation.
static bool save_png(const char *path) {
    const int w = epd_rotated_display_width();
    const int h = epd_rotated_display_height();
    if (!s_canvas || w < 1 || h < 1) return false;

    const size_t raw_n = (size_t)(w + 1) * (size_t)h;
    uint8_t *raw = heap_caps_malloc(raw_n, SKETCH_PSRAM);
    if (!raw) return false;

    for (int y = 0; y < h; y++) {
        uint8_t *row = raw + (size_t)y * (size_t)(w + 1);
        row[0] = 0;
        const bool in_canvas = y >= SKETCH_CANVAS_TOP && y < SKETCH_CANVAS_BOTTOM;
        for (int x = 0; x < w; x++) {
            uint8_t v = in_canvas ? canvas_read_logical_nibble(x, y) : 15;
            row[1 + x] = (uint8_t)(v * 17);
        }
    }

    const size_t zcap = raw_n + (raw_n / 65535 + 1) * 5 + 16;
    uint8_t *z = heap_caps_malloc(zcap, SKETCH_PSRAM);
    if (!z) {
        free(raw);
        return false;
    }

    size_t q = 0;
    z[q++] = 0x78;
    z[q++] = 0x01;
    size_t off = 0;
    uint32_t a = 1, b = 0;
    while (off < raw_n) {
        uint16_t take = raw_n - off > 65535 ? 65535 : (uint16_t)(raw_n - off);
        bool last = off + take == raw_n;
        z[q++] = last ? 1 : 0;
        z[q++] = (uint8_t)take;
        z[q++] = (uint8_t)(take >> 8);
        uint16_t inv = (uint16_t)~take;
        z[q++] = (uint8_t)inv;
        z[q++] = (uint8_t)(inv >> 8);
        memcpy(z + q, raw + off, take);
        for (size_t i = off; i < off + take; i++) {
            a = (a + raw[i]) % 65521;
            b = (b + a) % 65521;
        }
        q += take;
        off += take;
    }
    be32(z + q, (b << 16) | a);
    q += 4;

    FILE *f = fopen(path, "wb");
    bool ok = false;
    if (f) {
        static const uint8_t sig[] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        uint8_t ihdr[13];
        be32(ihdr, (uint32_t)w);
        be32(ihdr + 4, (uint32_t)h);
        ihdr[8] = 8;
        ihdr[9] = 0;
        ihdr[10] = ihdr[11] = ihdr[12] = 0;
        ok = fwrite(sig, 1, 8, f) == 8
            && png_chunk(f, "IHDR", ihdr, 13)
            && png_chunk(f, "IDAT", z, (uint32_t)q)
            && png_chunk(f, "IEND", NULL, 0);
        if (fclose(f) != 0) ok = false;
    }

    free(raw);
    free(z);
    return ok;
}

static bool ensure_canvas(void) {
    if (s_canvas && s_undo) return true;
    s_bytes = (size_t)epd_width() * (size_t)epd_height() / 2;
    if (!s_canvas) s_canvas = heap_caps_malloc(s_bytes, SKETCH_PSRAM);
    if (!s_undo) s_undo = heap_caps_malloc(s_bytes, SKETCH_PSRAM);
    if (!s_canvas || !s_undo) {
        free(s_canvas);
        free(s_undo);
        s_canvas = s_undo = NULL;
        s_bytes = 0;
        return false;
    }
    memset(s_canvas, 0xFF, s_bytes);
    memset(s_undo, 0xFF, s_bytes);
    return true;
}

static void draw_chrome(uint8_t *fb) {
    // 只清除页眉与底部工具区，保留画布内容。/ Clear header and bottom chrome while preserving canvas ink.
    ui_clear_rect_fast(fb, (EpdRect){ 0, 0, epd_rotated_display_width(), SKETCH_CANVAS_TOP });
    ui_clear_rect_fast(fb, (EpdRect){
        0, SKETCH_CANVAS_BOTTOM,
        epd_rotated_display_width(),
        epd_rotated_display_height() - SKETCH_CANVAS_BOTTOM,
    });

    char sub[128];
    snprintf(sub, sizeof(sub), "%s · 笔刷 %s%s",
             s_status[0] ? s_status : "滑动书写，支持撤销与导出",
             brush_label(),
             s_erase ? " · 橡皮中" : "");
    ui_draw_header(fb, "随手便签", sub);
    ui_hairline(fb, SKETCH_CANVAS_BOTTOM, UI_MARGIN, ui_content_width(), UI_GRAY_LIGHT);

    const char *labels[SKETCH_TOOLS_COUNT] = {
        "保存", "撤销", "清空", brush_label(), "橡皮", "锁屏",
    };
    for (int i = 0; i < SKETCH_TOOLS_COUNT; i++) {
        EpdRect r = tool_rect(i);
        bool active = (i == 4 && s_erase);
        ui_draw_choice_round_rect(fb, r, UI_BTN_RADIUS, active);
        ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, UI_PX_CAPTION,
                   labels[i], EPD_DRAW_ALIGN_CENTER, false);
    }

    ui_text_vc(fb, UI_MARGIN, UI_BAR_TOP + UI_BAR_H / 2, UI_PX_CAPTION,
               "单指在画布区滑动书写 · KEY2 清残影", EPD_DRAW_ALIGN_LEFT, false);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    if (ensure_canvas()) {
        memcpy(fb, s_canvas, s_bytes);
    } else {
        ui_clear_page(fb);
    }
    draw_chrome(fb);
}

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void stamp_disc(app_ctx_t *ctx, int lx, int ly, int r, bool erase, bool mark_du) {
    const EpdRect c = canvas_rect();
    const int cx = clamp_int(lx, c.x + r, c.x + c.width - 1 - r);
    const int cy = clamp_int(ly, c.y + r, c.y + c.height - 1 - r);
    const uint8_t color = erase ? UI_GRAY_WHITE : UI_GRAY_BLACK;
    const int r2 = r * r;

    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r2) continue;
            epd_draw_pixel(cx + dx, cy + dy, color, s_canvas);
            if (ctx->fb) epd_draw_pixel(cx + dx, cy + dy, color, ctx->fb);
        }
    }

    if (mark_du) {
        int px = 0, py = 0;
        continuous_du_from_logical(cx, cy, &px, &py);
        int phases = erase ? -continuous_du_light_phases() : continuous_du_dark_phases();
        continuous_du_mark_circle(px, py, r, phases);
    } else {
        EpdRect dot = {
            .x = cx - r - 2,
            .y = cy - r - 2,
            .width = 2 * r + 5,
            .height = 2 * r + 5,
        };
        s_plain_rect = ui_rect_union(s_plain_rect, dot);
        s_plain_dirty = true;
    }
}

static void draw_segment(app_ctx_t *ctx, int x0, int y0, int x1, int y1, int r, bool erase, bool mark_du) {
    const int dx = x1 - x0;
    const int dy = y1 - y0;
    const int ax = dx < 0 ? -dx : dx;
    const int ay = dy < 0 ? -dy : dy;
    const int dist = ax > ay ? ax : ay;
    const int step = r > 2 ? r / 2 : 1;

    if (dist == 0) {
        stamp_disc(ctx, x1, y1, r, erase, mark_du);
        return;
    }
    for (int d = step; d < dist; d += step) {
        int x = x0 + dx * d / dist;
        int y = y0 + dy * d / dist;
        stamp_disc(ctx, x, y, r, erase, mark_du);
    }
    stamp_disc(ctx, x1, y1, r, erase, mark_du);
}

static void stop_active_stroke(void) {
    s_drawing = false;
    s_plain_dirty = false;
    s_plain_rect = (EpdRect){ 0 };
    if (s_du_active) {
        continuous_du_reset();
        s_du_active = false;
    }
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    (void)ensure_canvas();
    s_stale_touch = true;
    s_drawing = false;
    s_du_active = false;
    s_plain_dirty = false;
    s_plain_rect = (EpdRect){ 0 };
    s_status[0] = '\0';
}

static void sketch_on_exit(app_ctx_t *ctx) {
    (void)ctx;
    stop_active_stroke();
    free(s_canvas);
    free(s_undo);
    s_canvas = s_undo = NULL;
    s_bytes = 0;
}

static app_redraw_t on_touch(app_ctx_t *ctx, const cst836u_touch_t *t) {
    (void)ctx;
    if (!ensure_canvas()) return APP_REDRAW_FULL;

    for (int i = 0; i < SKETCH_TOOLS_COUNT; i++) {
        if (!ui_rect_hit(tool_rect(i), t->x, t->y)) continue;
        stop_active_stroke();
        s_stale_touch = false;
        read_pico_sd_info_t sd = { 0 };
        read_pico_sd_get_info(&sd);
        if (i == 0) {
            if (!sd.mounted) {
                snprintf(s_status, sizeof(s_status), "未检测到已挂载的 TF 卡");
            } else {
                mkdir("/sdcard/notes", 0777);
                time_t utc = 0;
                char path[96];
                if (app_time_now_utc(&utc)) {
                    snprintf(path, sizeof(path), "/sdcard/notes/sketch_%lld_%u.png",
                             (long long)utc, ++s_save_seq);
                } else {
                    snprintf(path, sizeof(path), "/sdcard/notes/sketch_%u.png", ++s_save_seq);
                }
                snprintf(s_status, sizeof(s_status), "%s",
                         save_png(path) ? "已导出 PNG 到 TF 卡" : "保存 PNG 失败");
            }
        } else if (i == 1) {
            memcpy(s_canvas, s_undo, s_bytes);
            snprintf(s_status, sizeof(s_status), "已撤销上一笔");
        } else if (i == 2) {
            memcpy(s_undo, s_canvas, s_bytes);
            memset(s_canvas, 0xFF, s_bytes);
            snprintf(s_status, sizeof(s_status), "已清空画布");
        } else if (i == 3) {
            s_brush = s_brush <= 2 ? 4 : (s_brush <= 5 ? 8 : 2);
            s_erase = false;
            snprintf(s_status, sizeof(s_status), "笔刷切换为 %s", brush_label());
        } else if (i == 4) {
            s_erase = !s_erase;
            snprintf(s_status, sizeof(s_status), "%s", s_erase ? "橡皮模式已开启" : "已切回画笔");
        } else if (i == 5) {
            if (!sd.mounted) {
                snprintf(s_status, sizeof(s_status), "未检测到已挂载的 TF 卡");
            } else if (save_png("/sdcard/screensaver.png")) {
                app_settings_set_saver_style(APP_SAVER_IMAGE);
                snprintf(s_status, sizeof(s_status), "已设为 TF 卡图片锁屏");
            } else {
                snprintf(s_status, sizeof(s_status), "设为锁屏失败");
            }
        }
        return APP_REDRAW_FULL;
    }

    // 非工具按钮落点放行给 on_tick 连续跟手书写。/ Pass non-toolbar touches to on_tick for live drag drawing.
    return APP_REDRAW_NONE;
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    if (!s_canvas || !s_undo) return APP_REDRAW_NONE;

    if (s_stale_touch) {
        if (!ctx->pressed || ctx->consumed) return APP_REDRAW_NONE;
        s_stale_touch = false;
    }
    if (ctx->consumed) return APP_REDRAW_NONE;

    const cst836u_touch_t *touch = ctx->touch;
    const EpdRect c = canvas_rect();
    const bool in_canvas = touch && touch->touched && touch->count == 1
        && ui_rect_hit(c, touch->x, touch->y);

    if (in_canvas) {
        const int r = s_erase ? (s_brush + 6) : s_brush;
        if (!s_drawing) {
            memcpy(s_undo, s_canvas, s_bytes);
            s_drawing = true;
            s_last_x = touch->x;
            s_last_y = touch->y;
            stamp_disc(ctx, touch->x, touch->y, r, s_erase, ctx->continuous_ready);
        } else if (touch->x != s_last_x || touch->y != s_last_y) {
            draw_segment(ctx, s_last_x, s_last_y, touch->x, touch->y, r, s_erase, ctx->continuous_ready);
            s_last_x = touch->x;
            s_last_y = touch->y;
        }

        if (ctx->continuous_ready) {
            s_du_active = true;
            if (continuous_du_busy()) {
                rails_keepalive();
                epd_poweron();
                enum EpdDrawError err = continuous_du_scan(
                    ctx->hl, continuous_du_rect_from_logical(c)
                );
                guard_draw_result(ctx->hl, err);
            }
            return APP_REDRAW_DONE;
        }

        if (s_plain_dirty && (ctx->pressed || ctx->now_ms - s_last_plain_ms >= SKETCH_PLAIN_INTERVAL_MS)) {
            s_last_plain_ms = ctx->now_ms;
            s_plain_dirty = false;
            enum EpdDrawError err = update_display_area_with(
                ctx->hl, &E0470_FOLLOW_WAVEFORM, APP_DYNAMIC_REFRESH_MODE, s_plain_rect
            );
            guard_draw_result(ctx->hl, err);
            s_plain_rect = (EpdRect){ 0 };
        }
        return APP_REDRAW_DONE;
    }

    s_drawing = false;
    if (ctx->continuous_ready && s_du_active) {
        if (continuous_du_busy()) {
            rails_keepalive();
            epd_poweron();
            enum EpdDrawError err = continuous_du_scan(
                ctx->hl, continuous_du_rect_from_logical(c)
            );
            guard_draw_result(ctx->hl, err);
            return APP_REDRAW_DONE;
        }
        s_du_active = false;
        // 连续 DU 扫完后回写参考帧，后续局部或整页刷新可正确差分。/ Sync back_fb once continuous DU drains so later diffs match the screen.
        if (ctx->hl && ctx->hl->back_fb && ctx->fb) {
            memcpy(ctx->hl->back_fb, ctx->fb, s_bytes);
        }
        return APP_REDRAW_DONE;
    }

    if (!ctx->continuous_ready && s_plain_dirty) {
        s_plain_dirty = false;
        enum EpdDrawError err = update_display_area_with(
            ctx->hl, &E0470_FOLLOW_WAVEFORM, APP_DYNAMIC_REFRESH_MODE, s_plain_rect
        );
        guard_draw_result(ctx->hl, err);
        s_plain_rect = (EpdRect){ 0 };
        return APP_REDRAW_DONE;
    }

    return APP_REDRAW_NONE;
}

static EpdRect area(app_ctx_t *ctx) {
    (void)ctx;
    return canvas_rect();
}

const app_desc_t app_sketch = {
    .title = "随手便签",
    .detail = "手写便签与 PNG 保存",
    .render = render,
    .on_enter = on_enter,
    .on_exit = sketch_on_exit,
    .on_touch = on_touch,
    .on_tick = on_tick,
    .area_hint = area,
};
