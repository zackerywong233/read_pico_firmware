/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：配网界面的宿主依赖替身。/ English: Host dependency substitutes for provisioning UI.
 * 冻结：仅供测试。/ Frozen: Tests only.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "read_pico_transfer_companion.h"
#include "read_pico_transfer_network.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
typedef struct { int x,y,width,height; } EpdRect;
typedef enum { APP_REDRAW_NONE, APP_REDRAW_AREA, APP_REDRAW_PAGE, APP_REDRAW_FULL, APP_REDRAW_DONE } app_redraw_t;
struct app_desc;
typedef struct { uint8_t* fb; void* hl; bool consumed; bool key_handled; bool keep_awake; int64_t now_ms; const struct app_desc* request_app; bool request_return; } app_ctx_t;
typedef enum { UI_GESTURE_PRESS, UI_GESTURE_TAP, UI_GESTURE_CANCEL, UI_GESTURE_LONG_PRESS } ui_gesture_type_t;
typedef struct { ui_gesture_type_t type; uint16_t x,y,x0,y0; } ui_gesture_event_t;
typedef struct app_desc { const char *title,*detail; bool enter_full,blocks_idle_sleep; void(*on_enter)(app_ctx_t*); void(*on_exit)(app_ctx_t*); void(*on_media_lost)(app_ctx_t*); void(*render)(app_ctx_t*,uint8_t*); bool(*present)(app_ctx_t*,app_redraw_t); app_redraw_t(*on_tick)(app_ctx_t*); app_redraw_t(*on_gesture)(app_ctx_t*,const ui_gesture_event_t*); app_redraw_t(*on_key)(app_ctx_t*,int); } app_desc_t;
#define UI_KEY_1 0
#define UI_KEY_2 1
#define UI_KEY_3 2
enum EpdDrawMode { MODE_DU,MODE_GL16,MODE_GC16 };
#define UI_MARGIN 40
#define UI_CONTENT_TOP 176
#define UI_BTN_H 84
#define UI_BTN_RADIUS 14
#define UI_PX_CAPTION 28
#define UI_PX_BODY 38
#define UI_PX_BTN 34
#define UI_PX_SUB 30
#define UI_PAD 16
#define UI_GAP 12
#define UI_BAR_TOP 1096
#define UI_LOCK_HEIGHT 1216
#define UI_LOCK_WIDTH 684
#define EPD_DRAW_ALIGN_LEFT 0
#define EPD_DRAW_ALIGN_CENTER 1
#define UI_GRAY_LIGHT 0xcc
#define UI_GRAY_BLACK 0
#define MALLOC_CAP_8BIT 1
#define MALLOC_CAP_INTERNAL 2
static const int E0470_WAVEFORM = 0;
static const int E0470_FOLLOW_WAVEFORM = 1;
#define ESP_LOGI(tag,...) ((void)(tag))
static inline const char* esp_err_to_name(int e) { (void)e; return "error"; }
static inline void app_time_sync_sta_ip(void) {}
static inline void app_time_sync_poll(void) {}
static inline void app_time_sync_stop(void) {}
static uint32_t test_utc_time = 1763724000U;
static int test_time_set_calls;
static inline bool app_time_now_utc(time_t* out) { if (out) *out = (time_t)test_utc_time; return test_utc_time != 0; }
static inline bool app_time_set_utc(uint32_t unix_sec, bool* pmu_synced) {
    test_utc_time = unix_sec;
    ++test_time_set_calls;
    if (pmu_synced) *pmu_synced = true;
    return true;
}
static int test_pmu_poll_calls;
static inline bool read_pico_pmu_ready(void) { return true; }
static inline int read_pico_pmu_poll(void) { ++test_pmu_poll_calls; return ESP_OK; }
static inline uint32_t reading_stats_total_seconds(void) { return 3661U; }
static inline bool reading_stats_get_last_book(char* title, uint32_t cap, uint8_t* pct) {
    snprintf(title, cap, "%s", "三体.epub");
    if (pct) *pct = 42;
    return true;
}
static inline bool reading_stats_get_excerpt(char* title, uint32_t tcap, char* text, uint32_t cap) {
    snprintf(title, tcap, "%s", "三体.epub");
    snprintf(text, cap, "%s", "给岁月以文明");
    return true;
}
static inline int ui_content_width(void) { return 604; }
static inline EpdRect ui_row_rect(int i,int n,int y,int h) { int w=(604-(n-1)*12)/n; return (EpdRect){40+i*(w+12),y,w,h}; }
static inline EpdRect ui_bar_rect(int i,int n) { int w=(508-(n-1)*12)/n; return (EpdRect){40+i*(w+12),1096,w,96}; }
static inline bool ui_rect_hit(EpdRect r,int x,int y) { return x>=r.x&&y>=r.y&&x<r.x+r.width&&y<r.y+r.height; }
static inline EpdRect ui_rect_union(EpdRect a,EpdRect b) { int x=a.x<b.x?a.x:b.x,y=a.y<b.y?a.y:b.y; int x2=a.x+a.width>b.x+b.width?a.x+a.width:b.x+b.width; int y2=a.y+a.height>b.y+b.height?a.y+a.height:b.y+b.height; return(EpdRect){x,y,x2-x,y2-y}; }
static inline void ui_clear_page(uint8_t* f) {(void)f;}
static inline void ui_clear_rect_fast(uint8_t* f,EpdRect r) {(void)f;(void)r;}
static char test_last_header[64];
static inline void ui_draw_header(uint8_t* f, const char* title, const char* sub) {
    (void)f; (void)sub;
    snprintf(test_last_header, sizeof(test_last_header), "%s", title ? title : "");
}
static inline void ui_draw_button(uint8_t* f,...) {(void)f;}
static inline void ui_draw_pressed_round_rect(uint8_t* f,...) {(void)f;}
static inline void ui_draw_round_rect(uint8_t* f,...) {(void)f;}
static inline void ui_draw_menu_handle(uint8_t* f,...) {(void)f;}
static inline void ui_hairline(uint8_t* f,...) {(void)f;}
static inline void ui_text(uint8_t* f,...) {(void)f;}
static inline void ui_text_vc(uint8_t* f,...) {(void)f;}
static inline void epd_fill_rect(EpdRect r,...) {(void)r;}
static inline int ttf_text_width_px(int px,const char* s) {return (int)strlen(s)*px/2;}
static bool test_font_suspended;
static inline int ttf_font_suspend_sd(bool suspend) {test_font_suspended=suspend;return ESP_OK;}
static inline void display_set_bulk_io(bool b) {(void)b;}
static inline int update_display_area_with(void* p,...) {(void)p;return 0;}
static inline int update_display_white(void* p) {(void)p;return 0;}
static inline int update_display_full(void* p) {(void)p;return 0;}
static inline void guard_draw_result(void* p,int e) {(void)p;(void)e;}
static inline size_t heap_caps_get_free_size(int cap) {(void)cap;return 1000000;}
typedef struct {char path[160];bool is_flash;} book_store_root_t;
static inline uint64_t book_store_free_bytes(void* r) {(void)r;return 1000000;}
static inline int book_store_upload_root(book_store_root_t* r) {strcpy(r->path,"/flash/books");return 0;}
static inline int book_store_upload_root_for(bool flash,book_store_root_t* r) {(void)flash;strcpy(r->path,"/flash/books");return 0;}
static inline int book_store_roots(book_store_root_t out[2],int* n) {strcpy(out[0].path,"/flash/books");out[0].is_flash=true;*n=1;return ESP_OK;}
static inline int book_store_forget_changed_path(const char* path) {(void)path;return ESP_OK;}
static inline size_t book_store_file_limit(void* r) {(void)r;return 1000000;}
static int test_store_changes;
static inline void book_store_notify_changed(void) { ++test_store_changes; }
static inline int app_count(void) {return 0;}
static inline const app_desc_t* app_at(int i) {(void)i;return NULL;}
typedef enum { APP_SLEEP_LIGHT = 0, APP_SLEEP_DEEP = 1, APP_SLEEP_OFF = 2 } app_sleep_mode_t;
typedef enum { APP_SAVER_BUILTIN = 0, APP_SAVER_QUOTE = 1, APP_SAVER_IMAGE = 2 } app_saver_style_t;
static read_pico_device_settings_t test_settings = {
    .sleep_mode = APP_SLEEP_DEEP, .auto_sleep_sec = 120, .book_px = 48,
    .book_refresh = 1, .book_line_height = 1, .book_margin = 1,
    .dashboard_items = 0x0f, .timezone_min = 480, .charge_led = true,
    .low_soc_notify_percent = 10, .font_path = "/sdcard/fonts/test.ttf"
};
static int test_runtime_apply_calls;
static inline app_sleep_mode_t app_settings_sleep_mode(void) { return (app_sleep_mode_t)test_settings.sleep_mode; }
static inline void app_settings_set_sleep_mode(app_sleep_mode_t m) { test_settings.sleep_mode = (uint8_t)m; }
static inline uint16_t app_settings_auto_sleep_sec(void) { return test_settings.auto_sleep_sec; }
static inline void app_settings_set_auto_sleep_sec(uint16_t s) { test_settings.auto_sleep_sec = s; }
static inline bool app_settings_pickup_wake(void) { return test_settings.pickup_wake; }
static inline void app_settings_set_pickup_wake(bool on) { test_settings.pickup_wake = on; }
static inline uint8_t app_settings_book_px(void) { return test_settings.book_px; }
static inline void app_settings_set_book_px(uint8_t px) { test_settings.book_px = px; }
static inline uint8_t app_settings_book_refresh(void) { return test_settings.book_refresh; }
static inline void app_settings_set_book_refresh(uint8_t p) { test_settings.book_refresh = p; }
static inline uint8_t app_settings_book_line_height(void) { return test_settings.book_line_height; }
static inline void app_settings_set_book_line_height(uint8_t p) { test_settings.book_line_height = p; }
static inline uint8_t app_settings_book_margin(void) { return test_settings.book_margin; }
static inline void app_settings_set_book_margin(uint8_t p) { test_settings.book_margin = p; }
static inline uint16_t app_settings_book_weight(void) { return test_settings.book_weight; }
static inline void app_settings_set_book_weight(uint16_t w) { test_settings.book_weight = w; }
static inline uint8_t app_settings_book_rotation(void) { return test_settings.book_rotation; }
static inline void app_settings_set_book_rotation(uint8_t r) { test_settings.book_rotation = r; }
static inline uint8_t app_settings_book_gesture(void) { return test_settings.book_gesture; }
static inline void app_settings_set_book_gesture(uint8_t g) { test_settings.book_gesture = g; }
static inline app_saver_style_t app_settings_saver_style(void) { return (app_saver_style_t)test_settings.saver_style; }
static inline void app_settings_set_saver_style(app_saver_style_t s) { test_settings.saver_style = (uint8_t)s; }
static inline uint8_t app_settings_dashboard_items(void) { return test_settings.dashboard_items; }
static inline void app_settings_set_dashboard_items(uint8_t d) { test_settings.dashboard_items = d; }
static inline int16_t app_settings_timezone_min(void) { return test_settings.timezone_min; }
static inline void app_settings_set_timezone_min_deferred(int16_t m) { test_settings.timezone_min = m; }
static inline bool app_settings_charge_led(void) { return test_settings.charge_led; }
static inline void app_settings_set_charge_led_deferred(bool on) { test_settings.charge_led = on; }
static inline uint8_t app_settings_low_soc_notify_percent(void) { return test_settings.low_soc_notify_percent; }
static inline void app_settings_set_low_soc_notify_percent_deferred(uint8_t p) { test_settings.low_soc_notify_percent = p; }
static inline bool app_settings_muted(void) { return test_settings.muted; }
static inline void app_settings_set_muted(bool m) { test_settings.muted = m; }
static inline bool app_settings_transfer_pin_required(void) { return test_settings.transfer_pin_required; }
static inline void app_settings_set_transfer_pin_required(bool r) { test_settings.transfer_pin_required = r; }
static inline bool app_settings_ble_enabled(void) { return test_settings.ble_enabled; }
static inline void app_settings_set_ble_enabled(bool e) { test_settings.ble_enabled = e; }
static inline bool app_settings_ble_pin_required(void) { return test_settings.ble_pin_required; }
static inline void app_settings_set_ble_pin_required(bool r) { test_settings.ble_pin_required = r; }
static inline const char* app_settings_font_path(void) { return test_settings.font_path; }
static inline void app_settings_set_font_path(const char* p) { snprintf(test_settings.font_path, sizeof(test_settings.font_path), "%s", p ? p : ""); }
static inline void app_settings_apply_runtime(void) { ++test_runtime_apply_calls; }

