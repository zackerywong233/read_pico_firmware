/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TF 卡文件管理独立应用。支持完整目录树浏览、排序与隐藏开关、新建文件夹、
 * 同卷快速移动、迭代安全删除（含生效字体保护与书架联动）、图片全屏预览设壁纸与纯文本预览。
 * English: Standalone TF card file manager with directory browsing, sort/hidden toggles,
 * new folder, fast same-volume move, iterative safe delete (with active-font & bookshelf guards),
 * fullscreen image preview + wallpaper assignment, and plain-text preview.
 *
 * 冻结：严禁路径穿越逃逸 /sdcard；删除/移动当前生效字体前必须先回退内置字体；
 * 单目录 PSRAM 上限 512 项；拔卡立即释放缓冲并回退，不画屏不重挂载。
 * Frozen: Never escape /sdcard; fall back to built-in font before touching active TF font;
 * cap directory listing at 512 items; on_media_lost frees buffers without drawing or remounting.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "app.h"
#include "app_registry.h"
#include "book_image.h"
#include "book_store.h"
#include "esp_heap_caps.h"
#include "files_ops.h"
#include "read_pico_sd.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

extern const app_desc_t app_book;
extern void app_book_queue_open(const char *path);

#define FILES_ROOT "/sdcard"
#define FILES_ROWS 8
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef enum {
    FM_BROWSE = 0,
    FM_DETAIL,
    FM_CONFIRM_DEL,
    FM_PREVIEW_IMG,
    FM_PREVIEW_TXT,
} fm_view_t;

static fm_view_t s_view;
static char s_cur_dir[FILES_PATH_MAX] = FILES_ROOT;
static files_entry_t *s_entries;
static size_t s_count;
static bool s_truncated;
static bool s_show_hidden;
static bool s_sort_size;
static bool s_multi_mode;
static int s_selected_idx = -1;
static size_t s_confirm_tree_count;
static bool s_confirm_touches_font;
static bool s_confirm_batch;
static char s_status[128];

static uint8_t *s_img_pixels;
static uint16_t s_img_w, s_img_h;
static char s_preview_path[FILES_PATH_MAX];
static char *s_txt_buf;
static size_t s_txt_len;
static int s_txt_page;

static void free_previews(void) {
    free(s_img_pixels); s_img_pixels = NULL;
    s_img_w = s_img_h = 0;
    free(s_txt_buf); s_txt_buf = NULL;
    s_txt_len = 0;
    s_txt_page = 0;
    s_preview_path[0] = '\0';
}

static void free_entries(void) {
    free(s_entries); s_entries = NULL;
    s_count = 0;
    s_truncated = false;
}

static void cb_font_fallback(void) {
    (void)ttf_font_open_builtin();
    app_settings_set_font_path("");
}

static void cb_book_changed(const char *book_path) {
    (void)book_store_forget_changed_path(book_path);
    book_store_notify_changed();
}

static int page_count(void) {
    if (!s_count) return 1;
    return (int)((s_count + FILES_ROWS - 1) / FILES_ROWS);
}

static void format_size(uint32_t sz, char *out, size_t cap) {
    if (sz >= 1024u * 1024u) snprintf(out, cap, "%.1f MB", sz / 1048576.0);
    else if (sz >= 1024u) snprintf(out, cap, "%.1f KB", sz / 1024.0);
    else snprintf(out, cap, "%u B", (unsigned)sz);
}

static void reload_dir(app_ctx_t *ctx) {
    free_entries();
    read_pico_sd_info_t info = {0};
    if (read_pico_sd_get_info(&info) != ESP_OK || !info.mounted) {
        snprintf(s_status, sizeof(s_status), "TF 卡未挂载");
        if (ctx) ctx->leaf = 0;
        return;
    }
    s_entries = heap_caps_calloc(FILES_MAX_ENTRIES, sizeof(*s_entries), PSRAM_CAPS);
    if (!s_entries) {
        snprintf(s_status, sizeof(s_status), "内存不足，无法读取目录");
        return;
    }
    if (!files_list_dir(s_cur_dir, s_show_hidden, s_sort_size,
                        s_entries, FILES_MAX_ENTRIES, &s_count, &s_truncated)) {
        snprintf(s_status, sizeof(s_status), "目录读取失败");
        s_count = 0;
    } else if (s_truncated) {
        snprintf(s_status, sizeof(s_status), "目录项过多，仅显示前 %d 项", FILES_MAX_ENTRIES);
    } else {
        s_status[0] = '\0';
    }
    if (ctx && ctx->leaf >= page_count()) ctx->leaf = page_count() - 1;
    if (ctx && ctx->leaf < 0) ctx->leaf = 0;
}

