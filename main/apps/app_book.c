/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 图书书架、阅读、目录与页内进度；文件解析和排版由book模块负责。
 * Book shelf, reader, TOC and progress; book modules own parsing and pagination.
 *
 * 冻结：阅读翻页刷新按 bk_ref 档位执行；均衡/极速周期性 GC16，离开阅读页强制恢复板级方向与传感器配置。
 * Frozen: Reader refresh follows bk_ref with periodic GC16 on balanced/fast; leaving reader restores board rotation and sensor configuration.
 * 决策变更：统一全系统三键契约——正文阅读无弹窗时短按 KEY1/KEY3 翻上/下页，子页与弹窗短按 KEY1 返回/取消；
 * KEY2 始终走主循环整屏 GC16 强刷，非正文态 KEY3 开关应用菜单，长按三键统一由主循环处理（KEY1 回桌面 / KEY2 强刷 / KEY3 菜单）。
 * Shake/tap gestures are configurable and restore sensor state on exit. Render only paints.
 * 预渲染回调返回前收齐，避免菜单/锁屏绕过页内TTF锁。
 * 刷新档位变化：纯净每页 GC16；均衡 GL16/8 页；极速 DU/6 页，切章与工具弹窗退出时 GC16。
 * Refresh policy changed by product request: clean GC16 per page, balanced GL16/8 pages, fast DU/6 pages, with GC16 at chapter and modal exits.
 * 点按正文中央区域开关工具条；KEY2 随时触发整屏 GC16 清残影并重置周期计数。
 * 底部进度条始终可点按跳转；章节跳转须补排至目标正文偏移后再呈现，不能落在未完成分页的末页。
 * 用户授权基础管理：长按书架先看完整详情，清进度与删文件分别确认；失败保留待重试记录，不自动回收其他书进度。
 * 用户修订：单本管理为书架弹窗；管理页用于批量操作。分页和排序保留勾选，筛选/应用搜索及重扫清除勾选。
 * 失败进度仅按变更路径失效；删除后的清理重试保留到本次开机结束，不随切页释放。
 * 卡失效时先保存进度并关闭阅读资源，再由主循环回退字体；禁止自动续读失效挂载。
 * 用户修订：进入书架只询问续读，确认前不打开上次图书，避免大书阻塞书架。
 * 用户修订：图片点击后才加载，预览返回不改变正文分页；只缓存当前章节最近查看的一幅图，已读重复位置不冒充全书首次位置。
 * 用户授权：书签与摘录按路径/文件大小保存在图书目录隐藏 sidecar；数量由存储空间决定，清除阅读进度保留笔记，删除图书时清除笔记。
 * Frozen: Decision change to match the OS-wide physical key contract: clean reading maps short KEY1/KEY3 to prev/next, subviews/modals map short KEY1 to back/cancel, KEY2 always runs global GC16 refresh, non-reading KEY3 toggles the app menu, and 500ms holds are handled globally (KEY1 Home / KEY2 Refresh / KEY3 Menu). Screen turns commit on release without pressed decoration.
 * Shake is experimental, off by default, forward only; exit disables AOI2 and sleeps it. Render only paints.
 * Join preparation before returning callbacks so menus/lock cannot race the page-local TTF lock.
 * Refresh profiles: clean uses GC16 every turn, balanced GL16 every turn with GC16 every eight pages, and fast DU every turn with GC16 every six pages; chapter changes and modal exits GC16.
 * Tapping the center body band toggles tools; KEY2 performs a full GC16 ghost-clearing refresh anywhere.
 * The bottom progress track is always tappable; chapter seeking paginates through the target text offset before presenting.
 * User-authorized management shows full details before separate clear/delete confirmations; retain failed saves for retry without pruning other books.
 * User revision: single-book actions use a shelf dialog; full management is for batches. Paging/sorting preserve selection; filtering/applied search and rescanning clear it.
 * Invalidate failed progress only for changed paths; retain deletion cleanup retries across page exits for this boot.
 * Lost media saves progress and closes reader resources before global font fallback; never auto-resume an invalid mount.
 * User revision: offer resume on shelf entry; open the previous book only after confirmation to keep the shelf available.
 * User revision: load images only after a tap and return without repagination; cache one viewed image in the current chapter and distinguish visited-chapter origins from the first occurrence in the whole book.
 * User authorization: bookmarks and excerpts persist by path/file size in hidden book sidecars, with storage-bounded counts; clearing resume progress keeps notes, while deleting a book removes them.
 */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "app_registry.h"
#include "book_layout.h"
#include "book_notes.h"
#include "book_policy.h"
#include "book_progress.h"
#include "reading_stats.h"
#include "book_source.h"
#include "book_store.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "read_pico_init.h"
#include "read_pico_sd.h"
#include "read_pico_search.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "ui_gesture.h"
#include "ui_menu.h"
#include "sc7a20h_lab.h"

#define BOOK_ROWS 7
#define BOOK_TOC_ROWS 10
#define BOOK_NOTE_ROWS 8
#define BOOK_PX_MIN 36
#define BOOK_PX_MAX 72
#define BOOK_PX_STEP 4
#define BOOK_SHAKE_THS_MG 192
#define BOOK_SHAKE_DUR 2
#define BOOK_SIZE_SETTLE_MS 400
#define BOOK_TOOL_COUNT 12
#define BOOK_TOOL_ROWS 4
#define BOOK_SEARCH_RESULTS 7

typedef enum { SHELF, READING, TOC, MANAGE, BULK, SEARCH, BOOKMARKS, EXCERPTS, TYPOGRAPHY, CHAPTER_RESULTS } book_view_t;
typedef struct {
    char name[256];
    char path[BOOK_STORE_PATH_MAX];
    uint32_t size;
    bool is_flash;
    bool has_progress;
    uint8_t pct;
    uint32_t recent;
    bool selected, removed, search_match;
} shelf_entry_t;

static const char* TAG = "book";
static book_view_t s_view;
static shelf_entry_t* s_shelf;
static size_t s_shelf_capacity;
static int s_count, s_visible_count, s_filter;
static bool s_recent_sort;
static shelf_entry_t s_managed;
static bool s_delete_confirm, s_file_removed;
static char s_shelf_warning[128], s_manage_message[128];
static char s_query[65], s_search_draft[65], s_batch_message[128];
static book_view_t s_search_parent;
static bool s_chapter_search;
static size_t s_chapter_matches[BOOK_SEARCH_RESULTS], s_chapter_match_count;
static bool s_batch_confirm, s_batch_delete;
typedef struct pending_progress {
    char path[BOOK_STORE_PATH_MAX];
    book_progress_t value;
    bool dirty, progress_saved;
    book_progress_watch_t* watch;
    struct pending_progress* next;
} pending_progress_t;
static pending_progress_t* s_pending;
typedef struct delete_retry {
    shelf_entry_t entry;
    struct delete_retry* next;
} delete_retry_t;
static delete_retry_t* s_delete_retries;
static char s_latest_path[BOOK_STORE_PATH_MAX];
static bool s_save_failed;
static int64_t s_battery_poll_ms;
static bool s_low_battery_notified;
static bool s_pending_invalidated;
static int64_t s_save_retry_ms;
static unsigned s_store_revision;
static bool s_scan_pending, s_resume_pending, s_toolbar, s_clear_confirm;
static char s_resume_path[BOOK_STORE_PATH_MAX], s_resume_name[256];
static char s_message[128], s_storage[128], s_path[BOOK_STORE_PATH_MAX], s_title[128], s_book_name[128];
static char s_font_path[192];
static char* s_text;
static blk_t* s_blocks;
static size_t s_block_count;
static size_t s_text_len, s_chapter, s_page;
static bool s_image_open;
static uint8_t* s_image_pixels;
static uint16_t s_image_width, s_image_height;
static size_t s_image_block = SIZE_MAX;
static const char* s_image_error;
static char s_image_origin[224];
static uint32_t s_file_size;
static int s_px, s_unsaved;
static int64_t s_reading_tick_ms;
static uint32_t s_reading_acc_ms;
static book_notes_t s_notes;
static bool s_notes_error, s_note_delete_confirm;
static size_t s_note_delete_index;
static bool s_note_delete_index_valid;
static char s_note_message[128];
static int64_t s_poll_ms, s_last_turn_ms, s_size_settle_ms, s_sensor_ms;
static bool s_shake_enabled, s_sensor_on;
static bool s_sensor_saved;
static bool s_auto_candidate_landscape;
static int64_t s_auto_candidate_since;
static int64_t s_auto_sample_ms;
static sc7a20h_sensor_config_t s_sensor_config;
static book_shake_gate_t s_shake;
static EpdRect s_area;
static enum EpdDrawMode s_mode = MODE_GL16;
static bool s_full;
static bool s_page_turn;
static enum EpdDrawMode s_page_turn_mode = MODE_GL16;
static unsigned s_pages_since_clean;
static bool s_reader_landscape;
static bool s_reader_split;
static int s_pressed_control = -1;
static int64_t s_du_ms;
static unsigned s_du_count;
static EpdRect s_du_area;
static SemaphoreHandle_t s_draw_lock, s_prep_done;
static TaskHandle_t s_prep_task;
static uint8_t* s_next_fb;
static int s_next_page = -1, s_prep_page = -1;

static void render(app_ctx_t* ctx, uint8_t* fb);
static void scan_shelf(app_ctx_t* ctx);
static void free_book(void);
static bool load_chapter(app_ctx_t* ctx, size_t chapter, size_t offset, bool last_page);
static void save_progress(void);
static void invalidate_prep(void);
static void sort_shelf(app_ctx_t* ctx);
static void sensor_set(app_ctx_t* ctx, bool on);
static bool reader_sensor_required(void);
static void draw_control(uint8_t* fb, EpdRect rect, const char* label, int id) {
    if (id == 112 && s_query[0]) {
        ui_fill_round_rect(fb, rect, UI_BTN_RADIUS, UI_GRAY_BLACK);
        ui_text_vc(fb, rect.x + rect.width / 2, rect.y + rect.height / 2,
                   UI_PX_BTN, label, EPD_DRAW_ALIGN_CENTER, true);
        return;
    }
    if (s_pressed_control == id) ui_draw_pressed_round_rect(fb, rect, UI_BTN_RADIUS);
    ui_draw_button(fb, rect, label, false);
}
static void lock_draw(void) { if (s_draw_lock) xSemaphoreTake(s_draw_lock, portMAX_DELAY); }
static void unlock_draw(void) { if (s_draw_lock) xSemaphoreGive(s_draw_lock); }
static size_t fb_bytes(void) { return (size_t)epd_width() * epd_height() / 2; }
static unsigned side_margin(void);
// 旋转后差分列向32像素对齐，分界必须落在对齐边上才不会驱动进度条。
// Rotated diff columns expand to 32 pixels; align the boundary so body updates cannot drive the track.
static int reader_width(void) { return s_reader_landscape ? epd_width() : epd_height(); }
static int reader_height(void) { return s_reader_landscape ? epd_height() : epd_width(); }
static void reader_rotation_begin(void) {
    epd_set_rotation(s_reader_landscape ? EPD_ROT_LANDSCAPE : EPD_ROT_INVERTED_PORTRAIT);
}
static void reader_rotation_end(void) { epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT); }
static EpdRect reader_area(void) {
    int h = reader_height();
    return (EpdRect){0, 0, reader_width(), (h / 32) * 32};
}
static EpdRect body_rect(void) {
    int top = ttf_font_is_builtin() ? 140 : 24;
    EpdRect rect;
    if (s_reader_landscape) rect = (EpdRect){0, top, reader_width(), reader_height() - top - 90};
    else rect = (EpdRect){0, top, reader_width(), reader_height() - 120 - top};
    return book_layout_side_margins(rect, side_margin());
}
static EpdRect progress_rect(void) {
    if (s_reader_landscape) return (EpdRect){side_margin(), reader_height() - 76, reader_width() - 2 * (int)side_margin(), 72};
    EpdRect b = ui_bar_rect(0, 1);
    return (EpdRect){b.x, reader_height() - 120, b.width, 112};
}
static EpdRect row_rect(int row, bool toc) {
    int h = toc ? (UI_CONTENT_BOTTOM - UI_CONTENT_TOP) / BOOK_TOC_ROWS : UI_BTN_H + UI_GAP;
    return (EpdRect){UI_MARGIN, (toc ? UI_CONTENT_TOP : 308) + row * h, ui_content_width(), toc ? h - 6 : UI_BTN_H};
}
static EpdRect tool_rect(int i) {
    if (s_reader_landscape) {
        int width = (reader_width() - 2 * (int)side_margin() - 2 * UI_GAP) / 3;
        return (EpdRect){(int)side_margin() + (i % 3) * (width + UI_GAP),
                         reader_height() - 76 - BOOK_TOOL_ROWS * (UI_BTN_H + UI_GAP) + (i / 3) * (UI_BTN_H + UI_GAP),
                         width, UI_BTN_H};
    }
    return ui_grid_rect(i % 3, 3, i / 3, UI_BAR_TOP - BOOK_TOOL_ROWS * (UI_BTN_H + UI_GAP), UI_BTN_H);
}
static unsigned line_height_percent(void) {
    static const unsigned values[] = {135, 150, 175};
    unsigned preset = app_settings_book_line_height();
    return values[preset < 3 ? preset : 1];
}
static unsigned side_margin(void) {
    static const unsigned values[] = {24, 40, 60};
    unsigned preset = app_settings_book_margin();
    return values[preset < 3 ? preset : 1];
}
static EpdRect note_row_rect(int row) {
    int bottom = UI_CONTENT_BOTTOM - 144;
    int top = UI_CONTENT_TOP + 40;
    int height = (bottom - top) / BOOK_NOTE_ROWS;
    return (EpdRect){UI_MARGIN, top + row * height, ui_content_width(), height - 8};
}
static EpdRect note_delete_rect(int row) {
    EpdRect row_rect = note_row_rect(row);
    return (EpdRect){row_rect.x + row_rect.width - 132, row_rect.y + 6, 112, row_rect.height - 12};
}
static EpdRect note_action_rect(int index) {
    return ui_row_rect(index, 3, UI_CONTENT_BOTTOM - 72, 56);
}
static int leaves(void) {
    size_t count = s_view == TOC ? book_chapter_count() :
        s_view == BOOKMARKS ? s_notes.bookmark_count :
        s_view == EXCERPTS ? s_notes.excerpt_count : (size_t)s_visible_count;
    int rows = s_view == TOC ? BOOK_TOC_ROWS :
        (s_view == BOOKMARKS || s_view == EXCERPTS) ? BOOK_NOTE_ROWS : BOOK_ROWS;
    size_t pages = count ? 1 + (count - 1) / (size_t)rows : 1;
    return pages > INT_MAX ? INT_MAX : (int)pages;
}