typedef enum {READ_PICO_TRANSFER_MODE_AP,READ_PICO_TRANSFER_MODE_STA} read_pico_transfer_mode_t;
typedef enum {READ_PICO_TRANSFER_STOPPED,READ_PICO_TRANSFER_STARTING,READ_PICO_TRANSFER_READY,READ_PICO_TRANSFER_UPLOADING,READ_PICO_TRANSFER_ERROR} read_pico_transfer_state_t;
typedef struct {
    read_pico_transfer_mode_t mode;
    const char* root_dir;
    const char* font_dir;
    bool is_flash;
    size_t file_limit;
    uint64_t(*free_bytes_cb)(void*);
    void* free_bytes_ctx;
    int(*file_changed_cb)(const char*);
    void(*sta_ip_cb)(void);
    void(*service_poll_cb)(void);
    void(*network_stop_cb)(void);
    const char* sd_root;
    bool(*storage_info_cb)(read_pico_storage_info_t*,void*);
    bool(*storage_switch_cb)(bool,void*);
    void* storage_ctx;
    bool(*settings_get_cb)(read_pico_device_settings_t*,void*);
    esp_err_t(*settings_set_cb)(const read_pico_device_settings_t*,void*);
    void* settings_ctx;
    bool pin_required;
    bool(*time_get_cb)(read_pico_time_info_t*,void*);
    esp_err_t(*time_set_cb)(uint32_t,bool,int16_t,void*);
    void* time_ctx;
    esp_err_t(*font_select_cb)(const char*,void*);
    void* font_ctx;
    bool(*dashboard_stats_cb)(read_pico_dashboard_stats_t*,void*);
    void* dashboard_ctx;
    bool(*book_progress_cb)(const char*,uint32_t,read_pico_book_progress_t*,void*);
    void* book_progress_ctx;
    size_t(*webmarks_list_cb)(read_pico_webmark_t*,size_t,void*);
    esp_err_t(*webmarks_upsert_cb)(const char*,const char*,void*);
    esp_err_t(*webmarks_delete_cb)(const char*,void*);
    void* webmarks_ctx;
    bool(*focus_get_cb)(read_pico_focus_status_t*,void*);
    esp_err_t(*focus_ctrl_cb)(bool,int,void*);
    void* focus_ctx;
    bool(*stop_request_cb)(void*);
    void* stop_request_ctx;
} read_pico_transfer_cfg_t;
static read_pico_transfer_cfg_t test_last_cfg;
typedef struct {read_pico_transfer_mode_t mode;read_pico_transfer_state_t state;bool wifi_configured,network_ready,pin_required;char ssid[33],pin[7],wifi_ssid[33],url[64],mdns_host[64],cur_name[121];unsigned sta_count,done_count,changed_count;size_t cur_bytes,cur_total;int last_error;} read_pico_transfer_status_t;
static int test_pin_reset_calls;
static inline void read_pico_transfer_reset_pin(void) { ++test_pin_reset_calls; }
#define READ_PICO_TRANSFER_PASSWORD "readpico"
static read_pico_transfer_status_t test_status;
static bool test_busy;
static int test_save_error;
static int test_forget_count, test_forget_error;
static bool test_configured = true;
static int test_scan_error, test_save_count, test_scan_calls;
static size_t test_scan_count = 8;
static bool test_ble_held;
static int test_ble_hold_calls, test_ble_release_calls;
static inline void app_ble_wifi_hold(bool hold) { test_ble_held = hold; if (hold) ++test_ble_hold_calls; else ++test_ble_release_calls; }
static char test_saved_ssid[33];
static inline bool read_pico_transfer_try_stop_if_idle(void) {return !test_busy;}
static int test_stop_count;
static inline void read_pico_transfer_stop(void) { ++test_stop_count; }
static inline void read_pico_transfer_get_status(read_pico_transfer_status_t* s) {*s=test_status;}
static inline int read_pico_transfer_get_saved_wifi(char* s,bool* c) {strcpy(s,test_configured?"saved":"");*c=test_configured;return 0;}
static inline int read_pico_transfer_get_saved_wifi_list(char s[5][33],size_t* count) {memset(s,0,5*33);*count=test_configured?2:0;if(*count){strcpy(s[0],"saved");strcpy(s[1],"backup");}return 0;}
static inline int read_pico_transfer_start(const read_pico_transfer_cfg_t* c) {test_last_cfg=*c; if (!test_font_suspended || !c->file_changed_cb || c->file_changed_cb("/flash/books/test.txt")) return ESP_FAIL;return 0;}
static inline void read_pico_transfer_service_poll(void) {}
static inline int read_pico_transfer_save_wifi(const char* s,const char* p) {(void)p;test_save_count++;snprintf(test_saved_ssid,33,"%s",s);return test_save_error;}
static inline int read_pico_transfer_forget_wifi_ssid(const char* s) {(void)s;test_forget_count++;return test_forget_error;}
static inline int read_pico_transfer_scan_wifi(read_pico_transfer_network_t* out,size_t* count) {++test_scan_calls;int err=test_scan_error;*count=err?0:test_scan_count;for(size_t i=0;i<*count;i++){out[i]=(read_pico_transfer_network_t){.supported=true,.requires_password=true};snprintf(out[i].ssid,33,"network%u",(unsigned)i);}if(*count)snprintf(out[0].ssid,33,"中文家庭网络");return err;}
static int test_qr_encodes;
static bool test_qr_failure;
static char test_qr_payload[128];
static inline void ui_wifi_qr_clear(void) {test_qr_payload[0]=0;}
static inline bool ui_wifi_qr_prepare(const char* s,const char* p) {(void)p;++test_qr_encodes;snprintf(test_qr_payload,sizeof(test_qr_payload),"%s",s);return !test_qr_failure;}
static inline bool ui_wifi_qr_prepare_url(const char* s) {++test_qr_encodes;snprintf(test_qr_payload,sizeof(test_qr_payload),"%s",s);return !test_qr_failure;}
static inline void ui_wifi_qr_draw(uint8_t* fb,EpdRect r) {(void)fb;(void)r;}

