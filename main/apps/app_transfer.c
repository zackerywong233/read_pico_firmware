/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 传书页负责两种接入模式生命周期、书源注入与状态展示；HTTP和文件接收属于独立组件。
 * Transfer page owns both network modes, storage injection and status; the component receives files.
 *
 * 冻结：按用户双模式要求提供热点与已有WiFi，离页停止网络。
 * 本页停留期间不计空闲自动休眠，避免等人传文件时热点和接收被掐断。
 * 决策变更：2 分钟空闲锁屏会把传书流程关掉，用户要求这段流程不能被打断。
 * 电源键锁屏仍走全局路径，深睡/断电会中断传输。
 * render只画快照；内置存储单文件上限由book_store提供，不格式化TF卡。
 * 上传并发扫描曾欠载，页内增加预填余量；离页停服后恢复，不改波形或像素时钟。
 * 用户要求设备端配网：停服后扫描选网，ASCII密码只在连接保存时写入，离开输入页即清空。
 * 用户验收要求停止后恢复进入传书前的页面或菜单位置；遗忘网络必须确认。
 * 用户要求联网后提供网址二维码；热点用单码切换连接/网页，网络变更清除旧码。
 * 卡失效时停止并汇合接收任务，清除旧容量与二维码；当前请求不得切换存储源。
 * TF 卡和内置存储同时可用时允许点按容量行切换图书上传目标；仅空闲时切换并重启服务，TTF 仍只写 TF 卡。
 * Frozen: user-requested AP/STA modes stop networking on exit.
 * Idle auto-sleep does not run while this page is current, so a waiting hotspot and reception stay up.
 * Decision change: the 2-minute idle lock was closing the transfer flow; the user requires that flow to keep running.
 * The power key still uses the global lock path, and deep sleep or power loss still interrupts transfer.
 * Render only paints snapshots; book_store defines the flash file limit. Never format the TF card.
 * Concurrent uploads underrun scan queues; increase prefill until service exit without changing waveforms or pixel clocks.
 * User-requested device provisioning scans while stopped; save ASCII passwords only on connect and clear input on leaving the editor.
 * Acceptance requires returning to the page or menu position used to enter transfer; forgetting WiFi requires confirmation.
 * User-requested URL QR follows network readiness; AP switches one code between joining and browsing, discarding stale codes on changes.
 * Lost media stops and joins reception and clears capacity/QR; never switch storage beneath an active request.
 * When TF and flash are both available, the capacity row switches the book target; restart only while idle, and keep TTF uploads TF-only.
 */
// 整个传书页暂停 SD 字体并使用内置字库；HTTP 停止后才恢复读取。
// Use the built-in font and suspend SD font opens for this page; resume only after HTTP stops.
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "app_focus.h"
#include "app_web.h"
#include "ble_service.h"
#include "reading_stats.h"
#include "settings.h"
#include "time_sync.h"
#include "web_bookmarks.h"
#include "feeds_store.h"
#include "book_notes.h"
#include "book_store.h"
#include "book_progress.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "read_pico_pmu.h"
#include "read_pico_transfer.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_wifi_qr.h"

static const char* TAG = "transfer_page";
static book_store_root_t s_root;
static read_pico_transfer_status_t s_status;
static bool s_start_pending, s_settle, s_any_changed;
static bool s_media_lost;
static esp_err_t s_font_error;
static int s_pressed = -1;
static read_pico_transfer_mode_t s_mode = READ_PICO_TRANSFER_MODE_AP;
static bool s_session_started;
static bool s_upload_flash, s_can_switch_root;
static char s_root_message[96];
static int64_t s_poll_ms;
static uint64_t s_free;
static size_t s_heap_before, s_internal_before;
static EpdRect s_area;
static bool s_qr_url, s_qr_ready;
static read_pico_storage_info_t s_storage_info;
static _Atomic bool s_settings_runtime_dirty;
static _Atomic bool s_remote_switch_pending;
static _Atomic bool s_remote_switch_flash;
static _Atomic bool s_pending_time_sync;
static _Atomic uint32_t s_pending_time_utc;
static _Atomic bool s_pending_focus_ctrl;
static _Atomic bool s_pending_focus_start;
static _Atomic int s_pending_focus_min;
static _Atomic bool s_remote_stop_pending;
static int64_t s_remote_stop_at_ms;
static int64_t s_pmu_poll_ms;

static bool transfer_storage_info(read_pico_storage_info_t* out, void* ctx) {
    (void)ctx;
    if (!out) return false;
    *out = s_storage_info;
    uint64_t live = book_store_free_bytes(&s_root);
    if (s_root.is_flash) out->flash_free_bytes = live;
    else if (out->sd_available) out->sd_free_bytes = live;
    return true;
}

static bool transfer_storage_switch(bool prefer_flash, void* ctx) {
    (void)ctx;
    if (prefer_flash ? !s_storage_info.flash_available : !s_storage_info.sd_available) return false;
    atomic_store(&s_remote_switch_flash, prefer_flash);
    atomic_store(&s_remote_switch_pending, true);
    return true;
}

static bool transfer_settings_get(read_pico_device_settings_t* out, void* ctx) {
    (void)ctx;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->sleep_mode = (uint8_t)app_settings_sleep_mode();
    out->auto_sleep_sec = app_settings_auto_sleep_sec();
    out->pickup_wake = app_settings_pickup_wake();
    out->book_px = app_settings_book_px();
    out->book_refresh = app_settings_book_refresh();
    out->book_line_height = app_settings_book_line_height();
    out->book_margin = app_settings_book_margin();
    out->book_weight = app_settings_book_weight();
    out->book_rotation = app_settings_book_rotation();
    out->book_gesture = app_settings_book_gesture();
    out->saver_style = (uint8_t)app_settings_saver_style();
    out->dashboard_items = app_settings_dashboard_items();
    out->timezone_min = app_settings_timezone_min();
    out->charge_led = app_settings_charge_led();
    out->low_soc_notify_percent = app_settings_low_soc_notify_percent();
    out->muted = app_settings_muted();
    out->transfer_pin_required = app_settings_transfer_pin_required();
    out->ble_enabled = app_settings_ble_enabled();
    out->ble_pin_required = app_settings_ble_pin_required();
    const char* font = app_settings_font_path();
    snprintf(out->font_path, sizeof(out->font_path), "%s", font ? font : "");
    return true;
}

static esp_err_t transfer_settings_set(const read_pico_device_settings_t* next, void* ctx) {
    (void)ctx;
    if (!next) return ESP_ERR_INVALID_ARG;
    app_settings_set_sleep_mode((app_sleep_mode_t)next->sleep_mode);
    app_settings_set_auto_sleep_sec(next->auto_sleep_sec);
    app_settings_set_pickup_wake(next->pickup_wake);
    app_settings_set_book_px(next->book_px);
    app_settings_set_book_refresh(next->book_refresh);
    app_settings_set_book_line_height(next->book_line_height);
    app_settings_set_book_margin(next->book_margin);
    app_settings_set_book_weight(next->book_weight);
    app_settings_set_book_rotation(next->book_rotation);
    app_settings_set_book_gesture(next->book_gesture);
    app_settings_set_saver_style((app_saver_style_t)next->saver_style);
    app_settings_set_dashboard_items(next->dashboard_items);
    app_settings_set_timezone_min_deferred(next->timezone_min);
    app_settings_set_charge_led_deferred(next->charge_led);
    app_settings_set_low_soc_notify_percent_deferred(next->low_soc_notify_percent);
    app_settings_set_muted(next->muted);
    app_settings_set_transfer_pin_required(next->transfer_pin_required);
    app_settings_set_ble_enabled(next->ble_enabled);
    app_settings_set_ble_pin_required(next->ble_pin_required);
    atomic_store(&s_settings_runtime_dirty, true);
    return ESP_OK;
}

static bool transfer_time_get(read_pico_time_info_t* out, void* ctx) {
    (void)ctx;
    if (!out) return false;
    time_t utc = 0;
    out->time_valid = app_time_now_utc(&utc);
    out->unix_sec = out->time_valid && utc > 0 ? (uint32_t)utc : 0;
    out->timezone_min = app_settings_timezone_min();
    return true;
}

static esp_err_t transfer_time_set(uint32_t unix_sec, bool has_tz, int16_t tz_min, void* ctx) {
    (void)ctx;
    if (has_tz) {
        app_settings_set_timezone_min_deferred(tz_min);
        atomic_store(&s_settings_runtime_dirty, true);
    }
    atomic_store(&s_pending_time_utc, unix_sec);
    atomic_store(&s_pending_time_sync, true);
    return ESP_OK;
}