static EpdRect list_row_rect(int row) {
    return (EpdRect){ UI_MARGIN, 304 + row * (UI_BTN_H + UI_GAP), ui_content_width(), UI_BTN_H };
}

static size_t selected_batch_count(void) {
    size_t n = 0;
    for (size_t i = 0; i < s_count; ++i) if (s_entries[i].selected) ++n;
    return n;
}

static void open_image_preview(const char *full_path) {
    free_previews();
    snprintf(s_preview_path, sizeof(s_preview_path), "%s", full_path);
    struct stat st;
    if (stat(full_path, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > FILES_WALLPAPER_MAX) {
        snprintf(s_status, sizeof(s_status), "图片超过 2 MB 或无法读取");
        s_view = FM_PREVIEW_IMG;
        return;
    }
    FILE *f = fopen(full_path, "rb");
    if (!f) { snprintf(s_status, sizeof(s_status), "无法打开图片"); s_view = FM_PREVIEW_IMG; return; }
    size_t n = (size_t)st.st_size;
    uint8_t *raw = heap_caps_malloc(n, PSRAM_CAPS);
    if (!raw) { fclose(f); snprintf(s_status, sizeof(s_status), "内存不足"); s_view = FM_PREVIEW_IMG; return; }
    bool ok = fread(raw, 1, n, f) == n;
    fclose(f);
    if (ok) {
        ok = book_image_decode_fit(raw, n, 604, 820, 2u * 1024u * 1024u, true,
                                   &s_img_pixels, &s_img_w, &s_img_h);
    }
    free(raw);
    if (!ok) snprintf(s_status, sizeof(s_status), "图片解码失败或格式不支持");
    else s_status[0] = '\0';
    s_view = FM_PREVIEW_IMG;
}

static void open_text_preview(const char *full_path) {
    free_previews();
    snprintf(s_preview_path, sizeof(s_preview_path), "%s", full_path);
    struct stat st;
    if (stat(full_path, &st) != 0 || st.st_size < 0 || (size_t)st.st_size > FILES_TEXT_PREVIEW_MAX) {
        snprintf(s_status, sizeof(s_status), "文本超过 256 KB，暂不支持直接预览");
        s_view = FM_PREVIEW_TXT;
        return;
    }
    FILE *f = fopen(full_path, "rb");
    if (!f) { snprintf(s_status, sizeof(s_status), "无法读取文本"); s_view = FM_PREVIEW_TXT; return; }
    size_t n = (size_t)st.st_size;
    s_txt_buf = heap_caps_malloc(n + 1, PSRAM_CAPS);
    if (!s_txt_buf) { fclose(f); snprintf(s_status, sizeof(s_status), "内存不足"); s_view = FM_PREVIEW_TXT; return; }
    s_txt_len = fread(s_txt_buf, 1, n, f);
    fclose(f);
    s_txt_buf[s_txt_len] = '\0';
    s_txt_page = 0;
    s_status[0] = '\0';
    s_view = FM_PREVIEW_TXT;
}

static void render_browse(app_ctx_t *ctx, uint8_t *fb) {
    read_pico_sd_info_t info = {0};
    bool mounted = (read_pico_sd_get_info(&info) == ESP_OK && info.mounted);
    char sub[128];
    if (mounted) {
        snprintf(sub, sizeof(sub), "%.80s · 余 %.1f/%.1f GB",
                 s_cur_dir,
                 (double)info.free_bytes / (1024.0 * 1024.0 * 1024.0),
                 (double)info.capacity_bytes / (1024.0 * 1024.0 * 1024.0));
    } else {
        snprintf(sub, sizeof(sub), "请插入并挂载 TF 卡");
    }
    ui_draw_header(fb, "文件管理", sub);

    ui_draw_button(fb, ui_row_rect(0, 4, 176, 68), s_sort_size ? "按大小" : "按名称", false);
    ui_draw_button(fb, ui_row_rect(1, 4, 176, 68), s_show_hidden ? "含隐藏" : "隐系统", s_show_hidden);
    ui_draw_button(fb, ui_row_rect(2, 4, 176, 68), "新目录", false);
    ui_draw_button(fb, ui_row_rect(3, 4, 176, 68), s_multi_mode ? "删所选" : "多选", s_multi_mode);

    if (s_status[0]) {
        ui_text(fb, UI_MARGIN, 258, UI_PX_CAPTION, s_status, EPD_DRAW_ALIGN_LEFT, false);
    } else if (s_multi_mode) {
        char hint[64];
        snprintf(hint, sizeof(hint), "已勾选 %u 项 · 点按条目切换勾选", (unsigned)selected_batch_count());
        ui_text(fb, UI_MARGIN, 258, UI_PX_CAPTION, hint, EPD_DRAW_ALIGN_LEFT, false);
    } else {
        char hint[64];
        snprintf(hint, sizeof(hint), "共 %u 项 · 第 %d / %d 页",
                 (unsigned)s_count, ctx->leaf + 1, page_count());
        ui_text(fb, UI_MARGIN, 258, UI_PX_CAPTION, hint, EPD_DRAW_ALIGN_LEFT, false);
    }

    if (!mounted) {
        ui_text_vc(fb, epd_rotated_display_width() / 2, 540, UI_PX_BODY,
                   "未检测到已挂载的 TF 卡", EPD_DRAW_ALIGN_CENTER, false);
        ui_draw_button(fb, ui_bar_rect(0, 1), "重新检测", false);
        return;
    }

    if (s_count == 0) {
        ui_text_vc(fb, epd_rotated_display_width() / 2, 540, UI_PX_BODY,
                   "当前目录为空", EPD_DRAW_ALIGN_CENTER, false);
    } else {
        for (int r = 0; r < FILES_ROWS; ++r) {
            size_t idx = (size_t)ctx->leaf * FILES_ROWS + (size_t)r;
            if (idx >= s_count) break;
            const files_entry_t *e = &s_entries[idx];
            EpdRect rect = list_row_rect(r);
            ui_draw_choice_round_rect(fb, rect, UI_BTN_RADIUS, s_multi_mode && e->selected);
            char left[160], right[40];
            snprintf(left, sizeof(left), "%s%s", e->is_dir ? "[目录] " : "", e->name);
            if (e->is_dir) snprintf(right, sizeof(right), "打开 ›");
            else format_size(e->size, right, sizeof(right));
            if (s_multi_mode) snprintf(right, sizeof(right), "%s", e->selected ? "已选" : "未选");
            int max_w = rect.width - 2 * UI_PAD - ttf_text_width_px(UI_PX_CAPTION, right) - UI_GAP;
            while (left[0] && ttf_text_width_px(UI_PX_BTN, left) > max_w) {
                size_t len = strlen(left) - 1;
                while (len && ((unsigned char)left[len] & 0xc0) == 0x80) --len;
                left[len] = '\0';
            }
            ui_text_vc(fb, rect.x + UI_PAD, rect.y + rect.height / 2, UI_PX_BTN,
                       left, EPD_DRAW_ALIGN_LEFT, false);
            ui_text_vc(fb, rect.x + rect.width - UI_PAD, rect.y + rect.height / 2, UI_PX_CAPTION,
                       right, EPD_DRAW_ALIGN_RIGHT, false);
        }
    }
}

static void render_detail(uint8_t *fb) {
    if (s_selected_idx < 0 || (size_t)s_selected_idx >= s_count) return;
    const files_entry_t *e = &s_entries[s_selected_idx];
    char sz[32] = "文件夹";
    if (!e->is_dir) format_size(e->size, sz, sizeof(sz));
    ui_draw_header(fb, "文件详情", e->name);
    int y = UI_CONTENT_TOP;
    y = ui_draw_row(fb, y, "位置", s_cur_dir);
    y = ui_draw_row(fb, y, "大小", sz);
    if (s_status[0]) ui_text(fb, UI_MARGIN, y + 12, UI_PX_CAPTION, s_status, EPD_DRAW_ALIGN_LEFT, false);

    int by = y + 64;
    const char *primary = e->is_dir ? "打开目录"
        : e->kind == FILES_KIND_BOOK ? "移入书架"
        : e->kind == FILES_KIND_FONT ? "移入字体并启用"
        : e->kind == FILES_KIND_IMAGE ? "预览图片"
        : e->kind == FILES_KIND_TEXT ? "预览文本" : "移至上级目录";
    ui_draw_button(fb, ui_row_rect(0, 1, by, UI_BTN_H), primary, false);
    ui_draw_button(fb, ui_row_rect(0, 1, by + 96, UI_BTN_H),
                   e->kind == FILES_KIND_BOOK ? "前往书架阅读" : "移至上级目录", false);
    ui_draw_button(fb, ui_row_rect(0, 1, by + 192, UI_BTN_H), "删除此项", false);
}

static void render_confirm_del(uint8_t *fb) {
    ui_draw_header(fb, "确认删除", "删除后文件不可恢复");
    char msg[160];
    if (s_confirm_batch) {
        snprintf(msg, sizeof(msg), "确认删除已勾选的 %u 项（共含 %u 个条目）？",
                 (unsigned)selected_batch_count(), (unsigned)s_confirm_tree_count);
    } else if (s_selected_idx >= 0 && (size_t)s_selected_idx < s_count) {
        snprintf(msg, sizeof(msg), "确认删除“%.96s”（共含 %u 个条目）？",
                 s_entries[s_selected_idx].name, (unsigned)s_confirm_tree_count);
    } else {
        snprintf(msg, sizeof(msg), "确认删除所选项目？");
    }
    ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 60, UI_PX_BODY, msg, EPD_DRAW_ALIGN_LEFT, false);
    if (s_confirm_touches_font) {
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 140, UI_PX_CAPTION,
                "注意：包含当前生效字体，删除前将自动回退内置字体", EPD_DRAW_ALIGN_LEFT, false);
    }
    ui_draw_button(fb, ui_row_rect(0, 2, 520, UI_BTN_H), "取消", false);
    ui_draw_button(fb, ui_row_rect(1, 2, 520, UI_BTN_H), "确认删除", true);
}