// 截断必须停在UTF8字符边界。/ Truncation must stop at a UTF8 character boundary.
static void copy_text(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xc0) == 0x80) --n;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}
static void fit_text(char* text, int px, int width) {
    while (*text && ttf_text_width_px(px, text) > width) {
        size_t n = strlen(text) - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
    }
}
static uint32_t chapter_end(void) {
    return s_chapter + 1 < book_chapter_count()
        ? book_chapter_byte_offset(s_chapter + 1) : book_total_bytes();
}
static unsigned percent(size_t page) {
    uint32_t total = book_total_bytes();
    if (!total) return 0;
    if (book_layout_complete() && s_chapter + 1 == book_chapter_count() && page + 1 == book_layout_page_count()) return 100;
    uint32_t off = book_position_bytes(book_chapter_byte_offset(s_chapter), chapter_end(),
                                       book_layout_page_start_offset(page), s_text_len);
    return (unsigned)((uint64_t)off * 100 / total);
}
static pending_progress_t* pending_find(const char* path) {
    for (pending_progress_t* p = s_pending; p; p = p->next) if (!strcmp(p->path, path)) return p;
    return NULL;
}
static int layout_name(uint8_t* fb, const char* name, int y, bool draw);
static EpdRect manage_panel(void) {
    int height = layout_name(NULL, s_managed.name, 0, false) + 420;
    return (EpdRect){UI_MARGIN - 16, 190 + (876 - height) / 2, ui_content_width() + 32, height};
}
static EpdRect manage_rect(int index, int count) {
    EpdRect panel = manage_panel();
    return ui_row_rect(index, count, panel.y + panel.height - 94, 76);
}
static EpdRect batch_rect(int id) {
    if (id < 3) return ui_row_rect(id, 3, 978, 48);
    return ui_row_rect(id - 3, 2, 1038, 48);
}
static EpdRect search_rect(int id) {
    if (id < 40) {
        int width = (ui_content_width() - 54) / 10;
        return (EpdRect){UI_MARGIN + id % 10 * (width + 6), 388 + id / 10 * 100, width, 88};
    }
    if (id < 43) return ui_row_rect(id - 40, 3, 808, 80);
    return ui_bar_rect(id - 43, 2);
}
static size_t selected_count(void) {
    size_t selected = 0;
    for (int i = 0; i < s_count; ++i) if (s_shelf[i].selected) ++selected;
    return selected;
}
static void clear_selection(void) {
    for (int i = 0; i < s_count; ++i) s_shelf[i].selected = false;
}
static void toggle_selection(int index) {
    if (index >= 0 && index < s_visible_count) s_shelf[index].selected = !s_shelf[index].selected;
}
static void select_page(int page) {
    for (int i = page * BOOK_ROWS; i < s_visible_count && i < (page + 1) * BOOK_ROWS; ++i) s_shelf[i].selected = true;
}
static const char* search_keys(void) {
    return "1234567890" "qwertyuiop" "asdfghjkl-" "zxcvbnm._'";
}
static void search_begin(void) {
    s_chapter_search = false;
    s_search_parent = s_view;
    memcpy(s_search_draft, s_query, sizeof(s_query));
    s_view = SEARCH;
}
static void refresh_search_matches(void) {
    for (int i = 0; i < s_count; ++i)
        s_shelf[i].search_match = read_pico_search_match(s_shelf[i].name, s_query);
}
static void search_finish(app_ctx_t* ctx, bool apply) {
    if (s_chapter_search) {
        s_chapter_search = false;
        if (apply && s_text && s_search_draft[0]) {
            s_chapter_match_count = book_text_search(s_text, s_text_len, s_search_draft, 0,
                                                      s_chapter_matches, BOOK_SEARCH_RESULTS);
            s_view = CHAPTER_RESULTS;
            sensor_set(ctx, false);
        } else { s_chapter_match_count = 0; s_view = READING; sensor_set(ctx, reader_sensor_required()); }
        memset(s_search_draft, 0, sizeof(s_search_draft));
        return;
    }
    s_view = s_search_parent;
    if (apply) {
        memcpy(s_query, s_search_draft, sizeof(s_query));
        refresh_search_matches();
        clear_selection();
        sort_shelf(ctx);
        s_batch_message[0] = 0;
    }
    memset(s_search_draft, 0, sizeof(s_search_draft));
}
static app_redraw_t search_action(app_ctx_t* ctx, int id) {
    size_t len = strlen(s_search_draft);
    if ((id >= 0 && id < 40) || id == 40) {
        if (len < sizeof(s_search_draft) - 1) {
            s_search_draft[len] = id == 40 ? ' ' : search_keys()[id];
            s_search_draft[len + 1] = 0;
        }
    } else if (id == 41 && len) s_search_draft[len - 1] = 0;
    else if (id == 42) s_search_draft[0] = 0;
    else if (id == 43 || id == 44) { search_finish(ctx, id == 44); return APP_REDRAW_PAGE; }
    return APP_REDRAW_AREA;
}
static bool pending_reserve(const char* path) {
    if (pending_find(path)) return true;
    pending_progress_t* p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sizeof(*p));
    if (!p) return false;
    memset(p, 0, sizeof(*p));
    copy_text(p->path, sizeof(p->path), path);
    // 阅读页内注册；传书 HTTP 尚未启动，退出后也不注销失败项。
    // Register in reading before transfer HTTP starts; failed records survive page exit.
    p->watch = book_progress_watch_create(path);
    if (!p->watch) { free(p); return false; }
    pending_progress_t** tail = &s_pending;
    while (*tail) tail = &(*tail)->next;
    *tail = p;
    return true;
}
static bool pending_restore(const char* path, uint32_t size, book_progress_t* out) {
    pending_progress_t* pending = pending_find(path);
    if (!pending || !pending->dirty || pending->value.file_size != size) return false;
    *out = pending->value;
    return true;
}
static void pending_discard(const char* path) {
    pending_progress_t** p = &s_pending;
    while (*p) {
        if (!strcmp((*p)->path, path)) {
            pending_progress_t* old = *p;
            *p = old->next;
            book_progress_watch_destroy(old->watch);
            free(old);
            break;
        }
        p = &(*p)->next;
    }
    s_save_failed = false;
    for (pending_progress_t* item = s_pending; item; item = item->next) if (item->dirty) s_save_failed = true;
}
static void pending_mark_latest(const char* path) {
    pending_progress_t** item = &s_pending;
    while (*item && strcmp((*item)->path, path)) item = &(*item)->next;
    if (*item) {
        pending_progress_t* current = *item;
        *item = current->next;
        current->next = NULL;
        pending_progress_t** tail = &s_pending;
        while (*tail) tail = &(*tail)->next;
        *tail = current;
    }
    copy_text(s_latest_path, sizeof(s_latest_path), path);
}
static void pending_drop_invalidated(void) {
    pending_progress_t* p = s_pending;
    while (p) {
        pending_progress_t* next = p->next;
        if (book_progress_watch_invalidated(p->watch)) {
            if (p->dirty) s_pending_invalidated = true;
            pending_discard(p->path);
        }
        p = next;
    }
}
static delete_retry_t* delete_retry_find(const char* path) {
    for (delete_retry_t* p = s_delete_retries; p; p = p->next)
        if (!strcmp(p->entry.path, path)) return p;
    return NULL;
}
static bool pending_flush(pending_progress_t* p) {
    if (!p->dirty) return true;
    if (!p->progress_saved) {
        if (book_progress_save(p->path, &p->value) != ESP_OK) return false;
        p->progress_saved = true;
    }
    if (!strcmp(p->path, s_latest_path) && book_progress_set_last_path(p->path) != ESP_OK) return false;
    p->dirty = false;
    return true;
}
static void retry_progress(void) {
    s_save_failed = false;
    bool had_dirty = false;
    pending_progress_t* p = s_pending;
    while (p) {
        pending_progress_t* next = p->next;
        bool dirty = p->dirty;
        had_dirty |= dirty;
        if (s_text && !strcmp(p->path, s_path)) { p = next; continue; }
        if (!pending_flush(p)) s_save_failed = true;
        else if (strcmp(p->path, s_path)) pending_discard(p->path);
        else if (dirty) s_unsaved = 0;
        p = next;
    }
    if (s_text && (s_unsaved || had_dirty)) save_progress();
    s_save_failed = false;
    for (pending_progress_t* item = s_pending; item; item = item->next) if (item->dirty) s_save_failed = true;
}