static esp_err_t transfer_font_select(const char* path, void* ctx) {
    (void)ctx;
    app_settings_set_font_path(path ? path : "");
    return ESP_OK;
}

static bool transfer_dashboard_stats(read_pico_dashboard_stats_t* out, void* ctx) {
    (void)ctx;
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->total_reading_sec = reading_stats_total_seconds();
    out->has_last_book = reading_stats_get_last_book(
        out->last_book_title, sizeof(out->last_book_title), &out->last_book_percent);
    out->has_excerpt = reading_stats_get_excerpt(
        out->excerpt_title, sizeof(out->excerpt_title),
        out->excerpt_text, sizeof(out->excerpt_text));
    out->has_last_book_path = book_progress_last_path(
        out->last_book_path, sizeof(out->last_book_path));
    return true;
}

static esp_err_t transfer_dashboard_stats_set(const read_pico_stats_restore_t* in,
                                              read_pico_dashboard_stats_t* out, void* ctx) {
    if (!in) return ESP_ERR_INVALID_ARG;
    esp_err_t err = ESP_OK;
    if (in->has_total_reading_sec) {
        uint32_t target = in->total_reading_sec;
        if (in->merge) {
            uint32_t cur = reading_stats_total_seconds();
            if (cur > target) target = cur;
        }
        err = reading_stats_set_total_seconds(target);
        if (err != ESP_OK) return err;
    }
    if (in->has_last_book) {
        if (in->clear_last_book) {
            if (!in->merge) {
                err = reading_stats_set_last_book_checked("", 0);
                if (err != ESP_OK) return err;
            }
        } else {
            char cur_title[128] = {0};
            uint8_t cur_pct = 0;
            bool have_cur = in->merge && reading_stats_get_last_book(cur_title, sizeof(cur_title), &cur_pct);
            if (!have_cur) {
                err = reading_stats_set_last_book_checked(in->last_book_title, in->last_book_percent);
                if (err != ESP_OK) return err;
            }
        }
    }
    if (in->has_excerpt) {
        if (in->clear_excerpt) {
            if (!in->merge) {
                err = reading_stats_set_excerpt_checked("", "");
                if (err != ESP_OK) return err;
            }
        } else {
            char cur_title[128] = {0};
            char cur_text[16] = {0};
            bool have_cur = in->merge && reading_stats_get_excerpt(
                cur_title, sizeof(cur_title), cur_text, sizeof(cur_text));
            if (!have_cur) {
                err = reading_stats_set_excerpt_checked(in->excerpt_title, in->excerpt_text);
                if (err != ESP_OK) return err;
            }
        }
    }
    if (out) (void)transfer_dashboard_stats(out, ctx);
    return ESP_OK;
}

static bool transfer_book_progress(const char* path, uint32_t size,
                                   read_pico_book_progress_t* out, void* ctx) {
    (void)ctx;
    if (!path || !out) return false;
    memset(out, 0, sizeof(*out));
    book_progress_t prog = {0};
    if (!book_progress_load(path, size, &prog)) return true;
    out->has_progress = true;
    out->percent = prog.pct;
    out->chapter = prog.chapter;
    out->byte_off = prog.byte_off;
    out->px = prog.px;
    out->order = prog.last_open_s;
    return true;
}

static esp_err_t transfer_book_progress_set(const char* path, uint32_t size,
                                            const read_pico_book_progress_t* in, bool set_last,
                                            read_pico_book_progress_t* out, void* ctx) {
    (void)ctx;
    if (!path || !in) return ESP_ERR_INVALID_ARG;
    uint8_t px = in->px ? in->px : app_settings_book_px();
    if (px < 36 || px > 72 || (px - 36) % 4 != 0) px = 48;
    book_progress_t prog = {
        .file_size = size,
        .chapter = in->chapter,
        .byte_off = in->byte_off,
        .px = px,
        .pct = in->percent,
        .last_open_s = 0,
    };
    // 失效阅读页旧待保存监视，避免退出传书后回写过期进度。
    // Invalidate stale reader pending watches so exiting Transfer never overwrites restored progress.
    book_progress_invalidate_watch(path);
    esp_err_t err = book_progress_save(path, &prog);
    if (err != ESP_OK) return err;
    if (set_last) {
        err = book_progress_set_last_path(path);
        if (err != ESP_OK) return err;
    }
    if (out) {
        memset(out, 0, sizeof(*out));
        book_progress_t saved = {0};
        if (book_progress_load(path, size, &saved)) {
            out->has_progress = true;
            out->percent = saved.pct;
            out->chapter = saved.chapter;
            out->byte_off = saved.byte_off;
            out->px = saved.px;
            out->order = saved.last_open_s;
        } else {
            out->has_progress = true;
            out->percent = prog.pct;
            out->chapter = prog.chapter;
            out->byte_off = prog.byte_off;
            out->px = prog.px;
        }
    }
    return ESP_OK;
}

static esp_err_t transfer_book_notes_set(const char* path, uint32_t size,
                                         const read_pico_book_note_item_t* items, size_t count,
                                         void* ctx) {
    (void)ctx;
    if (!path || (count && !items)) return ESP_ERR_INVALID_ARG;
    if (count == 0) return book_notes_forget(path);
    size_t bookmark_count = 0, excerpt_count = 0;
    for (size_t i = 0; i < count; ++i) {
        if (items[i].is_excerpt) ++excerpt_count;
        else ++bookmark_count;
    }
    book_note_position_t* bookmarks = bookmark_count
        ? (book_note_position_t*)calloc(bookmark_count, sizeof(*bookmarks)) : NULL;
    book_note_excerpt_t* excerpts = excerpt_count
        ? (book_note_excerpt_t*)calloc(excerpt_count, sizeof(*excerpts)) : NULL;
    if ((bookmark_count && !bookmarks) || (excerpt_count && !excerpts)) {
        free(bookmarks);
        free(excerpts);
        return ESP_ERR_NO_MEM;
    }
    size_t bi = 0, ei = 0;
    for (size_t i = 0; i < count; ++i) {
        book_note_position_t pos = {
            .chapter = items[i].chapter,
            .byte_off = items[i].byte_off,
            .percent = items[i].percent,
        };
        if (items[i].is_excerpt) {
            excerpts[ei].position = pos;
            excerpts[ei].text = (char*)items[i].text;
            excerpts[ei].text_len = items[i].text_len;
            ++ei;
        } else {
            bookmarks[bi++] = pos;
        }
    }
    book_notes_t notes = {
        .bookmark_count = bookmark_count,
        .excerpt_count = excerpt_count,
        .bookmarks = bookmarks,
        .excerpts = excerpts,
    };
    esp_err_t err = book_notes_save(path, size, &notes);
    free(bookmarks);
    free(excerpts);
    return err;
}

static size_t transfer_webmarks_list(read_pico_webmark_t* out, size_t cap, void* ctx) {
    (void)ctx;
    if (!out || !cap) return 0;
    // 收藏数组约 6.4 KB，放 PSRAM 避免在 HTTPD 任务栈上溢出。
    // Allocate the ~6.4 KB bookmark array in PSRAM to keep it off the HTTPD task stack.
    web_bookmark_t* items = heap_caps_calloc(WEB_BOOKMARK_MAX, sizeof(*items), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!items) items = calloc(WEB_BOOKMARK_MAX, sizeof(*items));
    if (!items) return 0;
    size_t count = 0;
    web_bookmarks_load(items, &count);
    if (count > cap) count = cap;
    for (size_t i = 0; i < count; ++i) {
        snprintf(out[i].title, sizeof(out[i].title), "%.*s",
                 (int)(sizeof(out[i].title) - 1), items[i].title);
        snprintf(out[i].url, sizeof(out[i].url), "%.*s",
                 (int)(sizeof(out[i].url) - 1), items[i].url);
    }
    free(items);
    return count;
}

static esp_err_t transfer_webmarks_upsert(const char* title, const char* url, void* ctx) {
    (void)ctx;
    return web_bookmarks_upsert(title, url);
}

static esp_err_t transfer_webmarks_delete(const char* url, void* ctx) {
    (void)ctx;
    return web_bookmarks_delete(url);
}