static void render_preview_img(uint8_t *fb) {
    const char *name = strrchr(s_preview_path, '/');
    ui_draw_header(fb, "图片预览", name ? name + 1 : s_preview_path);
    if (s_img_pixels && s_img_w && s_img_h) {
        int x0 = (epd_rotated_display_width() - (int)s_img_w) / 2;
        int y0 = UI_CONTENT_TOP + ((UI_CONTENT_BOTTOM - UI_CONTENT_TOP) - (int)s_img_h) / 2;
        if (x0 < 0) x0 = 0;
        if (y0 < UI_CONTENT_TOP) y0 = UI_CONTENT_TOP;
        for (uint16_t y = 0; y < s_img_h && y0 + y < UI_CONTENT_BOTTOM; ++y) {
            const uint8_t *row = s_img_pixels + (size_t)y * s_img_w;
            for (uint16_t x = 0; x < s_img_w && x0 + x < epd_rotated_display_width(); ++x) {
                epd_draw_pixel(x0 + x, y0 + y, row[x], fb);
            }
        }
    } else {
        ui_text_vc(fb, epd_rotated_display_width() / 2, 540, UI_PX_BODY,
                   s_status[0] ? s_status : "图片不可用", EPD_DRAW_ALIGN_CENTER, false);
    }
    ui_draw_button(fb, ui_bar_rect(0, 1), "设为锁屏图片", false);
}