/* ---- 管理详情 / Management details ---- */
static int layout_name(uint8_t* fb, const char* name, int y, bool draw) {
    const char* at = name;
    while (*at) {
        char line[256];
        size_t used = 0;
        while (at[used]) {
            unsigned char first = (unsigned char)at[used];
            size_t n = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
            size_t remain = strlen(at + used);
            if (n > remain) n = 1;
            if (used + n >= sizeof(line)) break;
            memcpy(line + used, at + used, n);
            line[used + n] = 0;
            if (ttf_text_width_px(UI_PX_CAPTION, line) > ui_content_width() && used) break;
            used += n;
        }
        if (!used) break;
        line[used] = 0;
        if (draw) ui_text(fb, UI_MARGIN, y, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
        y += 36;
        at += used;
    }
    return y;
}
static int draw_wrapped_name(uint8_t* fb, const char* name, int y) {
    return layout_name(fb, name, y, true);
}
static void draw_manage(uint8_t* fb) {
    EpdRect panel = manage_panel();
    ui_fill_round_rect(fb, panel, UI_BTN_RADIUS, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, panel, UI_BTN_RADIUS, UI_GRAY_BLACK);
    ui_text(fb, UI_MARGIN, panel.y + 18, UI_PX_BODY,
            s_clear_confirm ? (s_delete_confirm ? "确认删除文件？" : "确认清除进度？") : "图书详情", EPD_DRAW_ALIGN_LEFT, false);
    int y = draw_wrapped_name(fb, s_managed.name, panel.y + 74) + 12;
    char info[96];
    snprintf(info, sizeof(info), "%s · %s · %.2f MB", s_managed.is_flash ? "内置存储" : "TF 卡",
             strrchr(s_managed.name, '.') ? strrchr(s_managed.name, '.') + 1 : "", s_managed.size / 1048576.0);
    ui_text(fb, UI_MARGIN, y, UI_PX_CAPTION, info, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(info, sizeof(info), s_managed.has_progress ? "阅读进度 %u%%" : "尚无阅读进度", s_managed.pct);
    ui_text(fb, UI_MARGIN, y + 44, UI_PX_CAPTION, info, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 88, UI_PX_CAPTION, s_managed.is_flash ? "/flash/books" : "/sdcard/books", EPD_DRAW_ALIGN_LEFT, false);
    if (s_manage_message[0]) ui_text(fb, UI_MARGIN, panel.y + panel.height - 198, UI_PX_CAPTION, s_manage_message, EPD_DRAW_ALIGN_LEFT, false);
    if (s_clear_confirm) {
        ui_text(fb, UI_MARGIN, panel.y + panel.height - 142, UI_PX_CAPTION, s_delete_confirm ? "删除后文件无法恢复" : "仅清阅读进度，保留图书文件", EPD_DRAW_ALIGN_LEFT, false);
        draw_control(fb, manage_rect(0, 2), "取消", 300);
        draw_control(fb, manage_rect(1, 2), s_delete_confirm ? "确认删除" : "确认清除", 301);
    } else if (s_file_removed) {
        draw_control(fb, manage_rect(0, 2), "关闭", 400);
        draw_control(fb, manage_rect(1, 2), "重试清理", 403);
    } else {
        draw_control(fb, manage_rect(0, 3), "关闭", 400);
        draw_control(fb, manage_rect(1, 3), "清进度", 401);
        draw_control(fb, manage_rect(2, 3), "删除文件", 402);
    }
}
static void draw_search(uint8_t* fb) {
    ui_clear_page(fb);
    ui_draw_header(fb, s_chapter_search ? "搜索本章" : "搜索图书",
                   s_chapter_search ? "在当前已加载章节中查找关键词" : "输入拼音首字母、完整拼音或英文");
    EpdRect field = {UI_MARGIN, 200, ui_content_width(), 88};
    ui_draw_round_rect(fb, field, UI_BTN_RADIUS, UI_GRAY_BLACK);
    const char* tail = s_search_draft;
    while (*tail && ttf_text_width_px(UI_PX_BODY, tail) > field.width - 2 * UI_PAD) ++tail;
    ui_text_vc(fb, field.x + UI_PAD, field.y + field.height / 2, UI_PX_BODY, tail, EPD_DRAW_ALIGN_LEFT, false);
    char count[48];
    snprintf(count, sizeof(count), "%u/64 · 清空后应用可显示全部", (unsigned)strlen(s_search_draft));
    ui_text(fb, UI_MARGIN, 310, UI_PX_CAPTION, count, EPD_DRAW_ALIGN_LEFT, false);
    const char* keys = search_keys();
    for (int i = 0; i < 40; ++i) {
        char label[2] = {keys[i], 0};
        draw_control(fb, search_rect(i), label, 500 + i);
    }
    draw_control(fb, search_rect(40), "空格", 540);
    draw_control(fb, search_rect(41), "退格", 541);
    draw_control(fb, search_rect(42), "清空", 542);
    ui_text(fb, UI_MARGIN, 938, UI_PX_CAPTION, "应用清除勾选 · KEY1 取消", EPD_DRAW_ALIGN_LEFT, false);
    draw_control(fb, search_rect(43), "取消", 543);
    draw_control(fb, search_rect(44), "应用", 544);
    ui_draw_menu_handle(fb, false);
}
static void draw_chapter_results(uint8_t *fb) {
    ui_clear_page(fb);
    char subtitle[96];
    snprintf(subtitle, sizeof(subtitle), "“%s” · 当前章节", s_search_draft);
    ui_draw_header(fb, "本章搜索结果", subtitle);
    if (!s_chapter_match_count) ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 56, UI_PX_BODY, "没有找到匹配内容", EPD_DRAW_ALIGN_LEFT, false);
    for (size_t i = 0; i < s_chapter_match_count && i < BOOK_SEARCH_RESULTS; ++i) {
        EpdRect rect = row_rect((int)i, false);
        size_t begin = s_chapter_matches[i] > 24 ? s_chapter_matches[i] - 24 : 0;
        while (begin && (((unsigned char)s_text[begin] & 0xc0) == 0x80)) --begin;
        size_t len = s_text_len - begin;
        if (len > 96) len = 96;
        while (len && begin + len < s_text_len && (((unsigned char)s_text[begin + len] & 0xc0) == 0x80)) --len;
        char snippet[112]; memcpy(snippet, s_text + begin, len); snippet[len] = 0;
        for (size_t j = 0; j < len; ++j) if (snippet[j] == '\n' || snippet[j] == '\r' || snippet[j] == '\t') snippet[j] = ' ';
        fit_text(snippet, UI_PX_CAPTION, rect.width - 2 * UI_PAD);
        ui_draw_round_rect(fb, rect, UI_BTN_RADIUS, UI_GRAY_BLACK);
        ui_text(fb, rect.x + UI_PAD, rect.y + 18, UI_PX_CAPTION, snippet, EPD_DRAW_ALIGN_LEFT, false);
    }
    ui_text(fb, UI_MARGIN, UI_CONTENT_BOTTOM - 96, UI_PX_CAPTION, "点击片段跳转 · KEY1 返回阅读", EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_menu_handle(fb, false);
}
static void draw_batch_confirmation(uint8_t* fb) {
    if (!s_batch_confirm) return;
    EpdRect panel = {UI_MARGIN - 12, 410, ui_content_width() + 24, 330};
    ui_fill_round_rect(fb, panel, UI_BTN_RADIUS, UI_GRAY_WHITE);
    ui_draw_round_rect(fb, panel, UI_BTN_RADIUS, UI_GRAY_BLACK);
    char title[96];
    snprintf(title, sizeof(title), "%s %u 本图书？", s_batch_delete ? "删除" : "清除进度：", (unsigned)selected_count());
    ui_text(fb, UI_MARGIN, 438, UI_PX_BODY, title, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, 508, UI_PX_CAPTION, s_batch_delete ? "删除文件不可撤销，失败项可重试" : "仅清阅读进度，所有文件保留", EPD_DRAW_ALIGN_LEFT, false);
    draw_control(fb, ui_row_rect(0, 2, 620, UI_BTN_H), "取消", 600);
    draw_control(fb, ui_row_rect(1, 2, 620, UI_BTN_H), "确认", 601);
}
static void save_progress(void) {
    if (!s_text || !s_path[0] || !book_layout_page_count()) return;
    bool was_failed = s_save_failed;
    pending_progress_t* pending = pending_find(s_path);
    if (!pending) { s_save_failed = true; return; }
    book_progress_t p = {
        .file_size = s_file_size, .chapter = (uint16_t)s_chapter,
        .byte_off = (uint32_t)book_layout_page_start_offset(s_page),
        .px = (uint8_t)s_px, .pct = (uint8_t)percent(s_page),
        .last_open_s = 0,
    };
    if (s_book_name[0]) reading_stats_set_last_book(s_book_name, p.pct);
    if (!pending->dirty || pending->value.file_size != p.file_size || pending->value.chapter != p.chapter ||
        pending->value.byte_off != p.byte_off || pending->value.px != p.px || pending->value.pct != p.pct) {
        pending->value = p;
        pending->progress_saved = false;
    }
    pending->dirty = true;
    if (pending_flush(pending)) {
        s_unsaved = 0;
        s_save_failed = false;
        for (pending_progress_t* item = s_pending; item; item = item->next) if (item->dirty) s_save_failed = true;
    }
    else { if (!s_unsaved) s_unsaved = 1; s_save_failed = true; }
    if (was_failed != s_save_failed) invalidate_prep();
}
static void load_notes_for_book(void) {
    book_notes_free(&s_notes);
    memset(&s_notes, 0, sizeof(s_notes));
    esp_err_t err = book_notes_load(s_path, s_file_size, &s_notes);
    s_notes_error = err != ESP_OK && err != ESP_ERR_NOT_FOUND;
    s_note_delete_confirm = false;
    s_note_delete_index = 0;
    s_note_delete_index_valid = false;
    s_note_message[0] = 0;
}
static void invalidate_prep(void) { s_next_page = s_prep_page = -1; }
static void free_image(void) {
    free(s_image_pixels); s_image_pixels = NULL;
    s_image_width = s_image_height = 0;
    s_image_block = SIZE_MAX; s_image_open = false;
    s_image_error = NULL; s_image_origin[0] = 0;
}
static app_redraw_t close_image(app_ctx_t *ctx) {
    s_image_open = false;
    invalidate_prep();
    sensor_set(ctx, reader_sensor_required());
    s_full = true;
    return APP_REDRAW_PAGE;
}

/* ---- 绘制与预渲染 / Drawing and preparation ---- */
static void draw_reader(uint8_t* fb, size_t page) {
    reader_rotation_begin();
    ui_clear_page(fb);
    book_layout_draw_page(fb, page, body_rect(), s_px);
    if (ttf_font_is_builtin()) {
        ui_text(fb, (int)side_margin(), 16, UI_PX_CAPTION, "内建字体缺字，请选择 TF 卡字体", EPD_DRAW_ALIGN_LEFT, false);
        ui_draw_button(fb, (EpdRect){(int)side_margin(), 52, reader_width() - 2 * (int)side_margin(), 72},
                       "打开字体页 / 无卡正文可能缺字", false);
    }
    EpdRect track = progress_rect();
    epd_fill_rect((EpdRect){track.x, track.y + 16, track.width, 12}, UI_GRAY_LIGHT, fb);
    size_t chapters = book_chapter_count();
    if (chapters <= 40 && book_total_bytes()) {
        for (size_t i = 1; i < chapters; ++i) {
            int x = track.x + (uint64_t)book_chapter_byte_offset(i) * track.width / book_total_bytes();
            epd_fill_rect((EpdRect){x, track.y + 10, 2, 24}, UI_GRAY_BLACK, fb);
        }
    }
    epd_fill_rect((EpdRect){track.x, track.y + 16, track.width * (int)percent(page) / 100, 12}, UI_GRAY_BLACK, fb);
    char name[96], suffix[64], line[176];
    if (book_layout_complete())
        snprintf(suffix, sizeof(suffix), " · 本章 %u/%u · 全书 %u%%", (unsigned)page + 1,
                 (unsigned)book_layout_page_count(), percent(page));
    else snprintf(suffix, sizeof(suffix), " · 本章 %u/… · 全书 %u%%", (unsigned)page + 1, percent(page));
    copy_text(name, sizeof(name), s_save_failed ? "进度未保存，稍后重试" : s_title);
    fit_text(name, UI_PX_CAPTION, track.width - ttf_text_width_px(UI_PX_CAPTION, suffix));
    snprintf(line, sizeof(line), "%s%s", name, suffix);
    ui_text(fb, track.x, track.y + 36, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
    if (s_toolbar) {
        EpdRect panel = {(int)side_margin() - 8, tool_rect(0).y - 110, reader_width() - 2 * (int)side_margin() + 16, BOOK_TOOL_ROWS * (UI_BTN_H + UI_GAP) + 110};
        ui_clear_rect_fast(fb, (EpdRect){panel.x, panel.y - 24, panel.width, panel.height + 24});
        epd_fill_rect((EpdRect){panel.x, panel.y - 16, panel.width, 8}, UI_GRAY_LIGHT, fb);
        ui_hairline(fb, panel.y - 8, panel.x, panel.width, UI_GRAY_BLACK);
        ui_text(fb, (int)side_margin(), panel.y + 8, UI_PX_CAPTION,
                "KEY1 上页 · KEY2 强刷 · KEY3 下页", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, (int)side_margin(), panel.y + 40, UI_PX_CAPTION,
                "点中间开关工具 · 左右点按/滑动翻页", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, (int)side_margin(), panel.y + 72, UI_PX_CAPTION,
                "长按 KEY1 桌面 · 长按 KEY3 菜单", EPD_DRAW_ALIGN_LEFT, false);
        EpdRect size_box = {reader_width() - (int)side_margin() - 180, panel.y + 8, 180, 84};
        ui_draw_round_rect(fb, size_box, UI_CHIP_RADIUS, UI_GRAY_BLACK);
        ui_text(fb, size_box.x + size_box.width - UI_PAD, size_box.y + 8, UI_PX_CAPTION,
                "当前字号", EPD_DRAW_ALIGN_RIGHT, false);
        char size_hint[32];
        snprintf(size_hint, sizeof(size_hint), "%d px", s_px);
        ui_text(fb, size_box.x + size_box.width - UI_PAD, size_box.y + 44, UI_PX_CAPTION,
                size_hint, EPD_DRAW_ALIGN_RIGHT, false);
        static const char* refresh_names[] = {"纯净", "均衡", "极速"};
        static const char* rotation_names[] = {"竖屏", "横屏", "自动"};
        static const char* gesture_names[] = {"手势关", "晃动翻页", "轻敲翻页"};
        char refresh[24], rotation[24], gesture[24];
        snprintf(refresh, sizeof(refresh), "刷新 %s", refresh_names[app_settings_book_refresh() % 3]);
        snprintf(rotation, sizeof(rotation), "方向 %s", rotation_names[app_settings_book_rotation() % 3]);
        snprintf(gesture, sizeof(gesture), "%s", gesture_names[app_settings_book_gesture() % 3]);
        const char* labels[] = {"目录", "字号 −", "字号 +", "书签", "摘录", gesture,
                                "强刷", "书架", refresh, "排版", rotation, "搜索"};
        for (int i = 0; i < BOOK_TOOL_COUNT; ++i) draw_control(fb, tool_rect(i), labels[i], 200 + i);
    }
    ui_draw_menu_handle(fb, false);
    reader_rotation_end();
}
static void draw_note_view(uint8_t *fb, app_ctx_t *ctx) {
    bool bookmarks = s_view == BOOKMARKS;
    size_t count = bookmarks ? s_notes.bookmark_count : s_notes.excerpt_count;
    ui_clear_page(fb);
    ui_draw_header(fb, bookmarks ? "本书书签" : "本书摘录", s_book_name);
    if (s_note_delete_confirm) {
        EpdRect panel = {UI_MARGIN - 12, 420, ui_content_width() + 24, 340};
        ui_fill_round_rect(fb, panel, UI_BTN_RADIUS, UI_GRAY_WHITE);
        ui_draw_round_rect(fb, panel, UI_BTN_RADIUS, UI_GRAY_BLACK);
        ui_text(fb, UI_MARGIN, 458, UI_PX_BODY, bookmarks ? "删除这个书签？" : "删除这条摘录？", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, UI_MARGIN, 526, UI_PX_CAPTION, "只删除笔记，不会改动图书或阅读进度", EPD_DRAW_ALIGN_LEFT, false);
        draw_control(fb, ui_row_rect(0, 2, 630, UI_BTN_H), "取消", 830);
        draw_control(fb, ui_row_rect(1, 2, 630, UI_BTN_H), "确认删除", 831);
        ui_draw_menu_handle(fb, false);
        return;
    }
    if (s_note_message[0]) {
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP, UI_PX_CAPTION, s_note_message, EPD_DRAW_ALIGN_LEFT, false);
    }
    if (s_notes_error) {
        char hint[] = "本书笔记不可读取或与当前文件版本不匹配";
        fit_text(hint, UI_PX_BODY, ui_content_width());
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 24, UI_PX_BODY, hint, EPD_DRAW_ALIGN_LEFT, false);
    } else if (!count) {
        char hint[128];
        snprintf(hint, sizeof(hint), bookmarks ? "还没有书签，阅读到想回看的位置后点下方添加" :
                 "还没有摘录，阅读到想保存的内容后点下方摘录本页");
        fit_text(hint, UI_PX_BODY, ui_content_width());
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 48, UI_PX_BODY, hint, EPD_DRAW_ALIGN_LEFT, false);
    }
    int pages = leaves();
    char page_hint[80];
    snprintf(page_hint, sizeof(page_hint), "%u / %u · 上下滑动翻页 · KEY1 返回",
             (unsigned)ctx->leaf + 1, (unsigned)pages);
    ui_text(fb, UI_MARGIN, UI_CONTENT_BOTTOM - 116, UI_PX_CAPTION, page_hint, EPD_DRAW_ALIGN_LEFT, false);
    for (int row = 0; row < BOOK_NOTE_ROWS; ++row) {
        size_t index = (size_t)ctx->leaf * BOOK_NOTE_ROWS + (size_t)row;
        if (index >= count) break;
        EpdRect rect = note_row_rect(row);
        EpdRect remove = note_delete_rect(row);
        if (s_pressed_control == 800 + row) ui_draw_pressed_round_rect(fb, remove, UI_BTN_RADIUS);
        ui_draw_round_rect(fb, rect, UI_BTN_RADIUS, UI_GRAY_BLACK);
        char first[224], second[224] = {0};
        if (bookmarks) {
            const book_note_position_t *p = &s_notes.bookmarks[index];
            snprintf(first, sizeof(first), "第 %u 章 · 全书 %u%%", (unsigned)p->chapter + 1, p->percent);
            snprintf(second, sizeof(second), "位置 %u 字节", (unsigned)p->byte_off);
        } else {
            const book_note_excerpt_t *e = &s_notes.excerpts[index];
            snprintf(first, sizeof(first), "第 %u 章 · 全书 %u%%", (unsigned)e->position.chapter + 1, e->position.percent);
            snprintf(second, sizeof(second), "%s", e->text);
            for (char *p = second; *p; ++p) if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
        }
        fit_text(first, UI_PX_CAPTION, rect.width - remove.width - 3 * UI_PAD);
        fit_text(second, UI_PX_CAPTION, rect.width - remove.width - 3 * UI_PAD);
        ui_text(fb, rect.x + UI_PAD, rect.y + 10, UI_PX_CAPTION, first, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, rect.x + UI_PAD, rect.y + 46, UI_PX_CAPTION, second, EPD_DRAW_ALIGN_LEFT, false);
        draw_control(fb, remove, "删除", 800 + row);
    }
    draw_control(fb, note_action_rect(0), bookmarks ? "添加当前位置" : "摘录本页", 820);
    draw_control(fb, note_action_rect(1), "导出 MD", 821);
    draw_control(fb, note_action_rect(2), "返回阅读", 822);
    ui_draw_menu_handle(fb, false);
}
static void draw_typography(uint8_t *fb) {
    ui_clear_page(fb);
    ui_draw_header(fb, "排版设置", s_book_name);
    const char *lh[] = {"紧凑 · 1.35x", "标准 · 1.5x", "宽松 · 1.75x"};
    const char *mg[] = {"窄 · 24px", "标准 · 40px", "宽 · 60px"};
    char weight[64];
    uint16_t w = app_settings_book_weight();
    if (!ttf_font_has_variable_weight()) snprintf(weight, sizeof(weight), "字重 · 当前字体不支持");
    else if (!w) snprintf(weight, sizeof(weight), "字重 · 默认");
    else snprintf(weight, sizeof(weight), "字重 · %u", (unsigned)w);
    draw_control(fb, ui_row_rect(0, 1, 340, 84), lh[app_settings_book_line_height() % 3], 900);
    draw_control(fb, ui_row_rect(0, 1, 456, 84), mg[app_settings_book_margin() % 3], 901);
    draw_control(fb, ui_row_rect(0, 1, 572, 84), weight, 902);
    draw_control(fb, ui_row_rect(0, 1, 688, 84), "返回阅读", 903);
    ui_draw_menu_handle(fb, false);
}
static void prep_task(void* arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        lock_draw();
        if (s_prep_page >= 0 && s_text && s_next_fb) {
            draw_reader(s_next_fb, (size_t)s_prep_page);
            s_next_page = s_prep_page;
        }
        unlock_draw();
        xSemaphoreGive(s_prep_done);
    }
}
static void ensure_prep(void) {
    if (!s_draw_lock) s_draw_lock = xSemaphoreCreateMutex();
    if (!s_prep_done) s_prep_done = xSemaphoreCreateBinary();
    if (!s_next_fb) s_next_fb = heap_caps_aligned_alloc(16, fb_bytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_prep_task && s_draw_lock && s_prep_done && s_next_fb) {
        if (xTaskCreatePinnedToCore(prep_task, "book_prep", 12 * 1024, NULL, 3, &s_prep_task, 1) != pdPASS)
            s_prep_task = NULL;
    }
}
static bool kick_prep(void) {
    if (!s_prep_task || s_view != READING || s_image_open || s_toolbar || s_clear_confirm ||
        s_page + 1 >= book_layout_page_count() || s_next_page == (int)s_page + 1) return false;
    s_prep_page = (int)s_page + 1;
    xSemaphoreTake(s_prep_done, 0);
    xTaskNotifyGive(s_prep_task);
    return true;
}
static void draw_image(uint8_t* fb) {
    ui_clear_page(fb);
    bool title_image = s_image_block < s_block_count && s_blocks[s_image_block].image_title;
    ui_draw_header(fb, s_image_error ? "图片未加载" : title_image ? "标题图预览" : "图片预览", "点击图片或返回按钮回到正文");
    int top = UI_CONTENT_TOP;
    if (s_image_origin[0]) {
        char line[sizeof(s_image_origin)]; copy_text(line, sizeof(line), s_image_origin);
        fit_text(line, UI_PX_CAPTION, ui_content_width());
        ui_text(fb, UI_MARGIN, top, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
        top += UI_PX_CAPTION + UI_GAP;
    }
    EpdRect area = {UI_MARGIN, top, ui_content_width(), UI_CONTENT_BOTTOM - top};
    if (s_image_pixels && s_image_width && s_image_height) {
        int width = s_image_width, height = s_image_height;
        if (width > area.width) { height = height * area.width / width; width = area.width; }
        if (height > area.height) { width = width * area.height / height; height = area.height; }
        if (width < 1) width = 1;
        if (height < 1) height = 1;
        int left = area.x + (area.width - width) / 2, y0 = area.y + (area.height - height) / 2;
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
            size_t src = (size_t)((int64_t)y * s_image_height / height) * s_image_width +
                         (size_t)((int64_t)x * s_image_width / width);
            epd_draw_pixel(left + x, y0 + y, s_image_pixels[src], fb);
        }
    } else ui_text(fb, area.x, area.y + area.height / 2, UI_PX_CAPTION,
                   s_image_error ? s_image_error : "图片不可用", EPD_DRAW_ALIGN_LEFT, false);
    ui_draw_button(fb, ui_bar_rect(0, 1), "返回正文", false);
    ui_draw_menu_handle(fb, false);
}
static void render(app_ctx_t* ctx, uint8_t* fb) {
    lock_draw();
    if (s_image_open) { draw_image(fb); unlock_draw(); return; }
    if (s_view == SEARCH) { draw_search(fb); unlock_draw(); return; }
    if (s_view == TYPOGRAPHY) { draw_typography(fb); unlock_draw(); return; }
    if (s_view == CHAPTER_RESULTS) { draw_chapter_results(fb); unlock_draw(); return; }
    if (s_view == BOOKMARKS || s_view == EXCERPTS) { draw_note_view(fb, ctx); unlock_draw(); return; }
    if (s_view == READING && s_text) {
        draw_reader(fb, s_page);
        if (s_low_battery_notified) {
            EpdRect alert = {UI_MARGIN, UI_CONTENT_BOTTOM - 52, ui_content_width(), 48};
            epd_fill_rect(alert, UI_GRAY_LIGHT, fb);
            ui_text_vc(fb, alert.x + UI_PAD, alert.y + alert.height / 2, UI_PX_CAPTION,
                       "低电量：阅读进度已保存，请充电", EPD_DRAW_ALIGN_LEFT, false);
        }
    } else {
        ui_clear_page(fb);
        ui_draw_header(fb, s_view == TOC ? "目录 Contents" : "图书 Books", s_view == TOC ? s_title : s_storage);
        if (s_view != TOC) {
            draw_control(fb, ui_row_rect(0, 3, 176, 72), s_filter == 0 ? "全部来源" : s_filter == 1 ? "TF 卡" : "内置", 110);
            draw_control(fb, ui_row_rect(1, 3, 176, 72), s_recent_sort ? "按最近" : "按名称", 111);
            draw_control(fb, ui_row_rect(2, 3, 176, 72), s_query[0] ? "搜索中" : "搜索", 112);
            char shelf_hint[128];
            snprintf(shelf_hint, sizeof(shelf_hint), "已选 %u 本 · 筛选/重扫清勾选", (unsigned)selected_count());
            ui_text(fb, UI_MARGIN, 264, UI_PX_CAPTION, s_view == BULK && s_batch_message[0] ? s_batch_message : s_message[0] ? s_message : s_view == BULK ? shelf_hint : s_shelf_warning, EPD_DRAW_ALIGN_LEFT, false);
            if (s_view != BULK) ui_text(fb, UI_MARGIN, UI_CONTENT_BOTTOM - UI_PX_CAPTION, UI_PX_CAPTION,
                    s_save_failed ? "进度未保存：点此重试" : "点击打开 · 长按查看详情和管理", EPD_DRAW_ALIGN_LEFT, false);
        }
        if (s_message[0] && s_view == TOC) ui_text(fb, UI_MARGIN, UI_CONTENT_TOP, UI_PX_CAPTION, s_message, EPD_DRAW_ALIGN_LEFT, false);
        if (s_view != TOC && !s_visible_count && s_count)
            ui_text(fb, UI_MARGIN, 308, UI_PX_CAPTION, "当前筛选没有图书", EPD_DRAW_ALIGN_LEFT, false);
        if (!s_message[0] || (s_view != TOC && s_visible_count)) {
            int rows = s_view == TOC ? BOOK_TOC_ROWS : BOOK_ROWS;
            int count = s_view == TOC ? (int)book_chapter_count() : s_visible_count;
            for (int row = 0; row < rows; ++row) {
                int i = ctx->leaf * rows + row;
                if (i >= count) break;
                EpdRect r = row_rect(row, s_view == TOC);
                char name[128], mark[32] = "";
                if (s_view == TOC) {
                    if (book_chapter_title(i, name, sizeof(name)) != ESP_OK) snprintf(name, sizeof(name), "第 %d 节", i + 1);
                } else {
                    copy_text(name, sizeof(name), s_shelf[i].name);
                    if (s_shelf[i].has_progress) snprintf(mark, sizeof(mark), "%s %u%%", s_shelf[i].is_flash ? "内置" : "", s_shelf[i].pct);
                    else if (s_shelf[i].is_flash) copy_text(mark, sizeof(mark), "内置");
                }
                if (s_view == BULK) snprintf(mark, sizeof(mark), "%s", s_shelf[i].selected ? "已选" : "未选");
                if (s_view != TOC && s_shelf[i].removed) snprintf(mark, sizeof(mark), "%s待清理", s_view == BULK && s_shelf[i].selected ? "已选·" : "已删·");
                fit_text(name, UI_PX_BTN, r.width - 2 * UI_PAD - ttf_text_width_px(UI_PX_CAPTION, mark) - UI_GAP);
                if (s_pressed_control == row) ui_draw_pressed_round_rect(fb, r, UI_BTN_RADIUS);
                ui_draw_round_rect(fb, r, UI_BTN_RADIUS, UI_GRAY_BLACK);
                if (s_view == TOC && i == (int)s_chapter) ui_draw_selected_round_rect(fb, r, UI_BTN_RADIUS);
                ui_text_vc(fb, r.x + UI_PAD, r.y + r.height / 2, UI_PX_BTN, name, EPD_DRAW_ALIGN_LEFT, false);
                ui_text_vc(fb, r.x + r.width - UI_PAD, r.y + r.height / 2, UI_PX_CAPTION, mark, EPD_DRAW_ALIGN_RIGHT, false);
            }
        }
        draw_control(fb, ui_bar_rect(0, 3), "上一页", 100);
        draw_control(fb, ui_bar_rect(1, 3), s_view == TOC ? "返回阅读" : s_view == BULK ? "返回书架" : "管理", 101);
        draw_control(fb, ui_bar_rect(2, 3), "下一页", 102);
        if (s_view == BULK) {
            const char* labels[] = {"本页全选", "清除勾选", "重新扫描", "删除所选", "清进度"};
            for (int i = 0; i < 5; ++i) draw_control(fb, batch_rect(i), labels[i], 610 + i);
            draw_batch_confirmation(fb);
        }
        ui_draw_menu_handle(fb, false);
        if (s_view == MANAGE) draw_manage(fb);
        if (s_resume_path[0]) {
            EpdRect panel = {UI_MARGIN - 12, 380, ui_content_width() + 24, 350};
            ui_fill_round_rect(fb, panel, UI_BTN_RADIUS, UI_GRAY_WHITE);
            ui_draw_round_rect(fb, panel, UI_BTN_RADIUS, UI_GRAY_BLACK);
            ui_text(fb, UI_MARGIN, 410, UI_PX_BODY, "继续上次阅读？", EPD_DRAW_ALIGN_LEFT, false);
            char name[256];
            copy_text(name, sizeof(name), s_resume_name);
            fit_text(name, UI_PX_CAPTION, ui_content_width());
            ui_text(fb, UI_MARGIN, 478, UI_PX_CAPTION, name, EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, UI_MARGIN, 530, UI_PX_CAPTION, "确认后才加载图书", EPD_DRAW_ALIGN_LEFT, false);
            draw_control(fb, ui_row_rect(0, 2, 620, UI_BTN_H), "留在书架", 700);
            draw_control(fb, ui_row_rect(1, 2, 620, UI_BTN_H), "继续阅读", 701);
        }
    }
    unlock_draw();
}
static bool present(app_ctx_t* ctx, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_NONE || redraw == APP_REDRAW_DONE) return true;
    int64_t start = esp_timer_get_time();
    if (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL) render(ctx, ctx->fb);
    bool prep = kick_prep();
    int64_t drawn = esp_timer_get_time();
    enum EpdDrawError err;
    if (s_view == READING && s_text && !s_image_open) reader_rotation_begin();
    else reader_rotation_end();
    if (s_page_turn) {
        if (s_page_turn_mode == MODE_GC16) err = update_display_full(ctx->hl);
        else err = update_display_with(ctx->hl, &E0470_WAVEFORM, s_page_turn_mode);
        s_page_turn = false;
    }
    else if (redraw == APP_REDRAW_FULL || s_full) {
        s_pages_since_clean = 0;
        err = update_display_full(ctx->hl);
    }
    else if (redraw == APP_REDRAW_AREA) {
        err = update_display_area_with(ctx->hl, &E0470_WAVEFORM, s_mode, s_area);
        if (s_reader_split) {
            // 页码即时更新；百分比不变时轨道像素不变，DU不会擦掉整条轨道。
            // Page numbers stay current; unchanged percentages leave the track unchanged, so DU never wipes the whole track.
            err = (enum EpdDrawError)(err | update_display_area_with(ctx->hl, &E0470_FOLLOW_WAVEFORM,
                                                                     MODE_DU, progress_rect()));
            ESP_LOGI(TAG, "reader regions body_h=%d footer=diff pct=%u", s_area.height, percent(s_page));
        }
    }
    else err = update_display_mode(ctx->hl, APP_PAGE_REFRESH_MODE);
    reader_rotation_end();
    int64_t displayed = esp_timer_get_time();
    if (prep) xSemaphoreTake(s_prep_done, portMAX_DELAY);
    if (redraw == APP_REDRAW_AREA && s_mode == MODE_DU && !s_full) {
        s_du_area = s_du_count ? ui_rect_union(s_du_area, s_area) : s_area;
        ++s_du_count;
        s_du_ms = esp_timer_get_time() / 1000;
    } else s_du_count = 0;
    ESP_LOGI(TAG, "present draw=%lld display=%lld join=%lld ms", (drawn - start) / 1000,
             (displayed - drawn) / 1000, (esp_timer_get_time() - displayed) / 1000);
    guard_draw_result(ctx->hl, err);
    s_full = false;
    s_reader_split = false;
    s_mode = MODE_GL16;
    return true;
}
static app_redraw_t paint_reading(app_ctx_t* ctx, enum EpdDrawMode mode) {
    int64_t started = esp_timer_get_time();
    lock_draw();
    bool cached = !s_toolbar && !s_clear_confirm && s_next_fb && s_next_page == (int)s_page;
    if (cached)
        memcpy(ctx->fb, s_next_fb, fb_bytes());
    else draw_reader(ctx->fb, s_page);
    unlock_draw();
    ESP_LOGI(TAG, "paint cached=%d ms=%lld", cached, (esp_timer_get_time() - started) / 1000);
    s_area = reader_area();
    s_reader_split = true;
    s_mode = mode;
    return APP_REDRAW_AREA;
}
static void loading_detail(app_ctx_t* ctx, const char* text, const char* detail) {
    lock_draw();
    EpdRect r = {UI_MARGIN, UI_BAR_TOP + 8, ui_bar_rect(0, 1).width, 80};
    ui_clear_rect_fast(ctx->fb, r);
    ui_text(ctx->fb, r.x, r.y, UI_PX_CAPTION, text, EPD_DRAW_ALIGN_LEFT, false);
    if (detail) ui_text(ctx->fb, r.x, r.y + 36, UI_PX_CAPTION, detail, EPD_DRAW_ALIGN_LEFT, false);
    unlock_draw();
    guard_draw_result(ctx->hl, update_display_area_with(ctx->hl, &E0470_WAVEFORM, MODE_DU, r));
}
static void loading(app_ctx_t* ctx, const char* text) { loading_detail(ctx, text, NULL); }
static app_redraw_t open_image(app_ctx_t* ctx, size_t block_index) {
    if (block_index >= s_block_count || !s_blocks[block_index].image_src) return APP_REDRAW_NONE;
    blk_t* block = &s_blocks[block_index];
    if (s_image_block != block_index || !s_image_pixels) {
        free_image();
        invalidate_prep();
        loading_detail(ctx, "正在加载图片…", "加载后可返回原阅读位置");
        lock_draw();
        free(s_next_fb); s_next_fb = NULL;
        esp_err_t err = book_chapter_load_image(s_chapter, block->image_src, &s_image_pixels, &s_image_width, &s_image_height);
        unlock_draw();
        ensure_prep();
        s_image_block = block_index;
        if (err != ESP_OK) {
            s_image_error = err == ESP_ERR_NO_MEM ? "内存不足，请返回后重试" :
                            err == ESP_ERR_NOT_FOUND ? "图片文件缺失" :
                            err == ESP_ERR_INVALID_SIZE ? "图片超出大小限制或文件损坏" : "图片损坏或格式暂不支持";
        }
        if (block->image_repeated && !block->image_title) {
            char title[160] = {0};
            (void)book_chapter_title(block->image_first_chapter, title, sizeof(title));
            snprintf(s_image_origin, sizeof(s_image_origin), "已读最早：第 %u 节 · %s", (unsigned)block->image_first_chapter + 1, title);
        }
    }
    s_image_open = true;
    sensor_set(ctx, false);
    s_size_settle_ms = 0;
    invalidate_prep();
    return APP_REDRAW_PAGE;
}