static inline int read_pico_transfer_forget_wifi(void) { ++test_forget_count; if (!test_forget_error) test_configured=false; return test_forget_error; }

static inline int book_progress_forget(const char* path) {(void)path;return ESP_OK;}

#define READ_PICO_WEBOPEN_URL_MAX 256
const app_desc_t app_web = { .title = "网页阅读" };
static char test_pending_webopen[READ_PICO_WEBOPEN_URL_MAX];
static char test_queued_web_url[READ_PICO_WEBOPEN_URL_MAX];
static inline bool read_pico_transfer_take_webopen_url(char url[READ_PICO_WEBOPEN_URL_MAX]) {
    if (!test_pending_webopen[0]) return false;
    snprintf(url, READ_PICO_WEBOPEN_URL_MAX, "%s", test_pending_webopen);
    test_pending_webopen[0] = 0;
    return true;
}
void app_web_queue_url(const char* url) {
    snprintf(test_queued_web_url, sizeof(test_queued_web_url), "%s", url ? url : "");
}

typedef struct { uint32_t file_size; uint16_t chapter; uint32_t byte_off; uint8_t px; uint8_t pct; uint32_t last_open_s; } book_progress_t;
static inline bool book_progress_load(const char* path, uint32_t size, book_progress_t* out) {
    if (!path || !out || !strstr(path, "prog.txt")) return false;
    *out = (book_progress_t){ .file_size = size, .chapter = 2, .byte_off = 512, .px = 48, .pct = 65, .last_open_s = 12 };
    return true;
}

#define WEB_BOOKMARK_MAX 20
typedef struct { char title[64]; char url[256]; } web_bookmark_t;
static web_bookmark_t test_webmarks[WEB_BOOKMARK_MAX];
static size_t test_webmark_count;
static inline void web_bookmarks_load(web_bookmark_t out[WEB_BOOKMARK_MAX], size_t* count) {
    memcpy(out, test_webmarks, sizeof(test_webmarks));
    *count = test_webmark_count;
}
static inline esp_err_t web_bookmarks_upsert(const char* title, const char* url) {
    if (!url || strncmp(url, "http", 4) != 0) return ESP_ERR_INVALID_ARG;
    if (test_webmark_count >= WEB_BOOKMARK_MAX) return ESP_FAIL;
    snprintf(test_webmarks[test_webmark_count].title, 64, "%s", title ? title : "");
    snprintf(test_webmarks[test_webmark_count].url, 256, "%s", url);
    ++test_webmark_count;
    return ESP_OK;
}
static inline esp_err_t web_bookmarks_delete(const char* url) {
    for (size_t i = 0; i < test_webmark_count; ++i) {
        if (!strcmp(test_webmarks[i].url, url)) {
            test_webmarks[i] = test_webmarks[--test_webmark_count];
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}