static void render_preview_txt(uint8_t *fb) {
    const char *name = strrchr(s_preview_path, '/');
    ui_draw_header(fb, "文本预览", name ? name + 1 : s_preview_path);
    if (!s_txt_buf) {
        ui_text_vc(fb, epd_rotated_display_width() / 2, 540, UI_PX_BODY,
                   s_status[0] ? s_status : "无法预览", EPD_DRAW_ALIGN_CENTER, false);
    } else {
        const size_t chunk = 600;
        size_t off = (size_t)s_txt_page * chunk;
        if (off >= s_txt_len) { s_txt_page = 0; off = 0; }
        while (off < s_txt_len && (((unsigned char)s_txt_buf[off] & 0xc0) == 0x80)) ++off;
        int y = UI_CONTENT_TOP + 8;
        const int max_w = ui_content_width();
        size_t p = off;
        while (p < s_txt_len && y + 40 < UI_NO_BAR_BOTTOM) {
            char line[160] = {0};
            size_t l = 0;
            while (p < s_txt_len && s_txt_buf[p] != '\n' && l + 5 < sizeof(line)) {
                if (s_txt_buf[p] == '\r') { ++p; continue; }
                unsigned char c = (unsigned char)s_txt_buf[p];
                size_t cn = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : 4;
                if (p + cn > s_txt_len) cn = 1;
                memcpy(line + l, s_txt_buf + p, cn);
                line[l + cn] = '\0';
                if (ttf_text_width_px(30, line) > max_w && l > 0) {
                    line[l] = '\0';
                    break;
                }
                l += cn;
                p += cn;
            }
            if (p < s_txt_len && s_txt_buf[p] == '\n') ++p;
            ui_text(fb, UI_MARGIN, y, 30, line, EPD_DRAW_ALIGN_LEFT, false);
            y += 42;
        }
    }
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    ui_clear_page(fb);
    if (s_view == FM_DETAIL) render_detail(fb);
    else if (s_view == FM_CONFIRM_DEL) render_confirm_del(fb);
    else if (s_view == FM_PREVIEW_IMG) render_preview_img(fb);
    else if (s_view == FM_PREVIEW_TXT) render_preview_txt(fb);
    else render_browse(ctx, fb);
}