static const char* book_error_message(esp_err_t err) {
    if (err == ESP_ERR_NO_MEM) return "解析失败：内存不足，请稍后重试";
    if (err == ESP_ERR_INVALID_SIZE) return "超出上限：32768 项或资源过大";
    if (err == ESP_ERR_NOT_SUPPORTED) return "文件格式、压缩或加密暂不支持";
    if (err == ESP_ERR_NOT_FOUND) return "书籍或章节缺失，请检查文件和卡";
    return "解析失败：文件异常，请重新传入";
}

/* ---- 文件与进度 / Files and progress ---- */
static void free_book(void) {
    pending_progress_t* pending = pending_find(s_path);
    if (pending && !pending->dirty) pending_discard(s_path);
    book_notes_free(&s_notes);
    s_notes_error = false;
    invalidate_prep();
    free_image();
    book_layout_free();
    free(s_text);
    html_blocks_free(s_blocks, s_block_count);
    s_blocks = NULL;
    s_block_count = 0;
    s_text = NULL;
    s_text_len = 0;
    book_close();
    s_path[0] = 0;
}
static bool load_chapter(app_ctx_t* ctx, size_t chapter, size_t offset, bool last_page) {
    loading_detail(ctx, "正在加载和排版…", "长章节需要更多时间，请稍候");
    html_text_t loaded = {0};
    esp_err_t err = book_chapter_load_blocks(chapter, &loaded);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "chapter load failed: %s", esp_err_to_name(err));
        copy_text(s_message, sizeof(s_message), book_error_message(err));
        return false;
    }
    lock_draw();
    invalidate_prep();
    if (ttf_font_has_variable_weight()) {
        uint16_t weight = app_settings_book_weight();
        ttf_font_set_weight(weight);
    }
    bool ok = book_layout_begin_blocks_metrics(loaded.utf8, loaded.len, loaded.blocks, loaded.count,
                                               body_rect(), s_px, line_height_percent());
    // 先完成续读页；其余页由 tick 分批排版，上一章末页仍须定位到末尾。
    // Finish the resume page first; ticks paginate the rest, while previous-chapter navigation needs its end.
    while (ok && !book_layout_complete() &&
           (last_page || book_layout_page_start_offset(book_layout_page_count()) <= offset))
        ok = book_layout_extend(2);
    if (!ok) {
        html_text_free(&loaded);
        bool restored = s_text && book_layout_build_blocks_metrics(s_text, s_text_len, s_blocks, s_block_count,
                                                                   body_rect(), s_px, line_height_percent());
        unlock_draw();
        if (!restored) {
            free_book();
            s_view = SHELF;
            ctx->leaf = 0;
        }
        copy_text(s_message, sizeof(s_message), "排版失败：内存不足或章节过长");
        return false;
    }
    free(s_text);
    html_blocks_free(s_blocks, s_block_count);
    free_image();
    s_text = loaded.utf8;
    s_text_len = loaded.len;
    s_blocks = loaded.blocks;
    s_block_count = loaded.count;
    s_chapter = chapter;
    s_page = last_page ? book_layout_page_count() - 1 : book_layout_page_for_offset(offset);
    if (book_chapter_title(chapter, s_title, sizeof(s_title)) != ESP_OK)
        snprintf(s_title, sizeof(s_title), "第 %u 节", (unsigned)chapter + 1);
    copy_text(s_font_path, sizeof(s_font_path), ttf_font_path());
    s_message[0] = 0;
    unlock_draw();
    return true;
}
static bool open_book(app_ctx_t* ctx, const char* path) {
    if (!strncmp(path, "/sdcard/", 8)) {
        read_pico_sd_info_t sd = {0};
        read_pico_sd_get_info(&sd);
        if (!sd.present || !sd.mounted) {
            copy_text(s_message, sizeof(s_message), "TF 卡不可用，请重新挂载后打开");
            return false;
        }
    }
    // 自动续读也不能绕过同路径旧文件的清理重试。
    // Automatic resume must also finish cleanup of the old file at this path.
    if (delete_retry_find(path)) {
        copy_text(s_message, sizeof(s_message), "文件已删除，进度清理失败，请重试");
        return false;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) {
        copy_text(s_message, sizeof(s_message), "无法读取文件，请检查存储卡");
        return false;
    }
    if (strncmp(path, "/flash/", 7) == 0 && st.st_size > BOOK_STORE_FLASH_FILE_MAX) {
        copy_text(s_message, sizeof(s_message), "内置存储单本限 1 MB");
        return false;
    }
    if (!pending_reserve(path)) {
        copy_text(s_message, sizeof(s_message), "内存不足，无法保留待保存进度");
        return false;
    }
    save_progress();
    free_book();
    if (!pending_reserve(path)) {
        copy_text(s_message, sizeof(s_message), "内存不足，无法打开图书");
        return false;
    }
    loading_detail(ctx, "正在解析图书…", "大书需要更多时间，请稍候");
    // 持绘制锁释放下一页缓存，降低解析峰值。/ Release the next-page buffer under the drawing lock to reduce parsing peaks.
    lock_draw(); free(s_next_fb); s_next_fb = NULL; unlock_draw();
    int64_t open_started = esp_timer_get_time();
    esp_err_t err = book_open(path);
    int64_t source_ms = (esp_timer_get_time() - open_started) / 1000;
    ensure_prep();
    if (err != ESP_OK) {
        pending_progress_t* pending = pending_find(path);
        if (pending && !pending->dirty) pending_discard(path);
        copy_text(s_message, sizeof(s_message), book_error_message(err));
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        return false;
    }
    copy_text(s_path, sizeof(s_path), path);
    const char* book_name = strrchr(path, '/');
    copy_text(s_book_name, sizeof(s_book_name), book_name ? book_name + 1 : path);
    s_file_size = (uint32_t)st.st_size;
    load_notes_for_book();
    book_progress_t p = {0};
    bool resume = pending_restore(path, s_file_size, &p) || book_progress_load(path, s_file_size, &p);
    s_px = resume ? p.px : app_settings_book_px();
    if (s_px < BOOK_PX_MIN || s_px > BOOK_PX_MAX) s_px = 48;
    size_t chapter = resume && p.chapter < book_chapter_count() ? p.chapter : 0;
    if (!load_chapter(ctx, chapter, resume && p.chapter == chapter ? p.byte_off : 0, false)) {
        free_book();
        return false;
    }
    s_view = READING;
    s_toolbar = s_clear_confirm = s_batch_confirm = false;
    s_unsaved = 0;
    sensor_set(ctx, reader_sensor_required());
    s_size_settle_ms = 0;
    pending_mark_latest(s_path);
    save_progress();
    ESP_LOGI(TAG, "opened kind=%d chapters=%u pages=%u px=%d source=%lld chapter=%lld ms", book_kind(), (unsigned)book_chapter_count(), (unsigned)book_layout_page_count(), s_px,
             (long long)source_ms, (long long)((esp_timer_get_time() - open_started) / 1000 - source_ms));
    return true;
}
static app_redraw_t resume_choice(app_ctx_t* ctx, bool confirmed) {
    if (!s_resume_path[0]) return APP_REDRAW_NONE;
    char path[BOOK_STORE_PATH_MAX];
    copy_text(path, sizeof(path), s_resume_path);
    s_resume_path[0] = 0;
    if (confirmed) open_book(ctx, path);
    return APP_REDRAW_PAGE;
}
/* ---- 书架数据 / Shelf data ---- */
static bool shelf_matches(const shelf_entry_t* item) {
    return item->search_match && (s_filter == 0 || (s_filter == 1 && !item->is_flash) || (s_filter == 2 && item->is_flash));
}
static int compare_books(const void* a, const void* b) {
    const shelf_entry_t* x = a;
    const shelf_entry_t* y = b;
    if (shelf_matches(x) != shelf_matches(y)) return shelf_matches(x) ? -1 : 1;
    if (s_recent_sort && x->recent != y->recent) return x->recent > y->recent ? -1 : 1;
    int name = strcasecmp(x->name, y->name);
    return name ? name : strcmp(x->path, y->path);
}
static void sort_shelf(app_ctx_t* ctx) {
    if (s_count > 1) qsort(s_shelf, s_count, sizeof(*s_shelf), compare_books);
    s_visible_count = 0;
    while (s_visible_count < s_count && shelf_matches(&s_shelf[s_visible_count])) ++s_visible_count;
    ctx->leaf = 0;
}
static bool shelf_reserve(void) {
    if ((size_t)s_count < s_shelf_capacity) return true;
    if (s_count == INT_MAX) return false;
    size_t cap = s_shelf_capacity ? s_shelf_capacity * 2 : 32;
    if (cap > INT_MAX || cap > SIZE_MAX / sizeof(*s_shelf)) return false;
    shelf_entry_t* entries = heap_caps_realloc(s_shelf, cap * sizeof(*entries), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!entries) return false;
    s_shelf = entries;
    s_shelf_capacity = cap;
    return true;
}