static size_t transfer_feeds_list(read_pico_feed_source_t* out, size_t cap, void* ctx) {
    (void)ctx;
    if (!out || !cap) return 0;
    // 订阅源公开表约 6.2 KB，放 PSRAM 避免与 feeds_store 叠加撑爆 HTTPD 任务栈。
    // Allocate the ~6.2 KB public feed array in PSRAM to avoid overflowing the HTTPD task stack.
    feed_public_source_t* items = heap_caps_calloc(FEEDS_MAX_SOURCES, sizeof(*items), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!items) items = calloc(FEEDS_MAX_SOURCES, sizeof(*items));
    if (!items) return 0;
    size_t count = feeds_store_list_public(items, FEEDS_MAX_SOURCES);
    if (count > cap) count = cap;
    for (size_t i = 0; i < count; ++i) {
        snprintf(out[i].name, sizeof(out[i].name), "%s", items[i].name);
        snprintf(out[i].url, sizeof(out[i].url), "%s", items[i].url);
        out[i].kind = items[i].kind;
        snprintf(out[i].auth_user, sizeof(out[i].auth_user), "%s", items[i].auth_user);
        out[i].has_password = items[i].has_password;
    }
    free(items);
    return count;
}

static esp_err_t transfer_feeds_upsert(const char* name, const char* url, uint8_t kind,
                                       const char* auth_user, const char* auth_pass,
                                       bool clear_auth, void* ctx) {
    (void)ctx;
    bool full = false;
    if (!feeds_store_upsert(name, url, (feed_kind_t)kind, auth_user, auth_pass, clear_auth, &full)) {
        return full ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t transfer_feeds_delete(const char* url, void* ctx) {
    (void)ctx;
    bool found = false;
    if (!feeds_store_delete(url, &found)) return ESP_FAIL;
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool transfer_focus_get(read_pico_focus_status_t* out, void* ctx) {
    (void)ctx;
    if (!out) return false;
    app_focus_status_t st = {0};
    app_focus_get_status(&st);
    out->running = st.running;
    out->is_break = st.is_break;
    out->duration_min = st.duration_min;
    out->remaining_sec = st.remaining_sec;
    out->completed_count = st.completed_count;
    return true;
}

static esp_err_t transfer_focus_ctrl(bool start, int minutes, void* ctx) {
    (void)ctx;
    if (start && minutes != 15 && minutes != 25 && minutes != 45) return ESP_ERR_INVALID_ARG;
    atomic_store(&s_pending_focus_start, start);
    atomic_store(&s_pending_focus_min, minutes);
    atomic_store(&s_pending_focus_ctrl, true);
    return ESP_OK;
}

static bool transfer_stop_request(void* ctx) {
    (void)ctx;
    atomic_store(&s_remote_stop_pending, true);
    return true;
}

static void clear_qr(void) {
    ui_wifi_qr_clear();
    s_qr_ready = false;
}

// 仅在网络快照变化或用户切换时编码，render与上传进度不触发编码。
// Encode only on network snapshot changes or user switching, never from render or upload progress.
static void prepare_qr(void) {
    clear_qr();
    if (!s_status.network_ready) return;
    s_qr_ready = s_qr_url || s_mode == READ_PICO_TRANSFER_MODE_STA
        ? ui_wifi_qr_prepare_url(s_status.url)
        : ui_wifi_qr_prepare(s_status.ssid, READ_PICO_TRANSFER_PASSWORD);
}

typedef enum { TRANSFER_HOME, TRANSFER_NETWORKS, TRANSFER_PASSWORD, TRANSFER_SAVED } transfer_view_t;
#define NETWORK_ROWS 6
#define PASSWORD_MAX 64
static transfer_view_t s_view;
static read_pico_transfer_network_t s_networks[READ_PICO_TRANSFER_SCAN_MAX], s_selected;
static size_t s_network_count, s_network_page;
static bool s_scan_pending, s_saved_configured, s_password_visible, s_forget_confirm;
static char s_saved_ssid[33], s_password[PASSWORD_MAX + 1], s_network_message[96];
static char s_saved_ssids[5][33];
static size_t s_saved_count;
static int s_keyboard_mode;

static void render(app_ctx_t* ctx, uint8_t* fb);
static void stop_session(void);
bool app_transfer_remote_stop(app_ctx_t* ctx);

/* ---- 设备端配网 / Device provisioning ---- */
static void fit_label(char* text, int px, int width) {
    while (*text && ttf_text_width_px(px, text) > width) {
        size_t n = strlen(text) - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
        text[n] = 0;
    }
}

static void clear_password(void) {
    volatile char* p = s_password;
    for (size_t i = 0; i < sizeof(s_password); ++i) p[i] = 0;
    s_password_visible = false;
}

static EpdRect network_control_rect(int id) {
    if (id < NETWORK_ROWS) return (EpdRect){UI_MARGIN, 350 + id * 100, ui_content_width(), 88};
    if (id == 6) return ui_row_rect(0, 2, 176, 84);
    if (id == 11) return s_forget_confirm ? ui_row_rect(0, 2, 600, UI_BTN_H) : ui_row_rect(1, 2, 176, 84);
    if (id == 12) return ui_row_rect(1, 2, 600, UI_BTN_H);
    if (id == 10) return ui_bar_rect(0, 1);
    return ui_row_rect(id - 7, 3, 968, 84);
}

static EpdRect password_control_rect(int id) {
    if (id < 40) {
        const int gap = 6;
        int width = (ui_content_width() - 9 * gap) / 10;
        return (EpdRect){UI_MARGIN + (id % 10) * (width + gap), 420 + (id / 10) * 100, width, 88};
    }
    if (id < 44) return ui_row_rect(id - 40, 4, 830, 78);
    return ui_bar_rect(id - 44, 3);
}

static EpdRect saved_control_rect(int id) {
    if (id < 5) return (EpdRect){UI_MARGIN, 240 + id * 112, ui_content_width(), 96};
    if (id == 5) return ui_row_rect(0, 2, 840, 84);
    return ui_bar_rect(0, 1);
}

static const char* keyboard_chars(void) {
    static const char* const keys[] = {
        "1234567890" "qwertyuiop" "asdfghjkl-" "zxcvbnm,./",
        "1234567890" "QWERTYUIOP" "ASDFGHJKL-" "ZXCVBNM,./",
        "!\"#$%&'()*" "+,-./:;<=>" "?@[\\]^_`{|" "}~01234567",
    };
    return keys[s_keyboard_mode];
}

static void provisioning_button(uint8_t* fb, EpdRect rect, const char* text, int id) {
    if (s_pressed == id) ui_draw_pressed_round_rect(fb, rect, UI_BTN_RADIUS);
    ui_draw_button(fb, rect, text, false);
}

static void draw_networks(uint8_t* fb) {
    ui_clear_page(fb);
    if (s_view == TRANSFER_SAVED) {
        ui_draw_header(fb, "已保存 WiFi", "点按单个网络可遗忘，最多保存 5 组");
        for (size_t i = 0; i < s_saved_count; ++i) {
            EpdRect rect = saved_control_rect((int)i);
            ui_draw_button(fb, rect, s_saved_ssids[i], s_pressed == (int)i);
        }
        if (!s_saved_count) ui_text(fb, UI_MARGIN, 320, UI_PX_BODY, "尚未保存网络", EPD_DRAW_ALIGN_LEFT, false);
        provisioning_button(fb, saved_control_rect(5), "遗忘全部", 5);
        provisioning_button(fb, saved_control_rect(6), "返回选网", 6);
        return;
    }
    if (s_forget_confirm) {
        ui_draw_header(fb, "遗忘已保存网络？", "仅清除 WiFi 连接信息，不影响图书");
        char name[33];
        snprintf(name, sizeof(name), "%s", s_saved_ssid);
        fit_label(name, UI_PX_BODY, ui_content_width());
        ui_text(fb, UI_MARGIN, 350, UI_PX_BODY, name, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, UI_MARGIN, 450, UI_PX_CAPTION, s_network_message, EPD_DRAW_ALIGN_LEFT, false);
        provisioning_button(fb, network_control_rect(11), "确认遗忘", 11);
        provisioning_button(fb, network_control_rect(12), "取消", 12);
        return;
    }
    ui_draw_header(fb, "选择 WiFi", "选择 2.4 GHz 网络，连接后同网传书");
    if (s_saved_configured) {
        provisioning_button(fb, network_control_rect(6), "连接已保存", 6);
        provisioning_button(fb, network_control_rect(11), "遗忘网络", 11);
        char saved[64];
        snprintf(saved, sizeof(saved), "已保存网络：%s", s_saved_ssid);
        int name_px = UI_PX_CAPTION;
        while (name_px > 12 && ttf_text_width_px(name_px, saved) > ui_content_width()) --name_px;
        ui_text(fb, UI_MARGIN, 270, name_px, saved, EPD_DRAW_ALIGN_LEFT, false);
    } else ui_text(fb, UI_MARGIN, 196, UI_PX_CAPTION, "尚未保存 WiFi，点下方网络配网", EPD_DRAW_ALIGN_LEFT, false);
    char count[64];
    snprintf(count, sizeof(count), "%u 个网络 · 第 %u/%u 页", (unsigned)s_network_count,
             (unsigned)s_network_page + 1, (unsigned)(s_network_count ? (s_network_count + NETWORK_ROWS - 1) / NETWORK_ROWS : 1));
    ui_text(fb, UI_MARGIN, 308, UI_PX_CAPTION,
            s_scan_pending ? "正在扫描，请稍候…" : s_network_message[0] ? s_network_message : count,
            EPD_DRAW_ALIGN_LEFT, false);
    EpdRect manage = {UI_MARGIN + ui_content_width() / 2, 292, ui_content_width() / 2, 48};
    provisioning_button(fb, manage, "管理已保存网络", 13);
    for (int row = 0; row < NETWORK_ROWS; ++row) {
        size_t index = s_network_page * NETWORK_ROWS + row;
        if (index >= s_network_count) break;
        const read_pico_transfer_network_t* network = &s_networks[index];
        EpdRect rect = network_control_rect(row);
        if (s_pressed == row) ui_draw_pressed_round_rect(fb, rect, UI_BTN_RADIUS);
        ui_draw_round_rect(fb, rect, UI_BTN_RADIUS, UI_GRAY_BLACK);
        char name[33], detail[64];
        snprintf(name, sizeof(name), "%s", network->ssid);
        fit_label(name, UI_PX_BTN, rect.width - 2 * UI_PAD);
        ui_text(fb, rect.x + UI_PAD, rect.y + 8, UI_PX_BTN, name, EPD_DRAW_ALIGN_LEFT, false);
        snprintf(detail, sizeof(detail), "%d dBm · %s", network->rssi,
                 !network->supported ? "暂不支持" : network->requires_password ? "需要密码" : "开放网络");
        ui_text(fb, rect.x + UI_PAD, rect.y + 50, UI_PX_CAPTION, detail, EPD_DRAW_ALIGN_LEFT, false);
    }
    provisioning_button(fb, network_control_rect(7), "上一页", 7);
    provisioning_button(fb, network_control_rect(8), "重新扫描", 8);
    provisioning_button(fb, network_control_rect(9), "下一页", 9);
    provisioning_button(fb, network_control_rect(10), "返回传书", 10);
}

static void draw_password(uint8_t* fb) {
    ui_clear_page(fb);
    ui_draw_header(fb, "WiFi 密码", "输入完成后点连接，密码将保存在设备");
    char name[33];
    snprintf(name, sizeof(name), "%s", s_selected.ssid);
    fit_label(name, UI_PX_BODY, ui_content_width());
    ui_text(fb, UI_MARGIN, 176, UI_PX_BODY, name, EPD_DRAW_ALIGN_LEFT, false);
    char caption[80];
    size_t len = strlen(s_password);
    snprintf(caption, sizeof(caption), "%s · %u/%u", s_selected.requires_password ? "密码" : "开放网络，无需密码", (unsigned)len, PASSWORD_MAX);
    ui_text(fb, UI_MARGIN, 240, UI_PX_CAPTION, caption, EPD_DRAW_ALIGN_LEFT, false);
    EpdRect field = {UI_MARGIN, 288, ui_content_width(), 86};
    ui_draw_round_rect(fb, field, UI_BTN_RADIUS, UI_GRAY_BLACK);
    char shown[PASSWORD_MAX + 1];
    if (s_password_visible) memcpy(shown, s_password, len + 1);
    else { memset(shown, '*', len); shown[len] = 0; }
    const char* tail = shown;
    while (*tail && ttf_text_width_px(UI_PX_BODY, tail) > field.width - 2 * UI_PAD) ++tail;
    ui_text_vc(fb, field.x + UI_PAD, field.y + field.height / 2, UI_PX_BODY, tail, EPD_DRAW_ALIGN_LEFT, false);
    const char* keys = keyboard_chars();
    for (int i = 0; i < 40; ++i) {
        char label[2] = {keys[i], 0};
        provisioning_button(fb, password_control_rect(i), label, i);
    }
    provisioning_button(fb, password_control_rect(40), s_keyboard_mode == 1 ? "abc" : "ABC", 40);
    provisioning_button(fb, password_control_rect(41), s_keyboard_mode == 2 ? "字母" : "#+=", 41);
    provisioning_button(fb, password_control_rect(42), "空格", 42);
    provisioning_button(fb, password_control_rect(43), "退格", 43);
    ui_text(fb, UI_MARGIN, 940, UI_PX_CAPTION,
            s_network_message[0] ? s_network_message : "支持字母、数字、符号和空格", EPD_DRAW_ALIGN_LEFT, false);
    provisioning_button(fb, password_control_rect(44), "取消", 44);
    provisioning_button(fb, password_control_rect(45), s_password_visible ? "隐藏" : "显示", 45);
    provisioning_button(fb, password_control_rect(46), "连接", 46);
}

static void queue_network_start(read_pico_transfer_mode_t mode) {
    s_mode = mode;
    s_qr_url = mode == READ_PICO_TRANSFER_MODE_STA;
    clear_qr();
    s_view = TRANSFER_HOME;
    s_forget_confirm = false;
    s_scan_pending = false;
    s_pressed = -1;
    clear_password();
    memset(&s_status, 0, sizeof(s_status));
    s_status.mode = s_mode;
    read_pico_transfer_get_saved_wifi(s_status.wifi_ssid, &s_status.wifi_configured);
    s_start_pending = true;
}

static bool enter_networks(void) {
    if (!read_pico_transfer_try_stop_if_idle()) return false;
    stop_session();
    s_start_pending = false;
    s_view = TRANSFER_NETWORKS;
    s_forget_confirm = false;
    s_network_count = s_network_page = 0;
    s_scan_pending = true;
    s_network_message[0] = 0;
    s_saved_ssid[0] = 0;
    s_saved_configured = false;
    read_pico_transfer_get_saved_wifi(s_saved_ssid, &s_saved_configured);
    (void)read_pico_transfer_get_saved_wifi_list(s_saved_ssids, &s_saved_count);
    clear_password();
    return true;
}

// 提示页先呈现，下一轮才同步扫描；输入页不轮询已停服的传书状态。
// Present the scan notice first, scan synchronously next tick, and do not poll stopped transfer state in the editor.
static app_redraw_t network_ui_tick(app_ctx_t* ctx) {
    if (s_view != TRANSFER_NETWORKS || s_forget_confirm || !s_scan_pending || ctx->consumed) return APP_REDRAW_NONE;
    s_scan_pending = false;
    s_network_count = s_network_page = 0;
    // 进入传书页时已关闭 BLE（独占射频），此处直接扫描。
    // BLE is already off for the whole page (radio owned by Wi-Fi), so scan directly.
    esp_err_t err = read_pico_transfer_scan_wifi(s_networks, &s_network_count);
    if (err != ESP_OK) {
        s_network_count = 0;
        snprintf(s_network_message, sizeof(s_network_message), "扫描失败(%04X)，请重扫",
                 (unsigned)(err & 0xFFFF));
    }
    else if (!s_network_count) snprintf(s_network_message, sizeof(s_network_message), "未发现网络，请靠近路由器后重扫");
    else s_network_message[0] = 0;
    s_pressed = -1;
    return APP_REDRAW_PAGE;
}

static int provisioning_hit(uint16_t x, uint16_t y) {
    if (s_forget_confirm) {
        for (int i = 11; i <= 12; ++i) if (ui_rect_hit(network_control_rect(i), x, y)) return i;
        return -1;
    }
    if (s_view == TRANSFER_PASSWORD) {
        for (int i = 0; i < 47; ++i) if (ui_rect_hit(password_control_rect(i), x, y)) return i;
    } else if (s_view == TRANSFER_SAVED) {
        for (int i = 0; i < (int)s_saved_count; ++i) if (ui_rect_hit(saved_control_rect(i), x, y)) return i;
        if (ui_rect_hit(saved_control_rect(5), x, y)) return 5;
        if (ui_rect_hit(saved_control_rect(6), x, y)) return 6;
    } else {
        EpdRect manage = {UI_MARGIN + ui_content_width() / 2, 292, ui_content_width() / 2, 48};
        if (ui_rect_hit(manage, x, y)) return 13;
        for (int i = 0; i < 12; ++i) {
            if (i < NETWORK_ROWS && (s_scan_pending || s_network_page * NETWORK_ROWS + i >= s_network_count)) continue;
            if ((i == 6 || i == 11) && !s_saved_configured) continue;
            if (ui_rect_hit(network_control_rect(i), x, y)) return i;
        }
    }
    return -1;
}

static app_redraw_t provisioning_action(int id) {
    if (s_forget_confirm) {
        if (id == 12) { s_forget_confirm = false; s_network_message[0] = 0; }
        else if (id == 11) {
            esp_err_t err = read_pico_transfer_forget_wifi();
            if (err == ESP_OK) {
                s_forget_confirm = false;
                s_saved_configured = false;
                s_saved_ssid[0] = 0;
                snprintf(s_network_message, sizeof(s_network_message), "已遗忘网络");
            } else snprintf(s_network_message, sizeof(s_network_message), "遗忘失败，请重试或取消");
        }
        return APP_REDRAW_PAGE;
    }
    if (s_view == TRANSFER_SAVED) {
        if (id < 5 && (size_t)id < s_saved_count) {
            esp_err_t err = read_pico_transfer_forget_wifi_ssid(s_saved_ssids[id]);
            if (err == ESP_OK) {
                snprintf(s_network_message, sizeof(s_network_message), "已遗忘：%.64s", s_saved_ssids[id]);
                (void)read_pico_transfer_get_saved_wifi_list(s_saved_ssids, &s_saved_count);
                (void)read_pico_transfer_get_saved_wifi(s_saved_ssid, &s_saved_configured);
            } else snprintf(s_network_message, sizeof(s_network_message), "遗忘失败，请重试");
        } else if (id == 5) {
            if (read_pico_transfer_forget_wifi() == ESP_OK) {
                s_saved_count = 0; s_saved_configured = false; s_saved_ssid[0] = 0;
                memset(s_saved_ssids, 0, sizeof(s_saved_ssids));
                snprintf(s_network_message, sizeof(s_network_message), "已遗忘全部网络");
            }
        } else if (id == 6) { s_view = TRANSFER_NETWORKS; s_scan_pending = true; }
        return APP_REDRAW_PAGE;
    }
    if (s_view == TRANSFER_NETWORKS) {
        if (id < NETWORK_ROWS) {
            s_selected = s_networks[s_network_page * NETWORK_ROWS + id];
            if (!s_selected.supported) {
                snprintf(s_network_message, sizeof(s_network_message), "此网络认证暂不支持，请选择其他网络");
                return APP_REDRAW_PAGE;
            }
            clear_password();
            s_network_message[0] = 0;
            s_keyboard_mode = 0;
            s_view = TRANSFER_PASSWORD;
        } else if (id == 6) queue_network_start(READ_PICO_TRANSFER_MODE_STA);
        else if (id == 7 && s_network_page) --s_network_page;
        else if (id == 8) { s_scan_pending = true; s_network_message[0] = 0; }
        else if (id == 9 && (s_network_page + 1) * NETWORK_ROWS < s_network_count) ++s_network_page;
        else if (id == 10) queue_network_start(s_mode);
        else if (id == 11) { s_forget_confirm = true; s_network_message[0] = 0; }
        else if (id == 13) { (void)read_pico_transfer_get_saved_wifi_list(s_saved_ssids, &s_saved_count); s_view = TRANSFER_SAVED; }
        return APP_REDRAW_PAGE;
    }
    size_t len = strlen(s_password);
    if (id < 40 || id == 42) {
        if (len < PASSWORD_MAX) {
            s_password[len] = id == 42 ? ' ' : keyboard_chars()[id];
            s_password[len + 1] = 0;
            s_network_message[0] = 0;
        } else snprintf(s_network_message, sizeof(s_network_message), "密码最多 64 个字符");
    } else if (id == 40) s_keyboard_mode = s_keyboard_mode == 1 ? 0 : 1;
    else if (id == 41) s_keyboard_mode = s_keyboard_mode == 2 ? 0 : 2;
    else if (id == 43) { if (len) s_password[len - 1] = 0; s_network_message[0] = 0; }
    else if (id == 44) { clear_password(); s_network_message[0] = 0; s_view = TRANSFER_NETWORKS; return APP_REDRAW_PAGE; }
    else if (id == 45) s_password_visible = !s_password_visible;
    else if (id == 46) {
        if (s_selected.requires_password && len < 8) {
            snprintf(s_network_message, sizeof(s_network_message), "加密网络密码至少 8 个字符");
        } else {
            esp_err_t err = read_pico_transfer_save_wifi(s_selected.ssid,
                s_selected.requires_password ? s_password : "");
            if (err == ESP_OK) { queue_network_start(READ_PICO_TRANSFER_MODE_STA); return APP_REDRAW_PAGE; }
            snprintf(s_network_message, sizeof(s_network_message), "%s", err == ESP_ERR_INVALID_ARG ?
                     "密码须 8-63 字符或 64 位十六进制" : "保存失败，输入已保留，请重试");
        }
    }
    return APP_REDRAW_AREA;
}

static app_redraw_t provisioning_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    int old = s_pressed;
    int origin = provisioning_hit(ev->x0, ev->y0);
    s_pressed = ev->type == UI_GESTURE_PRESS ? origin : -1;
    bool action = ev->type == UI_GESTURE_TAP && old >= 0 && old == origin && provisioning_hit(ev->x, ev->y) == origin;
    EpdRect old_rect = s_view == TRANSFER_PASSWORD ? password_control_rect(old >= 0 ? old : origin >= 0 ? origin : 0)
        : s_view == TRANSFER_SAVED ? saved_control_rect(old >= 0 ? old : origin >= 0 ? origin : 0)
        : network_control_rect(old >= 0 ? old : origin >= 0 ? origin : 0);
    if (action) {
        app_redraw_t redraw = provisioning_action(origin);
        if (redraw == APP_REDRAW_PAGE) return redraw;
        render(ctx, ctx->fb);
        s_area = ui_rect_union(old_rect, (EpdRect){UI_MARGIN, 240, ui_content_width(), 744});
        s_settle = false;
        return APP_REDRAW_AREA;
    }
    if (old == s_pressed) return APP_REDRAW_NONE;
    render(ctx, ctx->fb);
    s_area = old_rect;
    s_settle = false;
    return APP_REDRAW_AREA;
}

static EpdRect status_rect(void) { return (EpdRect){UI_MARGIN, 610, ui_content_width(), 460}; }
static uint64_t free_bytes(void* arg) { return book_store_free_bytes(arg); }

static void draw_capacity(uint8_t* fb) {
    char capacity[96];
    ui_clear_rect_fast(fb, (EpdRect){UI_MARGIN, 518, ui_content_width(), 42});
    if (s_media_lost) snprintf(capacity, sizeof(capacity), "TF 卡已移除，原存储已停用");
    else snprintf(capacity, sizeof(capacity), "%s · 余 %.1f MB%s%s", s_root.is_flash ? "内置" : "TF 卡", s_free / 1048576.0,
                  s_root.is_flash ? " · 单本≤1MB" : "", s_can_switch_root ? " · 点按切换" : "");
    if (!s_root_message[0] && s_can_switch_root && !s_media_lost)
        ui_draw_button(fb, (EpdRect){UI_MARGIN, 518, ui_content_width(), 42}, capacity, false);
    else
        ui_text(fb, UI_MARGIN, 528, UI_PX_CAPTION, s_root_message[0] ? s_root_message : capacity, EPD_DRAW_ALIGN_LEFT, false);
}

static void draw_status(uint8_t* fb) {
    EpdRect area = status_rect();
    ui_clear_rect_fast(fb, area);
    const char* state = "网络已停止";
    if (s_start_pending || s_status.state == READ_PICO_TRANSFER_STARTING) state = "正在准备传书…";
    else if (s_status.state == READ_PICO_TRANSFER_READY) state = "等待上传";
    else if (s_status.state == READ_PICO_TRANSFER_UPLOADING) state = "正在接收";
    else if (s_status.state == READ_PICO_TRANSFER_ERROR) state = s_status.network_ready ? "上传失败，请重试" : "连接失败，可重试或切回热点";
    if (s_media_lost) state = "存储已移除，传书已停止";
    ui_text(fb, area.x, area.y, UI_PX_BODY, state, EPD_DRAW_ALIGN_LEFT, false);
    char line[160];
    if (s_media_lost) snprintf(line, sizeof(line), "重新进入传书可使用内置存储");
    else if (s_mode == READ_PICO_TRANSFER_MODE_AP)
        snprintf(line, sizeof(line), "已连接 %u 台 · 已完成 %u 个文件", s_status.sta_count, s_status.done_count);
    else snprintf(line, sizeof(line), "同网浏览器上传 · 已完成 %u 个文件", s_status.done_count);
    ui_text(fb, area.x, area.y + 64, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "%s", s_status.cur_name);
    while (*line && ttf_text_width_px(UI_PX_CAPTION, line) > area.width) {
        size_t n = strlen(line) - 1;
        while (n && ((unsigned char)line[n] & 0xc0) == 0x80) --n;
        line[n] = 0;
    }
    ui_text(fb, area.x, area.y + 116, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
    snprintf(line, sizeof(line), "%.1f / %.1f MB", s_status.cur_bytes / 1048576.0, s_status.cur_total / 1048576.0);
    ui_text(fb, area.x, area.y + 164, UI_PX_BODY, line, EPD_DRAW_ALIGN_LEFT, false);
    EpdRect track = {area.x, area.y + 224, area.width, 12};
    epd_fill_rect(track, UI_GRAY_LIGHT, fb);
    if (s_status.cur_total) {
        uint64_t width = (uint64_t)s_status.cur_bytes * track.width / s_status.cur_total;
        track.width = width > (uint64_t)track.width ? track.width : (int)width;
        epd_fill_rect(track, UI_GRAY_BLACK, fb);
    }
    if (s_status.last_error != ESP_OK)
        ui_text(fb, area.x, area.y + 260, UI_PX_CAPTION, s_status.network_ready ? "上传未完成，请在浏览器查看原因" : "检查WiFi配置后重新选择接入方式", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, area.x, area.y + 328, UI_PX_CAPTION, s_status.state == READ_PICO_TRANSFER_UPLOADING ? "上传完成后可切换接入方式" : "离开本页会断开网络", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, area.x, area.y + 370, UI_PX_CAPTION, "同名文件上传后将覆盖", EPD_DRAW_ALIGN_LEFT, false);
    if (s_status.pin[0] && !s_media_lost) {
        char pin_line[64];
        snprintf(pin_line, sizeof(pin_line), "配对码：%s%s", s_status.pin,
                 s_status.pin_required ? " · 已开启校验" : "");
        ui_text(fb, area.x, area.y + 412, UI_PX_CAPTION, pin_line, EPD_DRAW_ALIGN_LEFT, false);
    }
}

static EpdRect control_rect(int id) {
    if (id == 3) return (EpdRect){UI_MARGIN, 454, 350, 60};
    if (id == 4) return (EpdRect){UI_MARGIN, 518, ui_content_width(), 42};
    return id == 2 ? ui_bar_rect(0, 1) : ui_row_rect(id, 2, UI_CONTENT_TOP, UI_BTN_H);
}

static void render(app_ctx_t* ctx, uint8_t* fb) {
    if (s_view == TRANSFER_NETWORKS || s_view == TRANSFER_SAVED) { draw_networks(fb); return; }
    if (s_view == TRANSFER_PASSWORD) { draw_password(fb); return; }
    (void)ctx;
    ui_clear_page(fb);
    ui_draw_header(fb, "传书", "浏览器上传图书或 TTF 字体");
    for (int i = 0; i < 2; ++i) {
        EpdRect r = control_rect(i);
        if (s_pressed == i) ui_draw_pressed_round_rect(fb, r, UI_BTN_RADIUS);
        ui_draw_button(fb, r, i == 0 ? "设备热点" : "已有 WiFi", (int)s_mode == i);
    }
    char name[64];
    snprintf(name, sizeof(name), "%s", s_mode == READ_PICO_TRANSFER_MODE_AP ? s_status.ssid : s_status.wifi_ssid);
    int name_px = UI_PX_CAPTION;
    int name_width = 350;
    while (name_px > 12 && ttf_text_width_px(name_px, name) > name_width) --name_px;
    ui_text(fb, UI_MARGIN, 274, name_px, name, EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, 326, UI_PX_CAPTION,
            s_mode == READ_PICO_TRANSFER_MODE_AP ? "口令：" READ_PICO_TRANSFER_PASSWORD :
            s_status.network_ready ? "设备已连接；手机需同网" :
            s_start_pending || s_status.state == READ_PICO_TRANSFER_STARTING ? "正在连接已保存网络…" :
            s_status.wifi_configured ? "已保存，尚未连接" : "点已有 WiFi 扫描选网",
            EPD_DRAW_ALIGN_LEFT, false);
    int url_px = UI_PX_CAPTION;
    while (url_px > 12 && ttf_text_width_px(url_px, s_status.url) > 350) --url_px;
    ui_text(fb, UI_MARGIN, 370, url_px,
            s_status.network_ready ? s_status.url : "等待网络地址…", EPD_DRAW_ALIGN_LEFT, false);
    if (s_status.network_ready && s_mode == READ_PICO_TRANSFER_MODE_STA) {
        char local_url[96];
        snprintf(local_url, sizeof(local_url), "http://%s.local", s_status.mdns_host[0] ? s_status.mdns_host : "readpico");
        ui_text(fb, UI_MARGIN, 402, 24, local_url, EPD_DRAW_ALIGN_LEFT, false);
    }
    ui_text(fb, UI_MARGIN,
            (s_status.network_ready && s_mode == READ_PICO_TRANSFER_MODE_STA) ? 434 : 412, 24,
            !s_status.network_ready ? "等待网络连接…" : !s_qr_ready ? "二维码失败，请手输网址" :
            s_qr_url || s_mode == READ_PICO_TRANSFER_MODE_STA ? "扫码打开网页" : "扫码连接 WiFi",
            EPD_DRAW_ALIGN_LEFT, false);
    if (s_mode == READ_PICO_TRANSFER_MODE_AP && s_status.network_ready) {
        EpdRect toggle = control_rect(3);
        if (s_pressed == 3) ui_draw_pressed_round_rect(fb, toggle, UI_BTN_RADIUS);
        ui_draw_button(fb, toggle, s_qr_url ? "切换为 WiFi 码" : "切换为网页码", false);
    }
    if (s_status.network_ready && s_qr_ready)
        ui_wifi_qr_draw(fb, (EpdRect){404, 274, 240, 240});
    draw_capacity(fb);
    ui_hairline(fb, 578, UI_MARGIN, ui_content_width(), UI_GRAY_BLACK);
    draw_status(fb);
    EpdRect back = control_rect(2);
    if (s_pressed == 2) ui_draw_pressed_round_rect(fb, back, UI_BTN_RADIUS);
    ui_draw_button(fb, back, "停止并返回", false);
}

static void on_enter(app_ctx_t* ctx) {
    (void)ctx;
    // 先关 BLE 再碰 Wi-Fi：两者共存会耗尽内部内存。
    // Stop BLE before touching Wi-Fi: coexistence exhausts internal RAM.
    app_ble_wifi_hold(true);
    // 先关闭卡上字库，再开放替换；主循环的字体重试也会被暂停。
    // Close the card font before allowing replacement; event-loop font retries are suspended too.
    s_font_error = ttf_font_suspend_sd(true);
    display_set_bulk_io(true);
    s_media_lost = false;
    memset(&s_status, 0, sizeof(s_status));
    s_qr_url = s_mode == READ_PICO_TRANSFER_MODE_STA;
    clear_qr();
    memset(&s_root, 0, sizeof(s_root));
    s_heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    s_internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_start_pending = true;
    s_view = TRANSFER_HOME;
    s_forget_confirm = false;
    s_scan_pending = false;
    clear_password();
    s_session_started = false;
    s_pressed = -1;
    s_settle = s_any_changed = false;
    s_status.mode = s_mode;
    read_pico_transfer_get_saved_wifi(s_status.wifi_ssid, &s_status.wifi_configured);
    s_poll_ms = 0;
    s_free = 0;
    s_root_message[0] = 0;
    s_can_switch_root = false;
    memset(&s_storage_info, 0, sizeof(s_storage_info));
    atomic_store(&s_settings_runtime_dirty, false);
    atomic_store(&s_remote_switch_pending, false);
    atomic_store(&s_pending_time_sync, false);
    atomic_store(&s_pending_focus_ctrl, false);
    atomic_store(&s_remote_stop_pending, false);
    s_remote_stop_at_ms = 0;
    s_pmu_poll_ms = 0;
    read_pico_transfer_reset_pin();
    if (read_pico_pmu_ready()) (void)read_pico_pmu_poll();
}

static void stop_session(void) {
    clear_qr();
    // 停服后读最终计数，保留本页跨模式文件变更的记录。
    // Join reception before reading counts and retain file changes across mode switches.
    read_pico_transfer_stop();
    read_pico_transfer_get_status(&s_status);
    if (s_session_started && s_status.changed_count) s_any_changed = true;
    s_session_started = false;
}

// 必须先等待 HTTP 退出，再清除旧根；不会让在途请求写到另一存储。
// Join HTTP before clearing the old root; in-flight requests never move to another storage.
static void transfer_on_media_lost(app_ctx_t* ctx) {
    (void)ctx;
    if (s_root.is_flash) {
        bool had_sd = s_storage_info.sd_available;
        s_storage_info.sd_available = false;
        s_storage_info.sd_free_bytes = 0;
        s_can_switch_root = false;
        if (had_sd) {
            // 停服汇合可能正在访问 TF 卡（字体/锁屏/便签）的请求，随后以纯内置存储重启。
            // Join any in-flight TF-card request (fonts/screensaver/sketches), then restart flash-only.
            stop_session();
            s_start_pending = true;
        }
        return;
    }
    stop_session();
    s_start_pending = s_scan_pending = false;
    atomic_store(&s_remote_switch_pending, false);
    s_view = TRANSFER_HOME;
    clear_password();
    s_pressed = -1;
    s_free = 0;
    s_can_switch_root = false;
    memset(&s_storage_info, 0, sizeof(s_storage_info));
    memset(&s_root, 0, sizeof(s_root));
    s_media_lost = true;
    s_settle = false;
}

static void transfer_on_exit(app_ctx_t* ctx) {
    clear_password();
    s_scan_pending = false;
    atomic_store(&s_remote_switch_pending, false);
    atomic_store(&s_remote_stop_pending, false);
    s_remote_stop_at_ms = 0;
    stop_session();
    if (atomic_exchange(&s_settings_runtime_dirty, false)) app_settings_apply_runtime();
    if (atomic_exchange(&s_pending_time_sync, false))
        (void)app_time_set_utc(atomic_load(&s_pending_time_utc), NULL);
    ttf_font_suspend_sd(false);
    display_set_bulk_io(false);
    app_ble_wifi_hold(false);
    guard_draw_result(ctx->hl, update_display_white(ctx->hl));
    if (s_any_changed) book_store_notify_changed();
    ESP_LOGI(TAG, "heap before=%u after=%u internal_before=%u internal_after=%u",
             (unsigned)s_heap_before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)s_internal_before, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static bool switch_upload_root(bool prefer_flash, bool from_remote) {
    book_store_root_t target = {0};
    esp_err_t err = book_store_upload_root_for(prefer_flash, &target);
    if (err != ESP_OK) {
        if (!from_remote)
            snprintf(s_root_message, sizeof(s_root_message), "%s存储当前不可用，未切换", prefer_flash ? "内置" : "TF 卡");
        return false;
    }
    if (!read_pico_transfer_try_stop_if_idle()) {
        if (!from_remote)
            snprintf(s_root_message, sizeof(s_root_message), "上传进行中，请完成后再切换目标");
        return false;
    }
    stop_session();
    s_upload_flash = target.is_flash;
    s_root = target;
    s_free = book_store_free_bytes(&s_root);
    s_root_message[0] = 0;
    s_start_pending = true;
    s_can_switch_root = true;
    return true;
}

static app_redraw_t on_tick(app_ctx_t* ctx) {
    if (atomic_exchange(&s_settings_runtime_dirty, false)) app_settings_apply_runtime();
    if (atomic_exchange(&s_pending_time_sync, false))
        (void)app_time_set_utc(atomic_load(&s_pending_time_utc), NULL);
    if (s_view != TRANSFER_HOME) return network_ui_tick(ctx);
    if (atomic_exchange(&s_pending_focus_ctrl, false)) {
        if (atomic_load(&s_pending_focus_start)) {
            int mins = atomic_load(&s_pending_focus_min);
            if (app_focus_remote_start(mins) && ctx) {
                ctx->request_app = &app_focus;
                return APP_REDRAW_NONE;
            }
        } else {
            app_focus_remote_stop();
        }
    }
    char pushed_url[READ_PICO_WEBOPEN_URL_MAX];
    if (!s_start_pending && read_pico_transfer_take_webopen_url(pushed_url)) {
        app_web_queue_url(pushed_url);
        if (ctx) ctx->request_app = &app_web;
        return APP_REDRAW_NONE;
    }
    if (!s_start_pending && atomic_load(&s_remote_stop_pending)) {
        if (!s_remote_stop_at_ms) {
            // 留出 120 ms 让 HTTP 200 响应包发给客户端，再停服离页并恢复 BLE。
            // Wait 120 ms so the HTTP 200 response flushes before tearing down Wi-Fi and resuming BLE.
            s_remote_stop_at_ms = ctx ? ctx->now_ms + 120 : 1;
        } else if (!ctx || ctx->now_ms >= s_remote_stop_at_ms) {
            atomic_store(&s_remote_stop_pending, false);
            s_remote_stop_at_ms = 0;
            if (app_transfer_remote_stop(ctx)) return APP_REDRAW_NONE;
        }
    }
    if (!s_start_pending && atomic_load(&s_remote_switch_pending)) {
        bool prefer_flash = atomic_load(&s_remote_switch_flash);
        if (switch_upload_root(prefer_flash, true)) {
            atomic_store(&s_remote_switch_pending, false);
            return APP_REDRAW_PAGE;
        }
        if (prefer_flash ? !s_storage_info.flash_available : !s_storage_info.sd_available)
            atomic_store(&s_remote_switch_pending, false);
    }
    if (s_start_pending) {
        s_start_pending = false;
        if (s_font_error != ESP_OK) s_font_error = ttf_font_suspend_sd(true);
        esp_err_t err = s_font_error == ESP_OK ? book_store_upload_root_for(s_upload_flash, &s_root) : s_font_error;
        if (err == ESP_OK) {
            s_media_lost = false;
            s_free = book_store_free_bytes(&s_root);
            book_store_root_t roots[2]; int root_count = 0;
            memset(&s_storage_info, 0, sizeof(s_storage_info));
            if (book_store_roots(roots, &root_count) == ESP_OK) {
                s_can_switch_root = false;
                for (int i = 0; i < root_count; ++i) {
                    uint64_t free_b = book_store_free_bytes(&roots[i]);
                    if (roots[i].is_flash) {
                        s_storage_info.flash_available = true;
                        s_storage_info.flash_free_bytes = free_b;
                    } else {
                        s_storage_info.sd_available = true;
                        s_storage_info.sd_free_bytes = free_b;
                    }
                    if (roots[i].is_flash != s_root.is_flash) s_can_switch_root = true;
                }
            }
            read_pico_transfer_cfg_t cfg = {.mode = s_mode, .root_dir = s_root.path, .is_flash = s_root.is_flash,
                .font_dir = s_storage_info.sd_available ? "/sdcard/fonts" : NULL,
                .file_limit = book_store_file_limit(&s_root), .free_bytes_cb = free_bytes, .free_bytes_ctx = &s_root, .file_changed_cb = book_store_forget_changed_path,
                .sta_ip_cb = app_time_sync_sta_ip, .service_poll_cb = app_time_sync_poll,
                .network_stop_cb = app_time_sync_stop,
                .sd_root = s_storage_info.sd_available ? "/sdcard" : NULL,
                .storage_info_cb = transfer_storage_info, .storage_switch_cb = transfer_storage_switch,
                .settings_get_cb = transfer_settings_get, .settings_set_cb = transfer_settings_set,
                .pin_required = app_settings_transfer_pin_required(),
                .time_get_cb = transfer_time_get, .time_set_cb = transfer_time_set,
                .font_select_cb = transfer_font_select,
                .dashboard_stats_cb = transfer_dashboard_stats,
                .dashboard_stats_set_cb = transfer_dashboard_stats_set,
                .book_progress_cb = transfer_book_progress,
                .book_progress_set_cb = transfer_book_progress_set,
                .book_notes_set_cb = transfer_book_notes_set,
                .webmarks_list_cb = transfer_webmarks_list,
                .webmarks_upsert_cb = transfer_webmarks_upsert,
                .webmarks_delete_cb = transfer_webmarks_delete,
                .feeds_list_cb = transfer_feeds_list,
                .feeds_upsert_cb = transfer_feeds_upsert,
                .feeds_delete_cb = transfer_feeds_delete,
                .focus_get_cb = transfer_focus_get,
                .focus_ctrl_cb = transfer_focus_ctrl,
                .stop_request_cb = transfer_stop_request};
            err = read_pico_transfer_start(&cfg);
            s_session_started = err == ESP_OK;
            read_pico_transfer_get_status(&s_status);
            if (err == ESP_OK) s_free = book_store_free_bytes(&s_root);
        }
        s_status.mode = s_mode;
        if (err != ESP_OK) { s_status.state = READ_PICO_TRANSFER_ERROR; s_status.last_error = err; }
        if (err == ESP_OK) prepare_qr();
        else clear_qr();
        ESP_LOGI(TAG, "start root=%s result=%s", s_root.path, esp_err_to_name(err));
        return APP_REDRAW_PAGE;
    }
    if (ctx->consumed || ctx->now_ms - s_poll_ms < 2000) return APP_REDRAW_NONE;
    s_poll_ms = ctx->now_ms;
    if (read_pico_pmu_ready() && ctx->now_ms - s_pmu_poll_ms >= 10000) {
        s_pmu_poll_ms = ctx->now_ms;
        (void)read_pico_pmu_poll();
    }
    read_pico_transfer_service_poll();
    read_pico_transfer_status_t next = {0};
    read_pico_transfer_get_status(&next);
    if (s_status.state == READ_PICO_TRANSFER_ERROR && next.state == READ_PICO_TRANSFER_STOPPED) return APP_REDRAW_NONE;
    bool network_changed = next.network_ready != s_status.network_ready ||
        next.mode != s_status.mode || strcmp(next.ssid, s_status.ssid) ||
        next.wifi_configured != s_status.wifi_configured || strcmp(next.url, s_status.url) ||
        strcmp(next.wifi_ssid, s_status.wifi_ssid);
    if (!network_changed && next.state == s_status.state && next.sta_count == s_status.sta_count &&
        next.cur_bytes == s_status.cur_bytes && next.cur_total == s_status.cur_total &&
        next.done_count == s_status.done_count && next.changed_count == s_status.changed_count && next.last_error == s_status.last_error &&
        next.pin_required == s_status.pin_required && !strcmp(next.pin, s_status.pin) &&
        !strcmp(next.cur_name, s_status.cur_name)) return APP_REDRAW_NONE;
    s_settle = next.changed_count != s_status.changed_count || next.done_count != s_status.done_count ||
        (s_status.state == READ_PICO_TRANSFER_UPLOADING && next.state != READ_PICO_TRANSFER_UPLOADING);
    s_status = next;
    if (network_changed) prepare_qr();
    if (s_settle) { s_free = book_store_free_bytes(&s_root); return APP_REDRAW_FULL; }
    if (network_changed) { s_free = book_store_free_bytes(&s_root); return APP_REDRAW_PAGE; }
    draw_status(ctx->fb);
    s_area = status_rect();
    return APP_REDRAW_AREA;
}

static int hit_control(uint16_t x, uint16_t y) {
    if (s_mode == READ_PICO_TRANSFER_MODE_AP && s_status.network_ready && ui_rect_hit(control_rect(3), x, y)) return 3;
    if (s_can_switch_root && !s_media_lost && ui_rect_hit(control_rect(4), x, y)) return 4;
    for (int i = 0; i < 3; ++i) if (ui_rect_hit(control_rect(i), x, y)) return i;
    return -1;
}

static app_redraw_t on_gesture(app_ctx_t* ctx, const ui_gesture_event_t* ev) {
    if (s_view != TRANSFER_HOME) return provisioning_gesture(ctx, ev);
    int old = s_pressed;
    int origin = hit_control(ev->x0, ev->y0);
    s_pressed = ev->type == UI_GESTURE_PRESS ? origin : -1;
    if (ev->type == UI_GESTURE_TAP && old >= 0 && old == origin &&
        hit_control(ev->x, ev->y) == origin) {
        if (origin == 3) {
            s_qr_url = !s_qr_url;
            prepare_qr();
            return APP_REDRAW_PAGE;
        } else if (origin == 2) {
            // 即使没有切页历史也先停服，取消尚未执行的启动请求。
            // Stop even without navigation history and cancel a queued start.
            s_start_pending = false;
            stop_session();
            ctx->request_return = true;
        } else if (origin == 1) {
            if (enter_networks()) return APP_REDRAW_PAGE;
        } else if (origin == 4) {
            (void)switch_upload_root(!s_root.is_flash, false);
            return APP_REDRAW_PAGE;
        } else {
            read_pico_transfer_status_t latest;
            read_pico_transfer_get_status(&latest);
            if (((int)s_mode != origin || !latest.network_ready) &&
                read_pico_transfer_try_stop_if_idle()) {
                stop_session();
                s_mode = origin == 0 ? READ_PICO_TRANSFER_MODE_AP : READ_PICO_TRANSFER_MODE_STA;
                s_qr_url = s_mode == READ_PICO_TRANSFER_MODE_STA;
                clear_qr();
                memset(&s_status, 0, sizeof(s_status));
                s_status.mode = s_mode;
                read_pico_transfer_get_saved_wifi(s_status.wifi_ssid, &s_status.wifi_configured);
                s_start_pending = true;
                return APP_REDRAW_PAGE;
            }
        }
    }
    if (s_pressed == old) return APP_REDRAW_NONE;
    render(ctx, ctx->fb);
    s_settle = false;
    s_area = control_rect(old >= 0 ? old : s_pressed);
    return APP_REDRAW_AREA;
}

static bool present(app_ctx_t* ctx, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_FULL && s_settle) {
        render(ctx, ctx->fb);
        ui_key_hints_t hints = { .left = "返回", .center = "刷新 · 长按桌面" };
        ui_draw_key_hints(ctx->fb, &hints);
        guard_draw_result(ctx->hl, update_display_full(ctx->hl));
        s_settle = false;
        return true;
    }
    if (redraw != APP_REDRAW_AREA) return false;
    guard_draw_result(ctx->hl, update_display_area_with(ctx->hl, s_settle ? &E0470_WAVEFORM : &E0470_FOLLOW_WAVEFORM,
                      s_settle ? MODE_GL16 : MODE_DU, s_area));
    s_settle = false;
    return true;
}

void app_transfer_set_preferred_mode(read_pico_transfer_mode_t mode) {
    s_mode = mode == READ_PICO_TRANSFER_MODE_STA ? READ_PICO_TRANSFER_MODE_STA : READ_PICO_TRANSFER_MODE_AP;
}

bool app_transfer_remote_switch_mode(read_pico_transfer_mode_t mode) {
    mode = mode == READ_PICO_TRANSFER_MODE_STA ? READ_PICO_TRANSFER_MODE_STA : READ_PICO_TRANSFER_MODE_AP;
    read_pico_transfer_status_t latest = {0};
    read_pico_transfer_get_status(&latest);
    if (s_view == TRANSFER_HOME && s_mode == mode &&
        (s_start_pending || latest.state == READ_PICO_TRANSFER_STARTING ||
         latest.state == READ_PICO_TRANSFER_READY || latest.state == READ_PICO_TRANSFER_UPLOADING)) {
        return true;
    }
    if (!read_pico_transfer_try_stop_if_idle()) return false;
    stop_session();
    s_view = TRANSFER_HOME;
    s_forget_confirm = false;
    s_scan_pending = false;
    clear_password();
    s_mode = mode;
    s_qr_url = s_mode == READ_PICO_TRANSFER_MODE_STA;
    clear_qr();
    memset(&s_status, 0, sizeof(s_status));
    s_status.mode = s_mode;
    read_pico_transfer_get_saved_wifi(s_status.wifi_ssid, &s_status.wifi_configured);
    s_start_pending = true;
    return true;
}

bool app_transfer_remote_stop(app_ctx_t* ctx) {
    if (!read_pico_transfer_try_stop_if_idle()) return false;
    s_start_pending = false;
    s_scan_pending = false;
    clear_password();
    stop_session();
    if (ctx) ctx->request_return = true;
    return true;
}

static app_redraw_t on_key(app_ctx_t* ctx, int key) {
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    s_pressed = -1;
    if (s_forget_confirm) {
        s_forget_confirm = false;
        s_network_message[0] = 0;
        return APP_REDRAW_PAGE;
    }
    if (s_view == TRANSFER_PASSWORD) {
        clear_password();
        s_network_message[0] = 0;
        s_view = TRANSFER_NETWORKS;
        return APP_REDRAW_PAGE;
    }
    if (s_view == TRANSFER_SAVED) {
        s_view = TRANSFER_NETWORKS;
        s_scan_pending = true;
        return APP_REDRAW_PAGE;
    }
    if (s_view == TRANSFER_NETWORKS) {
        queue_network_start(s_mode);
        return APP_REDRAW_PAGE;
    }
    s_start_pending = false;
    stop_session();
    if (ctx) ctx->request_return = true;
    return APP_REDRAW_NONE;
}

const app_desc_t app_transfer = {
    .on_media_lost = transfer_on_media_lost,
    .title = "传书", .detail = "热点或已有 WiFi，浏览器上传",
    .blocks_idle_sleep = true,
    .on_enter = on_enter, .on_exit = transfer_on_exit, .render = render, .present = present,
    .on_tick = on_tick, .on_gesture = on_gesture, .on_key = on_key,
};