static void on_enter(app_ctx_t *ctx) {
    s_view = FM_BROWSE;
    s_multi_mode = false;
    s_selected_idx = -1;
    s_status[0] = '\0';
    snprintf(s_cur_dir, sizeof(s_cur_dir), "%s", FILES_ROOT);
    ctx->leaf = 0;
    reload_dir(ctx);
}

static void files_on_exit(app_ctx_t *ctx) {
    (void)ctx;
    free_previews();
    free_entries();
}

static void on_media_lost(app_ctx_t *ctx) {
    free_previews();
    free_entries();
    s_view = FM_BROWSE;
    s_multi_mode = false;
    s_selected_idx = -1;
    snprintf(s_cur_dir, sizeof(s_cur_dir), "%s", FILES_ROOT);
    snprintf(s_status, sizeof(s_status), "TF 卡已移除");
    if (ctx) ctx->leaf = 0;
}

static void prepare_delete_confirm(bool batch) {
    s_confirm_batch = batch;
    s_confirm_tree_count = 0;
    s_confirm_touches_font = false;
    const char *active_font = ttf_font_is_builtin() ? "" : ttf_font_path();
    if (batch) {
        for (size_t i = 0; i < s_count; ++i) {
            if (!s_entries[i].selected) continue;
            char full[FILES_PATH_MAX];
            if (!files_path_join_safe(FILES_ROOT, s_cur_dir, s_entries[i].name, full, sizeof(full))) continue;
            size_t c = 0;
            if (files_count_tree(FILES_ROOT, full, &c)) s_confirm_tree_count += c;
            if (active_font[0] && files_path_covers(full, active_font)) s_confirm_touches_font = true;
        }
    } else if (s_selected_idx >= 0 && (size_t)s_selected_idx < s_count) {
        char full[FILES_PATH_MAX];
        if (files_path_join_safe(FILES_ROOT, s_cur_dir, s_entries[s_selected_idx].name, full, sizeof(full))) {
            (void)files_count_tree(FILES_ROOT, full, &s_confirm_tree_count);
            if (active_font[0] && files_path_covers(full, active_font)) s_confirm_touches_font = true;
        }
    }
    s_view = FM_CONFIRM_DEL;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (ev->type == UI_GESTURE_SWIPE_L && s_view == FM_BROWSE && ctx->leaf + 1 < page_count()) {
        ctx->leaf++;
        return APP_REDRAW_PAGE;
    }
    if (ev->type == UI_GESTURE_SWIPE_R && s_view == FM_BROWSE && ctx->leaf > 0) {
        ctx->leaf--;
        return APP_REDRAW_PAGE;
    }
    if (ev->type == UI_GESTURE_SWIPE_L && s_view == FM_PREVIEW_TXT &&
        (size_t)(s_txt_page + 1) * 600 < s_txt_len) {
        s_txt_page++;
        return APP_REDRAW_PAGE;
    }
    if (ev->type == UI_GESTURE_SWIPE_R && s_view == FM_PREVIEW_TXT && s_txt_page > 0) {
        s_txt_page--;
        return APP_REDRAW_PAGE;
    }
    if (ev->type != UI_GESTURE_TAP && ev->type != UI_GESTURE_LONG_PRESS) return APP_REDRAW_NONE;

    uint16_t x = ev->x, y = ev->y;
    if (s_view == FM_PREVIEW_IMG) {
        if (ui_bar_hit(x, y, 1) == 0) {
            if (files_set_wallpaper_from(FILES_ROOT, s_preview_path)) {
                app_settings_set_saver_style(APP_SAVER_IMAGE);
                snprintf(s_status, sizeof(s_status), "已设为锁屏图片");
            } else {
                snprintf(s_status, sizeof(s_status), "设置锁屏图失败（需 <= 2 MB）");
            }
            free_previews();
            s_view = FM_BROWSE;
            reload_dir(ctx);
            return APP_REDRAW_FULL;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == FM_PREVIEW_TXT) {
        if (x < epd_rotated_display_width() / 3 && s_txt_page > 0) {
            s_txt_page--;
            return APP_REDRAW_PAGE;
        }
        if (x > epd_rotated_display_width() * 2 / 3 && (size_t)(s_txt_page + 1) * 600 < s_txt_len) {
            s_txt_page++;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == FM_CONFIRM_DEL) {
        if (ui_rect_hit(ui_row_rect(0, 2, 520, UI_BTN_H), x, y)) {
            s_view = FM_BROWSE;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(ui_row_rect(1, 2, 520, UI_BTN_H), x, y)) {
            const char *active_font = ttf_font_is_builtin() ? "" : ttf_font_path();
            if (s_confirm_batch) {
                for (size_t i = 0; i < s_count; ++i) {
                    if (!s_entries[i].selected) continue;
                    char full[FILES_PATH_MAX];
                    if (files_path_join_safe(FILES_ROOT, s_cur_dir, s_entries[i].name, full, sizeof(full))) {
                        (void)files_delete_tree_safe(FILES_ROOT, full, active_font, cb_font_fallback, cb_book_changed);
                    }
                }
                s_multi_mode = false;
            } else if (s_selected_idx >= 0 && (size_t)s_selected_idx < s_count) {
                char full[FILES_PATH_MAX];
                if (files_path_join_safe(FILES_ROOT, s_cur_dir, s_entries[s_selected_idx].name, full, sizeof(full))) {
                    (void)files_delete_tree_safe(FILES_ROOT, full, active_font, cb_font_fallback, cb_book_changed);
                }
            }
            s_selected_idx = -1;
            s_view = FM_BROWSE;
            reload_dir(ctx);
            snprintf(s_status, sizeof(s_status), "删除完成");
            return APP_REDRAW_FULL;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == FM_DETAIL) {
        if (s_selected_idx < 0 || (size_t)s_selected_idx >= s_count) { s_view = FM_BROWSE; return APP_REDRAW_PAGE; }
        const files_entry_t *e = &s_entries[s_selected_idx];
        char full[FILES_PATH_MAX];
        if (!files_path_join_safe(FILES_ROOT, s_cur_dir, e->name, full, sizeof(full))) return APP_REDRAW_NONE;
        const char *active_font = ttf_font_is_builtin() ? "" : ttf_font_path();
        int by = UI_CONTENT_TOP + 2 * UI_ROW_H + 64;
        if (ui_rect_hit(ui_row_rect(0, 1, by, UI_BTN_H), x, y)) {
            if (e->is_dir) {
                snprintf(s_cur_dir, sizeof(s_cur_dir), "%s", full);
                ctx->leaf = 0;
                s_view = FM_BROWSE;
                reload_dir(ctx);
                return APP_REDRAW_PAGE;
            }
            if (e->kind == FILES_KIND_BOOK) {
                char dst[FILES_PATH_MAX];
                if (files_move_safe(FILES_ROOT, full, FILES_ROOT "/books", e->name, active_font,
                                    cb_font_fallback, cb_book_changed, dst, sizeof(dst))) {
                    s_view = FM_BROWSE;
                    reload_dir(ctx);
                    snprintf(s_status, sizeof(s_status), "已移入书架");
                } else snprintf(s_status, sizeof(s_status), "移入失败（可能已存在同名文件）");
                return APP_REDRAW_PAGE;
            }
            if (e->kind == FILES_KIND_FONT) {
                char dst[FILES_PATH_MAX];
                if (files_move_safe(FILES_ROOT, full, FILES_ROOT "/fonts", e->name, active_font,
                                    cb_font_fallback, cb_book_changed, dst, sizeof(dst))) {
                    if (ttf_font_open(dst) == ESP_OK && !ttf_font_is_builtin()) {
                        app_settings_set_font_path(dst);
                    }
                    s_view = FM_BROWSE;
                    reload_dir(ctx);
                    snprintf(s_status, sizeof(s_status), "已移入字体目录并启用");
                } else snprintf(s_status, sizeof(s_status), "移动字体失败");
                return APP_REDRAW_FULL;
            }
            if (e->kind == FILES_KIND_IMAGE) { open_image_preview(full); return APP_REDRAW_FULL; }
            if (e->kind == FILES_KIND_TEXT) { open_text_preview(full); return APP_REDRAW_PAGE; }
        }
        if (ui_rect_hit(ui_row_rect(0, 1, by + 96, UI_BTN_H), x, y)) {
            if (e->kind == FILES_KIND_BOOK) {
                if (files_is_under_books(FILES_ROOT, full)) {
                    app_book_queue_open(full);
                }
                ctx->request_app = &app_book;
                return APP_REDRAW_NONE;
            }
            char parent[FILES_PATH_MAX];
            if (files_path_parent(FILES_ROOT, s_cur_dir, parent, sizeof(parent)) &&
                files_move_safe(FILES_ROOT, full, parent, e->name, active_font,
                                cb_font_fallback, cb_book_changed, NULL, 0)) {
                s_view = FM_BROWSE;
                reload_dir(ctx);
                snprintf(s_status, sizeof(s_status), "已移至上级目录");
            } else snprintf(s_status, sizeof(s_status), "无法移至上级目录");
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(ui_row_rect(0, 1, by + 192, UI_BTN_H), x, y)) {
            prepare_delete_confirm(false);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }

    // FM_BROWSE 视图点击处理。/ FM_BROWSE tap handling.
    read_pico_sd_info_t info = {0};
    if (read_pico_sd_get_info(&info) != ESP_OK || !info.mounted) {
        if (ui_rect_hit(ui_bar_rect(0, 1), x, y)) {
            (void)read_pico_sd_start_probe();
            reload_dir(ctx);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }

    if (ui_rect_hit(ui_row_rect(0, 4, 176, 68), x, y)) {
        s_sort_size = !s_sort_size;
        reload_dir(ctx);
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(ui_row_rect(1, 4, 176, 68), x, y)) {
        s_show_hidden = !s_show_hidden;
        reload_dir(ctx);
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(ui_row_rect(2, 4, 176, 68), x, y)) {
        if (files_mkdir_auto(FILES_ROOT, s_cur_dir, NULL, 0)) {
            reload_dir(ctx);
            snprintf(s_status, sizeof(s_status), "已创建新文件夹");
        } else snprintf(s_status, sizeof(s_status), "创建文件夹失败");
        return APP_REDRAW_PAGE;
    }
    if (ui_rect_hit(ui_row_rect(3, 4, 176, 68), x, y)) {
        if (s_multi_mode && selected_batch_count() > 0) {
            prepare_delete_confirm(true);
            return APP_REDRAW_PAGE;
        }
        s_multi_mode = !s_multi_mode;
        if (!s_multi_mode) {
            for (size_t i = 0; i < s_count; ++i) s_entries[i].selected = false;
        }
        return APP_REDRAW_PAGE;
    }

    for (int r = 0; r < FILES_ROWS; ++r) {
        if (!ui_rect_hit(list_row_rect(r), x, y)) continue;
        size_t idx = (size_t)ctx->leaf * FILES_ROWS + (size_t)r;
        if (idx >= s_count) break;
        if (s_multi_mode) {
            s_entries[idx].selected = !s_entries[idx].selected;
            return APP_REDRAW_PAGE;
        }
        char full[FILES_PATH_MAX];
        if (!files_path_join_safe(FILES_ROOT, s_cur_dir, s_entries[idx].name, full, sizeof(full))) break;
        if (ev->type == UI_GESTURE_LONG_PRESS) {
            s_selected_idx = (int)idx;
            s_view = FM_DETAIL;
            return APP_REDRAW_PAGE;
        }
        if (s_entries[idx].is_dir) {
            snprintf(s_cur_dir, sizeof(s_cur_dir), "%s", full);
            ctx->leaf = 0;
            reload_dir(ctx);
            return APP_REDRAW_PAGE;
        }
        if (s_entries[idx].kind == FILES_KIND_IMAGE) {
            open_image_preview(full);
            return APP_REDRAW_FULL;
        }
        if (s_entries[idx].kind == FILES_KIND_TEXT) {
            open_text_preview(full);
            return APP_REDRAW_PAGE;
        }
        s_selected_idx = (int)idx;
        s_view = FM_DETAIL;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (s_view == FM_PREVIEW_IMG) {
        if (key == UI_KEY_1) { free_previews(); s_view = FM_BROWSE; return APP_REDRAW_FULL; }
        return APP_REDRAW_NONE;
    }
    if (s_view == FM_PREVIEW_TXT) {
        if (key == UI_KEY_1) {
            if (s_txt_page > 0) { s_txt_page--; return APP_REDRAW_PAGE; }
            free_previews(); s_view = FM_BROWSE; return APP_REDRAW_PAGE;
        }
        if (key == UI_KEY_3 && (size_t)(s_txt_page + 1) * 600 < s_txt_len) {
            s_txt_page++; return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == FM_CONFIRM_DEL || s_view == FM_DETAIL) {
        if (key == UI_KEY_1) { s_view = FM_BROWSE; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_1) {
        if (ctx->leaf > 0) { ctx->leaf--; return APP_REDRAW_PAGE; }
        char parent[FILES_PATH_MAX];
        if (files_path_parent(FILES_ROOT, s_cur_dir, parent, sizeof(parent))) {
            snprintf(s_cur_dir, sizeof(s_cur_dir), "%s", parent);
            ctx->leaf = 0;
            reload_dir(ctx);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_3 && ctx->leaf + 1 < page_count()) {
        ctx->leaf++;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key_long(app_ctx_t *ctx, int key) {
    if (key == UI_KEY_1) {
        if (s_view != FM_BROWSE) {
            free_previews();
            s_view = FM_BROWSE;
            return APP_REDRAW_PAGE;
        }
        // 在子目录或第 2+ 页长按左键直接返回上一级应用（交由主循环弹栈）。
        // Long KEY1 returns directly to the parent app via the loop's nav_stack.
        return APP_REDRAW_NONE;
    }
    (void)ctx;
    return APP_REDRAW_NONE;
}

static void key_hints(app_ctx_t *ctx, ui_key_hints_t *out) {
    if (s_view == FM_BROWSE) {
        out->left = ctx->leaf > 0 ? "上页 · 长按返回"
            : (strcmp(s_cur_dir, FILES_ROOT) == 0 ? "返回" : "上级目录");
        out->right = (ctx->leaf + 1 < page_count()) ? "下一页" : NULL;
    } else if (s_view == FM_PREVIEW_TXT) {
        out->left = s_txt_page > 0 ? "上页 · 长按返回" : "返回";
        out->right = (size_t)(s_txt_page + 1) * 600 < s_txt_len ? "下一页" : NULL;
    } else if (s_view == FM_CONFIRM_DEL) {
        out->left = "取消";
        out->right = NULL;
    } else {
        out->left = "返回";
        out->right = NULL;
    }
}

const app_desc_t app_files = {
    .title = "文件管理",
    .detail = "浏览与整理 TF 卡文件",
    .on_enter = on_enter,
    .on_exit = files_on_exit,
    .on_media_lost = on_media_lost,
    .render = render,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_key_long = on_key_long,
    .key_hints = key_hints,
};