// 在删除文件前预留记录，避免删除成功后才遇到内存不足。
// Reserve before unlink so allocation failure never loses a completed deletion's retry.
static delete_retry_t* delete_retry_reserve(const shelf_entry_t* entry) {
    delete_retry_t* existing = delete_retry_find(entry->path);
    if (existing) return existing;
    delete_retry_t* p = heap_caps_malloc(sizeof(*p), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(sizeof(*p));
    if (!p) return NULL;
    p->entry = *entry;
    p->next = s_delete_retries;
    s_delete_retries = p;
    return p;
}
static void delete_retry_discard(const char* path) {
    delete_retry_t** p = &s_delete_retries;
    while (*p) {
        if (!strcmp((*p)->entry.path, path)) {
            delete_retry_t* old = *p;
            *p = old->next;
            free(old);
            return;
        }
        p = &(*p)->next;
    }
}
static void scan_shelf(app_ctx_t* ctx) {
    s_count = 0;
    s_visible_count = 0;
    s_message[0] = 0;
    s_shelf_warning[0] = 0;
    if (!book_store_flash_ready()) loading(ctx, "初始化内置存储…");
    book_store_root_t roots[2];
    int n = 0;
    esp_err_t root_err = book_store_roots(roots, &n);
    bool truncated = false, unreadable = root_err != ESP_OK || book_store_roots_degraded(), skipped = false;
    s_storage[0] = 0;
    for (int i = 0; i < n; ++i) {
        char capacity[64];
        snprintf(capacity, sizeof(capacity), "%s%s %.1f MB", i ? " · " : "", roots[i].is_flash ? "内置余" : "TF余", book_store_free_bytes(&roots[i]) / 1048576.0);
        strncat(s_storage, capacity, sizeof(s_storage) - strlen(s_storage) - 1);
        DIR* dir = opendir(roots[i].path);
        if (!dir) { unreadable = true; continue; }
        struct dirent* ent;
        for (;;) {
            errno = 0;
            ent = readdir(dir);
            if (!ent) { if (errno) unreadable = true; break; }
            const char* ext = strrchr(ent->d_name, '.');
            if (!ext || (strcasecmp(ext, ".txt") && strcasecmp(ext, ".epub") && strcasecmp(ext, ".md") && strcasecmp(ext, ".cbz"))) continue;
            shelf_entry_t candidate = {0};
            shelf_entry_t* item = &candidate;
            int len = snprintf(item->path, sizeof(item->path), "%s/%s", roots[i].path, ent->d_name);
            if (len < 0 || (size_t)len >= sizeof(item->path) || strlen(ent->d_name) >= sizeof(item->name)) { skipped = true; continue; }
            struct stat st;
            if (stat(item->path, &st) != 0) { unreadable = true; continue; }
            if (!S_ISREG(st.st_mode)) continue;
            if (st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX) { skipped = true; continue; }
            if (!shelf_reserve()) { truncated = true; break; }
            copy_text(item->name, sizeof(item->name), ent->d_name);
            item->search_match = read_pico_search_match(item->name, s_query);
            item->size = st.st_size;
            item->is_flash = roots[i].is_flash;
            book_progress_t p;
            item->has_progress = book_progress_load(item->path, item->size, &p);
            item->pct = item->has_progress ? p.pct : 0;
            item->recent = item->has_progress ? p.last_open_s : 0;
            s_shelf[s_count++] = candidate;
        }
        closedir(dir);
    }
    // 同路径重新出现也先完成旧清理，避免新阅读进度被后续重试擦掉。
    // Finish old cleanup even if the path reappears, before new reading can create progress.
    for (delete_retry_t* p = s_delete_retries; p; p = p->next) {
        int i = 0;
        while (i < s_count && strcmp(s_shelf[i].path, p->entry.path)) ++i;
        if (i == s_count) {
            if (!shelf_reserve()) { truncated = true; continue; }
            ++s_count;
        }
        s_shelf[i] = p->entry;
        s_shelf[i].selected = false;
        s_shelf[i].search_match = read_pico_search_match(p->entry.name, s_query);
    }
    sort_shelf(ctx);
    if (!n) copy_text(s_storage, sizeof(s_storage), "存储不可用，请检查 TF 卡");
    if (truncated) snprintf(s_shelf_warning, sizeof(s_shelf_warning), "内存不足，仅列 %d 本；释放后重扫", s_count);
    else if (unreadable) copy_text(s_shelf_warning, sizeof(s_shelf_warning), "部分目录或文件不可读，请检查后重扫");
    else if (skipped) copy_text(s_shelf_warning, sizeof(s_shelf_warning), "部分文件过大或名称过长，未列入");
    else if (s_pending_invalidated) copy_text(s_shelf_warning, sizeof(s_shelf_warning), "图书已更新，旧待保存进度已作废");
    if (!s_count && !unreadable && !truncated) copy_text(s_message, sizeof(s_message), "暂无图书，请传入 TXT 或 EPUB");
    ESP_LOGI(TAG, "shelf books=%d roots=%d", s_count, n);
}

static void refresh_capacity(void) {
    book_store_root_t roots[2];
    int count = 0;
    s_storage[0] = 0;
    book_store_roots(roots, &count);
    for (int i = 0; i < count; ++i) {
        char line[64];
        snprintf(line, sizeof(line), "%s%s %.1f MB", i ? " · " : "", roots[i].is_flash ? "内置余" : "TF余", book_store_free_bytes(&roots[i]) / 1048576.0);
        strncat(s_storage, line, sizeof(s_storage) - strlen(s_storage) - 1);
    }
}
static void manage_apply(app_ctx_t* ctx) {
    if (!strcmp(s_path, s_managed.path)) free_book();
    esp_err_t err;
    if (s_delete_confirm && !s_file_removed) {
        delete_retry_t* retry = delete_retry_reserve(&s_managed);
        if (!retry) {
            copy_text(s_manage_message, sizeof(s_manage_message), "内存不足，无法保留删除重试记录");
            s_clear_confirm = false;
            return;
        }
        bool removed = false;
        err = book_store_delete(s_managed.path, &removed);
        if (removed) {
            retry->entry.removed = true;
            s_file_removed = true;
            pending_discard(s_managed.path);
            book_store_notify_changed();
            s_store_revision = book_store_revision();
            for (int i = 0; i < s_count; ++i) if (!strcmp(s_shelf[i].path, s_managed.path)) s_shelf[i].removed = true;
            refresh_capacity();
        } else delete_retry_discard(s_managed.path);
    } else {
        pending_discard(s_managed.path);
        err = book_progress_forget(s_managed.path);
    }
    s_clear_confirm = false;
    if (err != ESP_OK) {
        copy_text(s_manage_message, sizeof(s_manage_message), s_file_removed ?
                  "文件已删除，进度清理失败，请重试" : s_delete_confirm ?
                  "文件删除失败，请检查存储后重试" : "进度清理失败，请重新尝试");
        return;
    }
    delete_retry_discard(s_managed.path);
    s_view = SHELF;
    int write = 0, leaf = ctx->leaf;
    for (int i = 0; i < s_count; ++i) {
        if (!strcmp(s_shelf[i].path, s_managed.path)) {
            if (s_file_removed) continue;
            s_shelf[i].has_progress = false; s_shelf[i].pct = 0; s_shelf[i].recent = 0;
        }
        s_shelf[write++] = s_shelf[i];
    }
    s_count = write;
    sort_shelf(ctx);
    ctx->leaf = leaf < leaves() ? leaf : leaves() - 1;
}

/* ---- 输入与生命周期 / Input and lifecycle ---- */
static void sensor_set(app_ctx_t* ctx, bool on) {
    memset(&s_shake, 0, sizeof(s_shake));
    if (!ctx->sensor_ready) return;
    if (on) {
        if (!s_sensor_saved) {
            s_sensor_config = *sc7a20h_get_config(ctx->acc);
            s_sensor_saved = true;
        }
        sc7a20h_sensor_config_t config = s_sensor_config;
        uint8_t gesture = app_settings_book_gesture();
        bool auto_rotation = app_settings_book_rotation() == 2;
        config.odr = gesture == 2 ? SC7A20H_ODR_200 : SC7A20H_ODR_100;
        config.fs = SC7A20H_FS_2G;
        s_sensor_on = sc7a20h_apply_config(ctx->acc, &config) == ESP_OK;
        sc7a20h_aoi_cfg_t off = {0};
        (void)sc7a20h_aoi_config(ctx->acc, SC7A20H_AOI1, &off);
        (void)sc7a20h_aoi_config(ctx->acc, SC7A20H_AOI2, &off);
        (void)sc7a20h_click_config(ctx->acc, 0, SC7A20H_CLICK_NORMAL);
        if (s_sensor_on && gesture == 1)
            s_sensor_on = sc7a20h_activity_config(ctx->acc, BOOK_SHAKE_THS_MG, BOOK_SHAKE_DUR) == ESP_OK;
        if (s_sensor_on && gesture == 2)
            s_sensor_on = sc7a20h_click_config(ctx->acc, SC7A20H_AXIS_X | SC7A20H_AXIS_Y | SC7A20H_AXIS_Z,
                                                SC7A20H_CLICK_NORMAL) == ESP_OK;
        if (auto_rotation) {
            s_auto_candidate_since = 0;
            s_auto_candidate_landscape = s_reader_landscape;
            s_auto_sample_ms = 0;
        }
        if (!s_sensor_on) {
            sc7a20h_aoi_cfg_t off = {0};
            sc7a20h_aoi_config(ctx->acc, SC7A20H_AOI2, &off);
            sc7a20h_apply_config(ctx->acc, &s_sensor_config);
            s_sensor_saved = false;
            read_pico_sensor_sleep(ctx->acc);
        }
    } else {
        sc7a20h_aoi_cfg_t off = {0};
        (void)sc7a20h_click_config(ctx->acc, 0, SC7A20H_CLICK_NORMAL);
        (void)sc7a20h_aoi_config(ctx->acc, SC7A20H_AOI1, &off);
        sc7a20h_aoi_config(ctx->acc, SC7A20H_AOI2, &off);
        if (s_sensor_saved) sc7a20h_apply_config(ctx->acc, &s_sensor_config);
        s_sensor_saved = false;
        read_pico_sensor_sleep(ctx->acc);
        s_sensor_on = false;
        s_auto_candidate_since = 0;
    }
}
static bool reader_sensor_required(void) {
    return app_settings_book_gesture() != 0 || app_settings_book_rotation() == 2;
}
static app_redraw_t turn_page(app_ctx_t* ctx, int dir) {
    if (!s_text || s_clear_confirm) return APP_REDRAW_NONE;
    if (dir > 0 && s_page + 1 >= book_layout_page_count() && !book_layout_complete()) {
        lock_draw();
        invalidate_prep();
        bool ok = book_layout_extend(2);
        unlock_draw();
        if (!ok) {
            free_book(); s_view = SHELF; ctx->leaf = 0;
            copy_text(s_message, sizeof(s_message), "排版失败：内存不足或章节过长");
            return APP_REDRAW_PAGE;
        }
    }
    size_t old_chapter = s_chapter;
    bool changed = true;
    if (dir > 0 && s_page + 1 < book_layout_page_count()) ++s_page;
    else if (dir < 0 && s_page) --s_page;
    else if (dir > 0 && s_chapter + 1 < book_chapter_count()) { save_progress(); changed = load_chapter(ctx, s_chapter + 1, 0, false); }
    else if (dir < 0 && s_chapter) { save_progress(); changed = load_chapter(ctx, s_chapter - 1, 0, true); }
    else return APP_REDRAW_NONE;
    if (!changed) { s_view = s_text ? TOC : SHELF; ctx->leaf = s_text ? s_chapter / BOOK_TOC_ROWS : 0; return APP_REDRAW_PAGE; }
    s_toolbar = false;
    s_last_turn_ms = ctx->now_ms;
    bool chapter_changed = old_chapter != s_chapter;
    if (s_unsaved < 8) ++s_unsaved;
    if (s_unsaved >= 8 && !s_save_failed) save_progress();
    ESP_LOGI(TAG, "turn chapter=%u page=%u/%u pct=%u", (unsigned)s_chapter, (unsigned)s_page + 1, (unsigned)book_layout_page_count(), percent(s_page));
    uint8_t profile = app_settings_book_refresh();
    unsigned cadence = profile == 2 ? 6 : 8;
    if (profile == 0 || chapter_changed || s_pages_since_clean + 1 >= cadence) {
        s_pages_since_clean = 0;
        s_page_turn_mode = MODE_GC16;
    } else {
        ++s_pages_since_clean;
        s_page_turn_mode = profile == 2 ? MODE_DU : MODE_GL16;
    }
    s_page_turn = true;
    app_redraw_t redraw = paint_reading(ctx, s_page_turn_mode);
    return redraw;
}
static app_redraw_t resize_text(app_ctx_t* ctx, int dir) {
    int next = s_px + dir * BOOK_PX_STEP;
    if (next < BOOK_PX_MIN || next > BOOK_PX_MAX) return APP_REDRAW_NONE;
    size_t off = book_layout_page_start_offset(s_page);
    save_progress();
    lock_draw();
    invalidate_prep();
    int old = s_px;
    if (!book_layout_build_blocks_metrics(s_text, s_text_len, s_blocks, s_block_count, body_rect(), next,
                                          line_height_percent())) {
        bool restored = book_layout_build_blocks_metrics(s_text, s_text_len, s_blocks, s_block_count,
                                                         body_rect(), old, line_height_percent());
        unlock_draw();
        if (!restored) {
            free_book();
            s_view = SHELF;
            copy_text(s_message, sizeof(s_message), "排版失败，请重新扫描并打开图书");
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    s_px = next;
    s_page = book_layout_page_for_offset(off);
    unlock_draw();
    app_settings_set_book_px(s_px);
    save_progress();
    s_size_settle_ms = ctx->now_ms + BOOK_SIZE_SETTLE_MS;
    return paint_reading(ctx, MODE_DU);
}
static void goto_font(app_ctx_t* ctx) {
    save_progress();
    ctx->request_app = app_font_page();
}
static app_redraw_t manage_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    int count = s_clear_confirm || s_file_removed ? 2 : 3;
    for (int i = 0; i < count; ++i) if (ui_rect_hit(manage_rect(i, count), x, y)) {
        if (s_clear_confirm) { if (!i) s_clear_confirm = false; else manage_apply(ctx); }
        else if (!i) s_view = SHELF;
        else if (s_file_removed) manage_apply(ctx);
        else { s_clear_confirm = true; s_delete_confirm = i == 2; s_manage_message[0] = 0; }
        break;
    }
    return APP_REDRAW_PAGE;
}
// 成功项退出选择，已删文件的失败项仅重试元数据。/ Deselect successes; removed-file failures retry metadata only.
static void batch_apply(app_ctx_t* ctx) {
    unsigned done = 0, failed = 0;
    int write = 0;
    for (int i = 0; i < s_count; ++i) {
        shelf_entry_t item = s_shelf[i];
        bool discard = false;
        if (item.selected) {
            if (!strcmp(s_path, item.path)) free_book();
            esp_err_t err;
            if (s_batch_delete && !item.removed) {
                delete_retry_t* retry = delete_retry_reserve(&item);
                if (!retry) { ++failed; s_shelf[write++] = item; continue; }
                bool removed = false;
                err = book_store_delete(item.path, &removed);
                if (removed) {
                    retry->entry.removed = true;
                    item.removed = true; pending_discard(item.path);
                    book_store_notify_changed(); s_store_revision = book_store_revision();
                } else delete_retry_discard(item.path);
            } else { pending_discard(item.path); err = book_progress_forget(item.path); }
            if (err == ESP_OK) {
                delete_retry_discard(item.path);
                ++done; item.selected = false; item.has_progress = false; item.pct = 0; item.recent = 0;
                discard = item.removed;
            } else ++failed;
        }
        if (!discard) s_shelf[write++] = item;
    }
    s_count = write;
    refresh_capacity();
    int leaf = ctx->leaf;
    sort_shelf(ctx);
    ctx->leaf = leaf < leaves() ? leaf : leaves() - 1;
    s_batch_confirm = false;
    snprintf(s_batch_message, sizeof(s_batch_message), "成功 %u 本，失败 %u 本%s", done, failed, failed ? "；所选可重试" : "");
}
static app_redraw_t batch_action(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_batch_confirm) {
        if (ui_rect_hit(ui_row_rect(0, 2, 620, UI_BTN_H), x, y)) s_batch_confirm = false;
        else if (ui_rect_hit(ui_row_rect(1, 2, 620, UI_BTN_H), x, y)) batch_apply(ctx);
        return APP_REDRAW_PAGE;
    }
    for (int i = 0; i < 5; ++i) if (ui_rect_hit(batch_rect(i), x, y)) {
        if (!i) select_page(ctx->leaf);
        else if (i == 1) clear_selection();
        else if (i == 2) { clear_selection(); s_batch_message[0] = 0; scan_shelf(ctx); }
        else if (selected_count()) { s_batch_delete = i == 3; s_batch_confirm = true; }
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t seek_progress(app_ctx_t* ctx, uint16_t x) {
    EpdRect track = progress_rect();
    size_t chapter_count = book_chapter_count();
    uint32_t total = book_total_bytes();
    if (track.width < 2 || chapter_count == 0 || total == 0) return APP_REDRAW_NONE;
    if (x < track.x) x = track.x;
    if (x >= track.x + track.width) x = (uint16_t)(track.x + track.width - 1);
    uint32_t target = (uint64_t)(x - track.x) * total / (track.width - 1);
    size_t lo = 0, hi = chapter_count;
    while (lo + 1 < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (book_chapter_byte_offset(mid) <= target) lo = mid;
        else hi = mid;
    }
    size_t chapter = lo;
    uint32_t chapter_start = book_chapter_byte_offset(chapter);
    uint32_t chapter_finish = chapter + 1 < chapter_count
        ? book_chapter_byte_offset(chapter + 1) : total;
    save_progress();
    if (!load_chapter(ctx, chapter, 0, false)) return APP_REDRAW_PAGE;
    size_t offset = chapter_finish > chapter_start
        ? (uint64_t)(target - chapter_start) * s_text_len / (chapter_finish - chapter_start) : 0;
    lock_draw();
    invalidate_prep();
    bool ok = true;
    while (ok && !book_layout_complete() &&
           book_layout_page_start_offset(book_layout_page_count()) <= offset)
        ok = book_layout_extend(2);
    if (ok) s_page = book_layout_page_for_offset(offset);
    unlock_draw();
    if (!ok) {
        free_book();
        s_view = SHELF;
        ctx->leaf = 0;
        copy_text(s_message, sizeof(s_message), "跳转排版失败：内存不足或章节过长");
        return APP_REDRAW_PAGE;
    }
    save_progress();
    s_toolbar = false;
    s_last_turn_ms = ctx->now_ms;
    s_full = true;
    if (s_unsaved < 8) ++s_unsaved;
    if (s_unsaved >= 8 && !s_save_failed) save_progress();
    return paint_reading(ctx, MODE_GL16);
}
static void open_note_list(app_ctx_t* ctx, book_view_t view) {
    save_progress();
    sensor_set(ctx, false);
    s_view = view;
    s_toolbar = false;
    s_note_delete_confirm = false;
    s_note_delete_index = 0;
    s_note_delete_index_valid = false;
    s_note_message[0] = 0;
    ctx->leaf = 0;
}
static book_note_position_t current_note_position(void) {
    return (book_note_position_t){
        .chapter = (uint16_t)s_chapter,
        .byte_off = (uint32_t)book_layout_page_start_offset(s_page),
        .percent = (uint8_t)percent(s_page),
    };
}
static bool save_notes_edit(void) {
    esp_err_t err = book_notes_save(s_path, s_file_size, &s_notes);
    if (err != ESP_OK) {
        snprintf(s_note_message, sizeof(s_note_message), "笔记保存失败：%s", esp_err_to_name(err));
        return false;
    }
    s_notes_error = false;
    return true;
}
static app_redraw_t export_notes(void) {
    char path[BOOK_STORE_PATH_MAX];
    esp_err_t err = book_notes_export_markdown(s_path, s_book_name, &s_notes, path, sizeof(path));
    if (err == ESP_OK) snprintf(s_note_message, sizeof(s_note_message), "已导出：%s", strrchr(path, '/') + 1);
    else snprintf(s_note_message, sizeof(s_note_message), "导出失败：%s", esp_err_to_name(err));
    return APP_REDRAW_PAGE;
}
static app_redraw_t add_bookmark(void) {
    book_note_position_t position = current_note_position();
    for (size_t i = 0; i < s_notes.bookmark_count; ++i) {
        book_note_position_t *item = &s_notes.bookmarks[i];
        if (item->chapter == position.chapter && item->byte_off == position.byte_off) {
            snprintf(s_note_message, sizeof(s_note_message), "此处已有书签");
            return APP_REDRAW_PAGE;
        }
    }
    if (s_notes.bookmark_count >= SIZE_MAX / sizeof(*s_notes.bookmarks)) {
        snprintf(s_note_message, sizeof(s_note_message), "书签数量已超出设备可处理范围");
        return APP_REDRAW_PAGE;
    }
    book_note_position_t *items = realloc(s_notes.bookmarks, (s_notes.bookmark_count + 1) * sizeof(*items));
    if (!items) { snprintf(s_note_message, sizeof(s_note_message), "内存不足，无法添加书签"); return APP_REDRAW_PAGE; }
    s_notes.bookmarks = items;
    memmove(&s_notes.bookmarks[1], &s_notes.bookmarks[0], s_notes.bookmark_count * sizeof(*items));
    s_notes.bookmarks[0] = position;
    ++s_notes.bookmark_count;
    if (!save_notes_edit()) {
        memmove(&s_notes.bookmarks[0], &s_notes.bookmarks[1], (s_notes.bookmark_count - 1) * sizeof(*items));
        --s_notes.bookmark_count;
        return APP_REDRAW_PAGE;
    }
    snprintf(s_note_message, sizeof(s_note_message), "已添加书签 · 全书 %u%%", position.percent);
    return APP_REDRAW_PAGE;
}
static app_redraw_t add_excerpt(void) {
    if (!s_text || !book_layout_page_count()) return APP_REDRAW_NONE;
    size_t start = book_layout_page_start_offset(s_page);
    size_t end = s_page + 1 < book_layout_page_count()
        ? book_layout_page_start_offset(s_page + 1) : s_text_len;
    if (start >= s_text_len || end <= start) {
        snprintf(s_note_message, sizeof(s_note_message), "当前页没有可保存的正文");
        return APP_REDRAW_PAGE;
    }
    if (end > s_text_len) end = s_text_len;
    size_t len = end - start;
    if (!len) { snprintf(s_note_message, sizeof(s_note_message), "当前页没有可保存的正文"); return APP_REDRAW_PAGE; }
    char *text = malloc(len + 1);
    if (!text) { snprintf(s_note_message, sizeof(s_note_message), "内存不足，无法保存摘录"); return APP_REDRAW_PAGE; }
    memcpy(text, s_text + start, len);
    text[len] = 0;
    book_note_position_t position = current_note_position();
    size_t existing = s_notes.excerpt_count;
    for (size_t i = 0; i < s_notes.excerpt_count; ++i) {
        if (s_notes.excerpts[i].position.chapter == position.chapter &&
            s_notes.excerpts[i].position.byte_off == position.byte_off) { existing = i; break; }
    }
    book_note_excerpt_t old = {0};
    size_t old_count = s_notes.excerpt_count;
    if (existing < s_notes.excerpt_count) {
        old = s_notes.excerpts[existing];
    } else {
        if (s_notes.excerpt_count >= SIZE_MAX / sizeof(*s_notes.excerpts)) {
            free(text);
            snprintf(s_note_message, sizeof(s_note_message), "摘录数量已超出设备可处理范围");
            return APP_REDRAW_PAGE;
        }
        book_note_excerpt_t *items = realloc(s_notes.excerpts, (s_notes.excerpt_count + 1) * sizeof(*items));
        if (!items) { free(text); snprintf(s_note_message, sizeof(s_note_message), "内存不足，无法保存摘录"); return APP_REDRAW_PAGE; }
        s_notes.excerpts = items;
        ++s_notes.excerpt_count;
    }
    memmove(&s_notes.excerpts[1], &s_notes.excerpts[0], existing * sizeof(*s_notes.excerpts));
    s_notes.excerpts[0] = (book_note_excerpt_t){.position = position, .text = text, .text_len = len};
    if (!save_notes_edit()) {
        memmove(&s_notes.excerpts[0], &s_notes.excerpts[1], existing * sizeof(*s_notes.excerpts));
        if (existing < old_count) s_notes.excerpts[existing] = old;
        else s_notes.excerpt_count = old_count;
        free(text);
        return APP_REDRAW_PAGE;
    }
    if (existing < old_count) free(old.text);
    reading_stats_save_excerpt(s_book_name, s_notes.excerpts[0].text);
    snprintf(s_note_message, sizeof(s_note_message), "已保存本页摘录");
    return APP_REDRAW_PAGE;
}
static app_redraw_t delete_note(app_ctx_t* ctx) {
    if (!s_note_delete_index_valid) return APP_REDRAW_NONE;
    size_t at = s_note_delete_index;
    if (s_view == BOOKMARKS) {
        if (at >= s_notes.bookmark_count) return APP_REDRAW_NONE;
        book_note_position_t removed = s_notes.bookmarks[at];
        memmove(&s_notes.bookmarks[at], &s_notes.bookmarks[at + 1],
                (s_notes.bookmark_count - at - 1) * sizeof(s_notes.bookmarks[0]));
        --s_notes.bookmark_count;
        if (!save_notes_edit()) {
            memmove(&s_notes.bookmarks[at + 1], &s_notes.bookmarks[at],
                    (s_notes.bookmark_count - at) * sizeof(s_notes.bookmarks[0]));
            s_notes.bookmarks[at] = removed;
            ++s_notes.bookmark_count;
            return APP_REDRAW_PAGE;
        }
    } else {
        if (at >= s_notes.excerpt_count) return APP_REDRAW_NONE;
        book_note_excerpt_t removed = s_notes.excerpts[at];
        memmove(&s_notes.excerpts[at], &s_notes.excerpts[at + 1],
                (s_notes.excerpt_count - at - 1) * sizeof(s_notes.excerpts[0]));
        --s_notes.excerpt_count;
        if (!save_notes_edit()) {
            memmove(&s_notes.excerpts[at + 1], &s_notes.excerpts[at],
                    (s_notes.excerpt_count - at) * sizeof(s_notes.excerpts[0]));
            s_notes.excerpts[at] = removed;
            ++s_notes.excerpt_count;
            return APP_REDRAW_PAGE;
        }
        free(removed.text);
    }
    s_note_delete_index_valid = false;
    snprintf(s_note_message, sizeof(s_note_message), "已删除");
    if (ctx->leaf >= leaves()) ctx->leaf = leaves() - 1;
    return APP_REDRAW_PAGE;
}
static app_redraw_t jump_to_note(app_ctx_t *ctx, const book_note_position_t *position) {
    if (!position || position->chapter >= book_chapter_count()) {
        snprintf(s_note_message, sizeof(s_note_message), "书签位置与当前目录不匹配");
        return APP_REDRAW_PAGE;
    }
    save_progress();
    if (!load_chapter(ctx, position->chapter, position->byte_off, false)) {
        snprintf(s_note_message, sizeof(s_note_message), "无法加载该位置，请检查图书文件");
        return APP_REDRAW_PAGE;
    }
    s_view = READING;
    s_toolbar = false;
    s_full = true;
    save_progress();
    return paint_reading(ctx, MODE_GL16);
}
static app_redraw_t action_at(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    if (s_resume_path[0]) {
        for (int i = 0; i < 2; ++i) if (ui_rect_hit(ui_row_rect(i, 2, 620, UI_BTN_H), x, y)) {
            return resume_choice(ctx, i != 0);
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == MANAGE) return manage_action(ctx, x, y);
    if (s_view == CHAPTER_RESULTS) {
        for (size_t i = 0; i < s_chapter_match_count && i < BOOK_SEARCH_RESULTS; ++i) if (ui_rect_hit(row_rect((int)i, false), x, y)) {
            size_t offset = s_chapter_matches[i];
            lock_draw();
            while (!book_layout_complete() && book_layout_page_start_offset(book_layout_page_count()) <= offset) {
                if (!book_layout_extend(2)) break;
            }
            s_page = book_layout_page_for_offset(offset);
            unlock_draw();
            s_view = READING; s_toolbar = false; sensor_set(ctx, reader_sensor_required()); s_full = true; save_progress();
            return APP_REDRAW_FULL;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == TYPOGRAPHY) {
        if (ui_rect_hit(ui_row_rect(0, 1, 688, 84), x, y)) {
            s_view = READING; s_full = true; return APP_REDRAW_FULL;
        }
        size_t offset = book_layout_page_start_offset(s_page);
        if (ui_rect_hit(ui_row_rect(0, 1, 340, 84), x, y))
            app_settings_set_book_line_height((app_settings_book_line_height() + 1) % 3);
        else if (ui_rect_hit(ui_row_rect(0, 1, 456, 84), x, y))
            app_settings_set_book_margin((app_settings_book_margin() + 1) % 3);
        else if (ui_rect_hit(ui_row_rect(0, 1, 572, 84), x, y) && ttf_font_has_variable_weight()) {
            uint16_t weight = app_settings_book_weight();
            weight = weight == 0 ? 300 : weight == 800 ? 0 : weight + 100;
            app_settings_set_book_weight(weight);
            ttf_font_set_weight(weight);
        } else return APP_REDRAW_NONE;
        lock_draw();
        invalidate_prep();
        bool ok = book_layout_build_blocks_metrics(s_text, s_text_len, s_blocks, s_block_count,
                                                   body_rect(), s_px, line_height_percent());
        if (ok) s_page = book_layout_page_for_offset(offset);
        unlock_draw();
        if (!ok) snprintf(s_note_message, sizeof(s_note_message), "排版失败，请检查可用内存");
        save_progress();
        s_view = READING; s_full = true;
        return APP_REDRAW_FULL;
    }
    if (s_view == SEARCH) {
        for (int i = 0; i < 45; ++i) if (ui_rect_hit(search_rect(i), x, y)) {
            app_redraw_t result = search_action(ctx, i);
            if (result == APP_REDRAW_AREA) { render(ctx, ctx->fb); s_area = (EpdRect){UI_MARGIN, 190, ui_content_width(), 850}; s_mode = MODE_DU; }
            return result;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == BULK) { app_redraw_t result = batch_action(ctx, x, y); if (result != APP_REDRAW_NONE) return result; }
    if (s_view == BOOKMARKS || s_view == EXCERPTS) {
        if (s_note_delete_confirm) {
            if (ui_rect_hit(ui_row_rect(0, 2, 630, UI_BTN_H), x, y)) {
                s_note_delete_confirm = false;
                s_note_delete_index_valid = false;
            }
            else if (ui_rect_hit(ui_row_rect(1, 2, 630, UI_BTN_H), x, y)) {
                s_note_delete_confirm = false;
                return delete_note(ctx);
            }
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(note_action_rect(0), x, y))
            return s_view == BOOKMARKS ? add_bookmark() : add_excerpt();
        if (ui_rect_hit(note_action_rect(1), x, y)) return export_notes();
        if (ui_rect_hit(note_action_rect(2), x, y)) {
            s_view = READING;
            sensor_set(ctx, reader_sensor_required());
            s_toolbar = false;
            s_note_message[0] = 0;
            s_full = true;
            return APP_REDRAW_FULL;
        }
        size_t count = s_view == BOOKMARKS ? s_notes.bookmark_count : s_notes.excerpt_count;
        for (int row = 0; row < BOOK_NOTE_ROWS; ++row) {
            if (ui_rect_hit(note_delete_rect(row), x, y)) {
                size_t index = (size_t)ctx->leaf * BOOK_NOTE_ROWS + (size_t)row;
                if (index < count) {
                    s_note_delete_index = index;
                    s_note_delete_index_valid = true;
                    s_note_delete_confirm = true;
                }
                return APP_REDRAW_PAGE;
            }
        }
        for (int row = 0; row < BOOK_NOTE_ROWS; ++row) {
            size_t index = (size_t)ctx->leaf * BOOK_NOTE_ROWS + (size_t)row;
            if (index >= count || !ui_rect_hit(note_row_rect(row), x, y)) continue;
            const book_note_position_t *position = s_view == BOOKMARKS
                ? &s_notes.bookmarks[index]
                : &s_notes.excerpts[index].position;
            return jump_to_note(ctx, position);
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == READING) {
        if (s_image_open) return close_image(ctx);
        if (ttf_font_is_builtin() && y < 140) { goto_font(ctx); return APP_REDRAW_NONE; }
        if (ui_rect_hit(progress_rect(), x, y)) return seek_progress(ctx, x);
        if (s_toolbar) {
            for (int i = 0; i < BOOK_TOOL_COUNT; ++i) if (ui_rect_hit(tool_rect(i), x, y)) {
                invalidate_prep();
                if (i == 0) { save_progress(); sensor_set(ctx, false); s_view = TOC; ctx->leaf = s_chapter / BOOK_TOC_ROWS; }
                else if (i == 1 || i == 2) return resize_text(ctx, i == 1 ? -1 : 1);
                else if (i == 3) open_note_list(ctx, BOOKMARKS);
                else if (i == 4) open_note_list(ctx, EXCERPTS);
                else if (i == 5) {
                    uint8_t gesture = (app_settings_book_gesture() + 1) % 3;
                    app_settings_set_book_gesture(gesture);
                    s_shake_enabled = gesture == 1;
                    sensor_set(ctx, gesture != 0 || app_settings_book_rotation() == 2);
                }
                else if (i == 6) return APP_REDRAW_FULL;
                else if (i == 7) { save_progress(); sensor_set(ctx, false); free_book(); s_view = SHELF; scan_shelf(ctx); }
                else if (i == 8) { app_settings_set_book_refresh((app_settings_book_refresh() + 1) % 3); s_pages_since_clean = 0; return APP_REDRAW_FULL; }
                else if (i == 9) { sensor_set(ctx, false); s_view = TYPOGRAPHY; save_progress(); return APP_REDRAW_PAGE; }
                else if (i == 10) {
                    uint8_t rotation = (app_settings_book_rotation() + 1) % 3;
                    app_settings_set_book_rotation(rotation);
                    s_reader_landscape = rotation == 1;
                    sensor_set(ctx, app_settings_book_gesture() != 0 || rotation == 2);
                    size_t off = book_layout_page_start_offset(s_page);
                    lock_draw();
                    bool ok = book_layout_build_blocks_metrics(s_text, s_text_len, s_blocks, s_block_count,
                                                               body_rect(), s_px, line_height_percent());
                    if (ok) s_page = book_layout_page_for_offset(off);
                    unlock_draw();
                    if (!ok) copy_text(s_note_message, sizeof(s_note_message), "屏幕旋转后排版失败");
                    s_full = true;
                    return APP_REDRAW_FULL;
                }
                else if (i == 11) { sensor_set(ctx, false); s_search_parent = READING; s_search_draft[0] = 0; s_chapter_search = true; s_view = SEARCH; return APP_REDRAW_PAGE; }
                return APP_REDRAW_PAGE;
            }
        }
        if (!s_toolbar) {
            lock_draw();
            size_t block = book_layout_image_at(s_page, body_rect(), x, y, NULL);
            unlock_draw();
            if (block != SIZE_MAX) return open_image(ctx, block);
        }
        const int rw = reader_width();
        if (x < rw * 3 / 10) return turn_page(ctx, -1);
        if (x >= rw * 7 / 10) return turn_page(ctx, 1);
        s_toolbar = !s_toolbar;
        invalidate_prep();
        return APP_REDRAW_PAGE;
    }
    if (s_view == SHELF || s_view == BULK) {
        if (ui_rect_hit(ui_row_rect(0, 3, 176, 72), x, y)) {
            s_filter = (s_filter + 1) % 3; clear_selection(); s_batch_message[0] = 0;
            sort_shelf(ctx);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(ui_row_rect(1, 3, 176, 72), x, y)) {
            s_recent_sort = !s_recent_sort;
            sort_shelf(ctx);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(ui_row_rect(2, 3, 176, 72), x, y)) { search_begin(); return APP_REDRAW_PAGE; }
        if (s_view == SHELF && s_save_failed && y >= UI_CONTENT_BOTTOM - UI_PX_CAPTION && y < UI_CONTENT_BOTTOM) {
            retry_progress();
            return APP_REDRAW_PAGE;
        }
    }
    int nav = ui_bar_hit(x, y, 3);
    if (nav == 0 || nav == 2) {
        int next = ctx->leaf + (nav == 0 ? -1 : 1);
        if (next >= 0 && next < leaves()) ctx->leaf = next;
        return APP_REDRAW_PAGE;
    }
    if (nav == 1) {
        s_message[0] = 0;
        if (s_view == TOC && s_text) { s_view = READING; s_toolbar = false; sensor_set(ctx, reader_sensor_required()); s_full = true; }
        else { s_view = s_view == BULK ? SHELF : BULK; s_batch_confirm = false; }
        return APP_REDRAW_PAGE;
    }
    if (s_message[0] && !((s_view == SHELF || s_view == BULK) && s_visible_count)) return APP_REDRAW_NONE;
    int rows = s_view == TOC ? BOOK_TOC_ROWS : BOOK_ROWS;
    for (int row = 0; row < rows; ++row) if (ui_rect_hit(row_rect(row, s_view == TOC), x, y)) {
        int i = ctx->leaf * rows + row;
        if (s_view == BULK && i < s_visible_count) toggle_selection(i);
        else if (s_view == SHELF && i < s_visible_count) {
            if (s_shelf[i].removed) {
                s_managed = s_shelf[i]; s_view = MANAGE; s_file_removed = s_delete_confirm = true; s_clear_confirm = false;
                copy_text(s_manage_message, sizeof(s_manage_message), "文件已删除，进度清理失败，请重试");
            } else open_book(ctx, s_shelf[i].path);
        }
        else if (s_view == TOC && i < book_chapter_count()) {
            save_progress();
            if (load_chapter(ctx, i, 0, false)) { s_view = READING; s_toolbar = false; sensor_set(ctx, reader_sensor_required()); s_full = true; save_progress(); }
        }
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}
static void on_enter(app_ctx_t* ctx) {
    s_resume_path[0] = 0;
    s_reading_tick_ms = ctx->now_ms;
    s_reading_acc_ms = 0;
    ensure_prep();
    s_view = SHELF;
    ctx->leaf = 0;
    s_count = 0;
    s_px = app_settings_book_px();
    s_scan_pending = true;
    s_resume_pending = true;
    // 传书页已停止并 join HTTP，安全注销且只丢弃对应路径的旧进度。
    // Transfer has stopped and joined HTTP; safely unregister only invalidated paths.
    pending_drop_invalidated();
    s_store_revision = book_store_revision();
    s_toolbar = s_clear_confirm = s_batch_confirm = false;
    s_pressed_control = -1;
    s_du_count = 0;
    s_full = false;
    s_size_settle_ms = 0;
    s_poll_ms = 0;
    copy_text(s_storage, sizeof(s_storage), "正在检测存储…");
    copy_text(s_message, sizeof(s_message), "正在扫描图书…");
    read_pico_sd_start_probe();
    s_shake_enabled = app_settings_book_gesture() == 1;
    s_reader_landscape = app_settings_book_rotation() == 1;
}
static void book_on_exit(app_ctx_t* ctx) {
    s_resume_path[0] = 0;
    s_pressed_control = -1;
    save_progress();
    reading_stats_add_seconds(s_reading_acc_ms / 1000);
    s_reading_acc_ms = 0;
    sensor_set(ctx, false);
    reader_rotation_end();
    free_book();
    free(s_shelf);
    s_shelf = NULL;
    s_shelf_capacity = 0;
    s_count = s_visible_count = 0;
    if (s_prep_task) { vTaskDelete(s_prep_task); s_prep_task = NULL; }
    free(s_next_fb);
    s_next_fb = NULL;
    if (s_prep_done) { vSemaphoreDelete(s_prep_done); s_prep_done = NULL; }
    if (s_draw_lock) { vSemaphoreDelete(s_draw_lock); s_draw_lock = NULL; }
}
// 先停止使用旧卡句柄与字体预渲染，主循环随后切换内置字体。
// Stop old-card handles and font preparation before the loop switches to the builtin font.
static void book_on_media_lost(app_ctx_t* ctx) {
    s_resume_path[0] = 0;
    reader_rotation_end();
    lock_draw();
    save_progress();
    free_book();
    unlock_draw();
    s_view = SHELF;
    s_count = s_visible_count = 0;
    ctx->leaf = 0;
    s_toolbar = s_clear_confirm = s_batch_confirm = false;
    s_pressed_control = -1;
    s_scan_pending = true;
    s_resume_pending = false;
    s_du_count = 0;
    s_size_settle_ms = 0;
    copy_text(s_storage, sizeof(s_storage), "TF 卡已移除");
    copy_text(s_message, sizeof(s_message), "已停止阅读，正在检查内置图书");
}
// 控件编号仅用于保持按下与抬起命中同一个目标。/ IDs pair a press with release on the same control.
static int control_at(app_ctx_t* ctx, uint16_t x, uint16_t y, EpdRect* rect) {
    if (s_resume_path[0]) {
        for (int i = 0; i < 2; ++i) {
            *rect = ui_row_rect(i, 2, 620, UI_BTN_H);
            if (ui_rect_hit(*rect, x, y)) return 700 + i;
        }
        return -1;
    }
    if (s_view == SEARCH) {
        for (int i = 0; i < 45; ++i) { *rect = search_rect(i); if (ui_rect_hit(*rect, x, y)) return 500 + i; }
        return -1;
    }
    if (s_view == CHAPTER_RESULTS) {
        for (size_t i = 0; i < s_chapter_match_count && i < BOOK_SEARCH_RESULTS; ++i) { *rect = row_rect((int)i, false); if (ui_rect_hit(*rect, x, y)) return 1200 + (int)i; }
        return -1;
    }
    if (s_view == BULK && s_batch_confirm) {
        for (int i = 0; i < 2; ++i) { *rect = ui_row_rect(i, 2, 620, UI_BTN_H); if (ui_rect_hit(*rect, x, y)) return 600 + i; }
        return -1;
    }
    if (s_view == BOOKMARKS || s_view == EXCERPTS) {
        if (s_note_delete_confirm) {
            for (int i = 0; i < 2; ++i) { *rect = ui_row_rect(i, 2, 630, UI_BTN_H); if (ui_rect_hit(*rect, x, y)) return 830 + i; }
            return -1;
        }
        for (int row = 0; row < BOOK_NOTE_ROWS; ++row) {
            *rect = note_delete_rect(row);
            if (ui_rect_hit(*rect, x, y)) return 800 + row;
        }
        for (int i = 0; i < 3; ++i) {
            *rect = note_action_rect(i);
            if (ui_rect_hit(*rect, x, y)) return 820 + i;
        }
        return -1;
    }
    if (s_view == TYPOGRAPHY) {
        for (int i = 0; i < 4; ++i) { *rect = ui_row_rect(0, 1, 340 + i * 116, 84); if (ui_rect_hit(*rect, x, y)) return 900 + i; }
        return -1;
    }
    if (s_view == BULK) for (int i = 0; i < 5; ++i) { *rect = batch_rect(i); if (ui_rect_hit(*rect, x, y)) return 610 + i; }
    if (s_view == MANAGE) {
        int count = s_clear_confirm || s_file_removed ? 2 : 3;
        for (int i = 0; i < count; ++i) {
            *rect = manage_rect(i, count);
            if (ui_rect_hit(*rect, x, y)) return s_clear_confirm ? 300 + i : s_file_removed && i == 1 ? 403 : 400 + i;
        }
        return -1;
    }
    if (s_clear_confirm) {
        for (int i = 0; i < 2; ++i) {
            *rect = ui_row_rect(i, 2, 610, UI_BTN_H);
            if (ui_rect_hit(*rect, x, y)) return 300 + i;
        }
        return -1;
    }
    if (s_view == READING) {
        if (s_toolbar) for (int i = 0; i < BOOK_TOOL_COUNT; ++i) {
            *rect = tool_rect(i);
            if (ui_rect_hit(*rect, x, y)) return 200 + i;
        }
        if (!s_toolbar) {
            lock_draw();
            size_t block = book_layout_image_at(s_page, body_rect(), x, y, rect);
            unlock_draw();
            if (block != SIZE_MAX) return 1000 + (int)block;
        }
        return -1;
    }
    for (int i = 0; i < 3; ++i) {
        *rect = ui_bar_rect(i, 3);
        if (ui_rect_hit(*rect, x, y)) return 100 + i;
    }
    if (s_view == SHELF || s_view == BULK) {
        for (int i = 0; i < 3; ++i) {
            *rect = ui_row_rect(i, 3, 176, 72);
            if (ui_rect_hit(*rect, x, y)) return 110 + i;
        }
        *rect = (EpdRect){UI_MARGIN, UI_CONTENT_BOTTOM - UI_PX_CAPTION, ui_content_width(), UI_PX_CAPTION};
        if (s_view == SHELF && s_save_failed && ui_rect_hit(*rect, x, y)) return 113;
    }
    if (s_message[0] && !((s_view == SHELF || s_view == BULK) && s_visible_count)) return -1;
    int rows = s_view == TOC ? BOOK_TOC_ROWS : BOOK_ROWS;
    int count = s_view == TOC ? (int)book_chapter_count() : s_visible_count;
    for (int row = 0; row < rows && ctx->leaf * rows + row < count; ++row) {
        *rect = row_rect(row, s_view == TOC);
        if (ui_rect_hit(*rect, x, y)) return row;
    }
    return -1;
}
static app_redraw_t paint_control(app_ctx_t* ctx, EpdRect rect) {
    render(ctx, ctx->fb);
    s_area = rect;
    s_mode = MODE_DU;
    return APP_REDRAW_AREA;
}
static void reader_touch_map(uint16_t *x, uint16_t *y) {
    if (!s_reader_landscape) return;
    uint16_t old_x = *x;
    *x = *y;
    *y = epd_height() > old_x ? (uint16_t)(epd_height() - old_x - 1) : 0;
}
static app_redraw_t gesture_event(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (s_image_open) {
        s_pressed_control = -1;
        return ev->type == UI_GESTURE_TAP ? close_image(ctx) : APP_REDRAW_NONE;
    }
    ui_gesture_event_t mapped = *ev;
    if (s_view == READING) {
        reader_touch_map(&mapped.x0, &mapped.y0);
        reader_touch_map(&mapped.x, &mapped.y);
    }
    const ui_gesture_event_t *e = &mapped;
    EpdRect start_rect = {0}, end_rect = {0};
    int start = control_at(ctx, e->x0, e->y0, &start_rect);
    int end = control_at(ctx, e->x, e->y, &end_rect);
    if (e->type == UI_GESTURE_PRESS) {
        if (start >= 1000) { s_pressed_control = -1; return APP_REDRAW_NONE; }
        s_pressed_control = start;
        return start >= 0 ? paint_control(ctx, start_rect) : APP_REDRAW_NONE;
    }
    bool decorated = s_pressed_control >= 0;
    s_pressed_control = -1;
    if (e->type == UI_GESTURE_LONG_PRESS && start == end) {
        if (s_view == SHELF && start >= 0 && start < BOOK_ROWS && !s_clear_confirm) {
            s_managed = s_shelf[ctx->leaf * BOOK_ROWS + start];
            s_view = MANAGE;
            s_clear_confirm = false;
            s_delete_confirm = s_file_removed = s_managed.removed;
            copy_text(s_manage_message, sizeof(s_manage_message), s_file_removed ? "文件已删除，进度清理失败，请重试" : "");
            return APP_REDRAW_PAGE;
        }
        if (s_view == READING && !s_toolbar && !s_clear_confirm && ui_rect_hit(body_rect(), e->x0, e->y0)) {
            save_progress();
            sensor_set(ctx, false);
            s_view = TOC;
            ctx->leaf = s_chapter / BOOK_TOC_ROWS;
            return APP_REDRAW_PAGE;
        }
    }
    if (e->type == UI_GESTURE_TAP && !s_scan_pending) {
        if (start >= 0 && start == end) {
            app_redraw_t result = action_at(ctx, e->x0, e->y0);
            return result != APP_REDRAW_NONE ? result : paint_control(ctx, start_rect);
        }
        if (start < 0 && end < 0 &&
            (s_view == READING || s_view == BOOKMARKS || s_view == EXCERPTS) && !s_clear_confirm)
            return action_at(ctx, e->x0, e->y0);
    }
    if (!s_clear_confirm && !s_scan_pending && !s_resume_path[0]) {
        if (s_view == READING && !s_toolbar && ui_rect_hit(body_rect(), e->x0, e->y0)) {
            if (e->type == UI_GESTURE_SWIPE_L) return turn_page(ctx, 1);
            if (e->type == UI_GESTURE_SWIPE_R) return turn_page(ctx, -1);
        } else if ((s_view == SHELF || s_view == TOC || (s_view == BULK && !s_batch_confirm) ||
                    s_view == BOOKMARKS || s_view == EXCERPTS) &&
                   (e->type == UI_GESTURE_SWIPE_U || e->type == UI_GESTURE_SWIPE_D)) {
            int next = ctx->leaf + (e->type == UI_GESTURE_SWIPE_U ? 1 : -1);
            if (next >= 0 && next < leaves()) { ctx->leaf = next; return APP_REDRAW_PAGE; }
        }
    }
    return decorated ? paint_control(ctx, start_rect) : APP_REDRAW_NONE;
}
static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    s_pressed_control = -1;
    if (key == UI_KEY_1) {
        if (s_image_open) return close_image(ctx);
        if (s_resume_path[0]) return resume_choice(ctx, false);
        if (s_scan_pending) return APP_REDRAW_NONE;
        if (s_view == BOOKMARKS || s_view == EXCERPTS) {
            if (s_note_delete_confirm) {
                s_note_delete_confirm = false;
                s_note_delete_index_valid = false;
                return APP_REDRAW_PAGE;
            }
            s_view = READING; s_toolbar = false;
            sensor_set(ctx, reader_sensor_required());
            s_full = true;
            return APP_REDRAW_FULL;
        }
        if (s_view == TYPOGRAPHY || s_view == CHAPTER_RESULTS) {
            s_view = READING; s_toolbar = false;
            sensor_set(ctx, reader_sensor_required());
            s_full = true;
            return APP_REDRAW_FULL;
        }
        if (s_view == MANAGE) {
            if (s_clear_confirm) { s_clear_confirm = false; return APP_REDRAW_PAGE; }
            s_view = SHELF;
            return APP_REDRAW_PAGE;
        }
        if (s_view == SEARCH) { search_finish(ctx, false); return APP_REDRAW_PAGE; }
        if (s_view == BULK) {
            if (s_batch_confirm) { s_batch_confirm = false; return APP_REDRAW_PAGE; }
            s_view = SHELF;
            return APP_REDRAW_PAGE;
        }
        if (s_view == TOC) {
            if (s_text) {
                s_view = READING; s_toolbar = false;
                sensor_set(ctx, reader_sensor_required());
                s_full = true;
                return APP_REDRAW_FULL;
            }
            s_view = SHELF;
            return APP_REDRAW_PAGE;
        }
        if (s_view == READING) {
            if (s_clear_confirm) { s_clear_confirm = false; return APP_REDRAW_PAGE; }
            if (s_toolbar) { s_toolbar = false; invalidate_prep(); return APP_REDRAW_PAGE; }
            ctx->key_handled = true;
            return turn_page(ctx, -1);
        }
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_3 && s_view == READING && !s_image_open && !s_toolbar &&
        !s_clear_confirm && !s_scan_pending && !s_resume_path[0]) {
        ctx->key_handled = true;
        return turn_page(ctx, 1);
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (ctx->consumed) return APP_REDRAW_NONE;
    if (s_view == READING && s_text && ctx->now_ms - s_battery_poll_ms >= 30000) {
        s_battery_poll_ms = ctx->now_ms;
        if (read_pico_pmu_poll() == ESP_OK) {
            const pmu_snapshot_t *pmu = read_pico_pmu_get();
            bool charging = pmu->charge_state == PMU_CHARGE_CHARGING;
            bool low = pmu->status_ok && ((pmu->flags & PMU_STATUS_BATTERY_CRITICAL) ||
                                          (pmu->soc_permille <= 50 && !charging));
            if (low && !s_low_battery_notified) {
                save_progress();
                s_low_battery_notified = true;
                copy_text(s_message, sizeof(s_message), "电量过低，阅读进度已保存，请尽快充电");
                return APP_REDRAW_PAGE;
            }
            if (!low) s_low_battery_notified = false;
        }
    }
    if (s_reading_tick_ms == 0) s_reading_tick_ms = ctx->now_ms;
    int64_t elapsed = ctx->now_ms - s_reading_tick_ms;
    s_reading_tick_ms = ctx->now_ms;
    if (s_view == READING && s_text && elapsed > 0 && elapsed <= 5000) {
        s_reading_acc_ms += (uint32_t)elapsed;
        if (s_reading_acc_ms >= 60000) {
            uint32_t minutes = s_reading_acc_ms / 60000;
            reading_stats_add_seconds(minutes * 60);
            s_reading_acc_ms -= minutes * 60000;
        }
    }
    if (s_save_failed && ctx->now_ms - s_save_retry_ms >= 15000) {
        s_save_retry_ms = ctx->now_ms;
        bool failed = s_save_failed;
        retry_progress();
        if (failed != s_save_failed) { invalidate_prep(); return APP_REDRAW_PAGE; }
    }
    if (s_scan_pending && ctx->now_ms - s_poll_ms >= 500) {
        s_poll_ms = ctx->now_ms;
        read_pico_sd_info_t info = {0};
        if (read_pico_sd_get_info(&info) == ESP_ERR_NOT_FINISHED) return APP_REDRAW_NONE;
        s_scan_pending = false;
        scan_shelf(ctx);
        if (s_resume_pending) {
            s_resume_pending = false;
            char path[BOOK_STORE_PATH_MAX];
            if (book_progress_last_path(path, sizeof(path))) {
                bool found = false;
                for (int i = 0; i < s_count; ++i) if (!s_shelf[i].removed && !strcmp(s_shelf[i].path, path)) {
                    found = true;
                    if (s_shelf[i].has_progress) {
                        copy_text(s_resume_path, sizeof(s_resume_path), path);
                        copy_text(s_resume_name, sizeof(s_resume_name), s_shelf[i].name);
                    } else {
                        (void)book_progress_set_last_path("");
                    }
                    break;
                }
                if (!found) (void)book_progress_set_last_path("");
            }
        }
        return APP_REDRAW_PAGE;
    }
    if (s_du_count && (s_du_count >= UI_SETTLE_DU_MAX ||
        ctx->now_ms - s_du_ms >= UI_SETTLE_IDLE_MS)) {
        s_area = s_du_area;
        s_mode = MODE_GL16;
        ESP_LOGI(TAG, "settle du=%u", s_du_count);
        return APP_REDRAW_AREA;
    }
    if (s_text && strcmp(s_font_path, ttf_font_path())) {
        size_t off = book_layout_page_start_offset(s_page);
        save_progress();
        lock_draw();
        invalidate_prep();
        if (ttf_font_has_variable_weight()) ttf_font_set_weight(app_settings_book_weight());
        bool ok = book_layout_build_blocks_metrics(s_text, s_text_len, s_blocks, s_block_count, body_rect(), s_px,
                                                   line_height_percent());
        if (ok) s_page = book_layout_page_for_offset(off);
        copy_text(s_font_path, sizeof(s_font_path), ttf_font_path());
        unlock_draw();
        if (!ok) { free_book(); s_view = SHELF; copy_text(s_message, sizeof(s_message), "字体重排失败，请重新打开图书"); }
        return APP_REDRAW_PAGE;
    }
    if (s_size_settle_ms && ctx->now_ms >= s_size_settle_ms) {
        s_size_settle_ms = 0;
        if (s_view == READING && !s_image_open) return paint_reading(ctx, MODE_GL16);
    }
    if (s_view == READING && !s_image_open && s_text && !book_layout_complete() &&
        !s_toolbar && !(ctx->touch && ctx->touch->touched)) {
        lock_draw();
        invalidate_prep();
        bool ok = book_layout_extend(2);
        unlock_draw();
        if (!ok) {
            free_book(); s_view = SHELF; ctx->leaf = 0;
            copy_text(s_message, sizeof(s_message), "排版失败：内存不足或章节过长");
            return APP_REDRAW_PAGE;
        }
        // 页数完成后随下次交互更新，避免阅读中重复刷屏。
        // Display the final page count on the next interaction, avoiding unsolicited refreshes.
    }
    if (s_sensor_on && ctx->now_ms - s_sensor_ms >= 40) {
        s_sensor_ms = ctx->now_ms;
        if (!sc7a20h_powered(ctx->acc)) sensor_set(ctx, true);
        uint8_t gesture = app_settings_book_gesture();
        bool suppressed = s_view != READING || s_image_open || s_toolbar || s_clear_confirm ||
                          (ctx->touch && ctx->touch->touched) || ctx->now_ms - s_last_turn_ms < 800;
        if (gesture != 0) {
            sc7a20h_events_t ev;
            if (sc7a20h_read_events(ctx->acc, &ev) == ESP_OK) {
                if (gesture == 1 && book_shake_feed(&s_shake, ev.aoi2_src & 0x40, suppressed, ctx->now_ms))
                    return turn_page(ctx, 1);
                if (gesture == 2 && ev.click_src && !suppressed) return turn_page(ctx, 1);
            }
        }
        if (app_settings_book_rotation() == 2 && ctx->now_ms - s_auto_sample_ms >= 100) {
            s_auto_sample_ms = ctx->now_ms;
            sc7a20h_sample_t sample;
            if (sc7a20h_read_new(ctx->acc, &sample) == ESP_OK) {
                int ax = abs(sample.x_mg), ay = abs(sample.y_mg);
                bool valid = (ax > 450 || ay > 450) && (ax > ay * 13 / 10 || ay > ax * 13 / 10);
                if (valid) {
                    bool candidate = ax > ay;
                    if (candidate != s_reader_landscape) {
                        if (s_auto_candidate_since == 0 || candidate != s_auto_candidate_landscape) {
                            s_auto_candidate_landscape = candidate;
                            s_auto_candidate_since = ctx->now_ms;
                        } else if (ctx->now_ms - s_auto_candidate_since >= 1500) {
                            size_t offset = book_layout_page_start_offset(s_page);
                            s_reader_landscape = candidate;
                            lock_draw();
                            invalidate_prep();
                            bool ok = book_layout_build_blocks_metrics(s_text, s_text_len, s_blocks, s_block_count,
                                                                       body_rect(), s_px, line_height_percent());
                            if (ok) s_page = book_layout_page_for_offset(offset);
                            unlock_draw();
                            s_auto_candidate_since = 0;
                            if (ok) { s_full = true; return APP_REDRAW_FULL; }
                        }
                    } else s_auto_candidate_since = 0;
                }
            }
        }
    }
    return APP_REDRAW_NONE;
}
static EpdRect area_hint(app_ctx_t* ctx) { (void)ctx; return s_area; }

const app_desc_t app_book = {
    .title = "图书 Books", .detail = "TF 卡 txt / epub 阅读", .owns_keys = true,
    .render = render, .present = present, .on_enter = on_enter, .on_exit = book_on_exit,
    .on_media_lost = book_on_media_lost,
    .on_gesture = gesture_event, .on_key = on_key,
    .on_tick = on_tick, .area_hint = area_hint,
};
