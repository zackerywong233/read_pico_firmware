/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：针对宫格首页返回、便签拖动与坐标旋转、统一时钟与手动调时、设置子页手势提交的主机回归测试。
 * English: Host regression tests for grid-home return, sketch drag/rotation, unified clock/time-edit, and Settings sub-page gestures.
 */
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "app.h"
#include "app_loop.h"
#include "app_registry.h"
#include "app_focus.h"
#include "app_personal.h"
#include "continuous_du.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_app_desc.h"
#include "esp_netif_sntp.h"
#include "ota_update.h"
#include "read_pico_pmu.h"
#include "read_pico_sd.h"
#include "read_pico_transfer.h"
#include "reading_stats.h"
#include "settings.h"
#include "time_sync.h"
#include "ui_heatmap.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_time_edit.h"

extern const app_desc_t app_settings;
extern const app_desc_t app_sketch;

/* ---- 模拟系统时钟 / Fake system clock ---- */
static time_t s_fake_sys_time = 0;

time_t test_time(time_t *out) {
    if (out) *out = s_fake_sys_time;
    return s_fake_sys_time;
}

int test_settimeofday(const struct timeval *tv, const struct timezone *tz) {
    (void)tz;
    if (!tv) return -1;
    s_fake_sys_time = tv->tv_sec;
    return 0;
}

/* ---- 屏幕与 framebuffer 模拟（与真机旋转一致）/ Panel & framebuffer stubs matching hardware rotation ---- */
EpdWaveform E0470_WAVEFORM;
EpdWaveform E0470_FOLLOW_WAVEFORM;

int epd_width(void) { return 1216; }
int epd_height(void) { return 684; }
int epd_rotated_display_width(void) { return 684; }
int epd_rotated_display_height(void) { return 1216; }

void continuous_du_from_logical(int lx, int ly, int *px, int *py) {
    *px = ly;
    *py = epd_height() - lx - 1;
}

EpdRect continuous_du_rect_from_logical(EpdRect logical) {
    return (EpdRect){
        .x = logical.y,
        .y = epd_height() - logical.x - logical.width,
        .width = logical.height,
        .height = logical.width,
    };
}

void epd_draw_pixel(int x, int y, uint8_t color, uint8_t *fb) {
    if (!fb) return;
    int px = 0, py = 0;
    continuous_du_from_logical(x, y, &px, &py);
    if (px < 0 || px >= epd_width() || py < 0 || py >= epd_height()) return;
    uint8_t *p = &fb[py * (epd_width() / 2) + px / 2];
    if (px & 1) *p = (uint8_t)((*p & 0x0F) | (color & 0xF0));
    else *p = (uint8_t)((*p & 0xF0) | (color >> 4));
}

void epd_fill_rect(EpdRect r, uint8_t color, uint8_t *fb) { (void)r; (void)color; (void)fb; }
void epd_fill_triangle(int a, int b, int c, int d, int e, int f, uint8_t g, uint8_t *h) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h;
}
void epd_poweron(void) {}

static int s_du_marks = 0;
static int s_du_scans = 0;
static int s_du_pending = 0;
int continuous_du_dark_phases(void) { return 6; }
int continuous_du_light_phases(void) { return 6; }
bool continuous_du_init(void) { return true; }
void continuous_du_reset(void) { s_du_pending = 0; }
bool continuous_du_busy(void) { return s_du_pending > 0; }
void continuous_du_mark_circle(int cx, int cy, int r, int phases) {
    (void)cx; (void)cy; (void)r; (void)phases;
    s_du_marks++;
    s_du_pending = 6;
}
enum EpdDrawError continuous_du_scan(EpdiyHighlevelState *hl, EpdRect area) {
    (void)hl; (void)area;
    s_du_scans++;
    if (s_du_pending > 0) s_du_pending--;
    return EPD_DRAW_SUCCESS;
}

/* ---- UI 绘制记录 / UI draw recording ---- */
static char s_last_header[128];
static char s_last_texts[128][128];
static int s_text_count;

static void clear_recorded_ui(void) {
    s_last_header[0] = '\0';
    s_text_count = 0;
}

static bool ui_saw_text(const char *needle) {
    if (strstr(s_last_header, needle)) return true;
    for (int i = 0; i < s_text_count; ++i) {
        if (strstr(s_last_texts[i], needle)) return true;
    }
    return false;
}

static void record_text(const char *t) {
    if (!t || s_text_count >= 128) return;
    snprintf(s_last_texts[s_text_count++], sizeof(s_last_texts[0]), "%s", t);
}

void ui_clear_page(uint8_t *fb) {
    if (fb) memset(fb, 0xFF, (size_t)epd_width() * (size_t)epd_height() / 2);
    clear_recorded_ui();
}
void ui_clear_rect_fast(uint8_t *fb, EpdRect r) { (void)fb; (void)r; }
int ui_draw_header(uint8_t *fb, const char *title, const char *sub) {
    (void)fb; (void)sub;
    snprintf(s_last_header, sizeof(s_last_header), "%s", title ? title : "");
    record_text(title);
    record_text(sub);
    return UI_CONTENT_TOP;
}
int ui_draw_section(uint8_t *fb, int y, const char *title) { (void)fb; record_text(title); return y + UI_SEC_HEAD; }
int ui_draw_row(uint8_t *fb, int y, const char *l, const char *v) { (void)fb; record_text(l); record_text(v); return y + UI_ROW_H; }
void ui_text(uint8_t *fb, int x, int y, int px, const char *t, enum EpdFontFlags a, bool inv) {
    (void)fb; (void)x; (void)y; (void)px; (void)a; (void)inv;
    record_text(t);
}
void ui_text_vc(uint8_t *fb, int x, int y, int px, const char *t, enum EpdFontFlags a, bool inv) {
    ui_text(fb, x, y, px, t, a, inv);
}
static int s_filled_round_rects = 0;
void ui_draw_round_rect(uint8_t *fb, EpdRect r, int rad, uint8_t c) { (void)fb; (void)r; (void)rad; (void)c; }
void ui_fill_round_rect(uint8_t *fb, EpdRect r, int rad, uint8_t c) {
    (void)fb; (void)r; (void)rad; (void)c;
    s_filled_round_rects++;
}
void ui_draw_pressed_round_rect(uint8_t *fb, EpdRect r, int rad) { (void)fb; (void)r; (void)rad; }
void ui_draw_choice_round_rect(uint8_t *fb, EpdRect r, int rad, bool on) { (void)fb; (void)r; (void)rad; (void)on; }
void ui_draw_button(uint8_t *fb, EpdRect r, const char *label, bool on) { (void)fb; (void)r; (void)on; record_text(label); }
void ui_hairline(uint8_t *fb, int y, int x, int w, uint8_t c) { (void)fb; (void)y; (void)x; (void)w; (void)c; }
void ui_draw_key_hints(uint8_t *fb, const ui_key_hints_t *h) { (void)fb; (void)h; }
int ui_key_hint_hit_test(uint16_t x, uint16_t y, const ui_key_hints_t *h) {
    if (!h || h->hidden || y < UI_KEY_HINT_RULE_Y || y >= UI_LOCK_HEIGHT || x >= UI_LOCK_WIDTH) return -1;
    int col_w = UI_LOCK_WIDTH / 3;
    int col = (int)x < col_w ? UI_KEY_1 : (int)x < col_w * 2 ? UI_KEY_2 : UI_KEY_3;
    const char *label = col == UI_KEY_1 ? h->left
        : col == UI_KEY_2 ? (h->center ? h->center : "刷新 · 长按桌面") : h->right;
    return (label && label[0]) ? col : -1;
}
int ui_content_width(void) { return epd_rotated_display_width() - 2 * UI_MARGIN; }
int ui_content_right(void) { return epd_rotated_display_width() - UI_MARGIN; }
EpdRect ui_content_refresh_area(void) { return (EpdRect){ 0, UI_HEADER_RULE_Y, 684, UI_CONTENT_BOTTOM - UI_HEADER_RULE_Y }; }
bool ui_rect_hit(EpdRect r, uint16_t x, uint16_t y) {
    return (int)x >= r.x && (int)x < r.x + r.width && (int)y >= r.y && (int)y < r.y + r.height;
}
EpdRect ui_grid_rect(int col, int cols, int row, int y0, int h) {
    int w = (ui_content_width() - (cols - 1) * UI_GAP) / cols;
    return (EpdRect){ UI_MARGIN + col * (w + UI_GAP), y0 + row * (h + UI_GAP), w, h };
}
EpdRect ui_row_rect(int col, int cols, int y0, int h) { return ui_grid_rect(col, cols, 0, y0, h); }
EpdRect ui_bar_rect(int col, int cols) {
    int avail = ui_content_width();
    int w = (avail - (cols - 1) * UI_GAP) / cols;
    return (EpdRect){ UI_MARGIN + col * (w + UI_GAP), UI_BAR_TOP, w, UI_BAR_H };
}
int ui_bar_hit(uint16_t x, uint16_t y, int cols) {
    for (int i = 0; i < cols; i++) if (ui_rect_hit(ui_bar_rect(i, cols), x, y)) return i;
    return -1;
}
EpdRect ui_rect_union(EpdRect a, EpdRect b) {
    if (a.width <= 0 || a.height <= 0) return b;
    if (b.width <= 0 || b.height <= 0) return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.width > b.x + b.width ? a.x + a.width : b.x + b.width;
    int y1 = a.y + a.height > b.y + b.height ? a.y + a.height : b.y + b.height;
    return (EpdRect){ x0, y0, x1 - x0, y1 - y0 };
}

/* ---- 设置、PMU、OTA、传书、SD、字体模拟 / Subsystem stubs ---- */
static int16_t s_tz_min = 480;
static uint8_t s_book_px = 48;
static uint8_t s_book_refresh = 1;
static uint16_t s_book_weight = 0;
static uint8_t s_book_lh = 1;
static uint8_t s_book_margin = 1;
static bool s_book_top = true;
static bool s_book_bot = true;
static uint8_t s_book_para = 1;
static uint8_t s_book_ind = 0;
static app_sleep_mode_t s_sleep_mode = APP_SLEEP_DEEP;
static uint16_t s_auto_sleep = 120;
static app_saver_style_t s_saver = APP_SAVER_BUILTIN;
static uint8_t s_dash_items = APP_DASH_TIME | APP_DASH_BATTERY | APP_DASH_LAST_BOOK |
                              APP_DASH_NOTE | APP_DASH_HEATMAP;
static bool s_charge_led = true;
static uint8_t s_low_soc = 15;
static bool s_muted = false;
static bool s_ble_enabled = true;
static bool s_ble_pin_required = false;
static app_ambient_settings_t s_ambient = { .enabled = false, .interval_min = 60 };

int16_t app_settings_timezone_min(void) { return s_tz_min; }
void app_settings_set_timezone_min(int16_t m) { s_tz_min = m; }
uint8_t app_settings_book_px(void) { return s_book_px; }
void app_settings_set_book_px(uint8_t v) { s_book_px = v; }
uint8_t app_settings_book_refresh(void) { return s_book_refresh; }
void app_settings_set_book_refresh(uint8_t v) { s_book_refresh = v; }
uint16_t app_settings_book_weight(void) { return s_book_weight; }
void app_settings_set_book_weight(uint16_t v) { s_book_weight = v; }
uint8_t app_settings_book_line_height(void) { return s_book_lh; }
void app_settings_set_book_line_height(uint8_t v) { s_book_lh = v; }
uint8_t app_settings_book_margin(void) { return s_book_margin; }
void app_settings_set_book_margin(uint8_t v) { s_book_margin = v; }
bool app_settings_book_top_bar(void) { return s_book_top; }
void app_settings_set_book_top_bar(bool v) { s_book_top = v; }
bool app_settings_book_bottom_bar(void) { return s_book_bot; }
void app_settings_set_book_bottom_bar(bool v) { s_book_bot = v; }
uint8_t app_settings_book_para_gap(void) { return s_book_para; }
void app_settings_set_book_para_gap(uint8_t v) { s_book_para = v; }
uint8_t app_settings_book_indent(void) { return s_book_ind; }
void app_settings_set_book_indent(uint8_t v) { s_book_ind = v; }
app_sleep_mode_t app_settings_sleep_mode(void) { return s_sleep_mode; }
void app_settings_set_sleep_mode(app_sleep_mode_t v) { s_sleep_mode = v; }
uint16_t app_settings_auto_sleep_sec(void) { return s_auto_sleep; }
void app_settings_set_auto_sleep_sec(uint16_t v) { s_auto_sleep = v; }
app_saver_style_t app_settings_saver_style(void) { return s_saver; }
void app_settings_set_saver_style(app_saver_style_t v) { s_saver = v; }
uint8_t app_settings_dashboard_items(void) { return s_dash_items; }
void app_settings_set_dashboard_items(uint8_t v) { s_dash_items = v; }
bool app_settings_charge_led(void) { return s_charge_led; }
void app_settings_set_charge_led(bool v) { s_charge_led = v; }
uint8_t app_settings_low_soc_notify_percent(void) { return s_low_soc; }
void app_settings_set_low_soc_notify_percent(uint8_t v) { s_low_soc = v; }
bool app_settings_muted(void) { return s_muted; }
void app_settings_set_muted(bool v) { s_muted = v; }
bool app_settings_ble_enabled(void) { return s_ble_enabled; }
void app_settings_set_ble_enabled(bool v) { s_ble_enabled = v; }
bool app_settings_ble_pin_required(void) { return s_ble_pin_required; }
void app_settings_set_ble_pin_required(bool v) { s_ble_pin_required = v; }
void app_ble_set_enabled(bool enabled) { s_ble_enabled = enabled; }
void app_ble_set_pin_required(bool required) { s_ble_pin_required = required; }
void app_ble_yield_to_ota(void) {}
void app_ble_format_status(char *buf, size_t cap) {
    if (!s_ble_enabled) snprintf(buf, cap, "已关闭");
    else if (s_ble_pin_required) snprintf(buf, cap, "ReadPico-1234 · 码 654321");
    else snprintf(buf, cap, "ReadPico-1234 · 广播中");
}
const char *app_settings_font_path(void) { return ""; }
void app_settings_ambient_get(app_ambient_settings_t *out) { *out = s_ambient; }
bool app_settings_ambient_set(const app_ambient_settings_t *cfg) { s_ambient = *cfg; return true; }
void app_settings_ambient_result_get(bool *h, bool *s, uint32_t *t) { *h = false; *s = false; *t = 0; }

static pmu_snapshot_t s_pmu = {
    .present = true, .identity_ok = true, .status_ok = true, .time_ok = false,
    .fw_major = 1, .fw_minor = 1, .fw_patch = 0,
    .battery_mv = 4100, .soc_permille = 850,
};
static int s_pmu_time_sync_calls = 0;
static bool s_pmu_time_sync_fail = false;
static esp_sntp_time_cb_t s_sntp_cb = NULL;
static int s_sntp_init_calls = 0;
static int s_sntp_deinit_calls = 0;
static int64_t s_fake_timer_offset_us = 0;

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *c) {
    s_sntp_init_calls++;
    s_sntp_cb = c ? c->sync_cb : NULL;
    return ESP_OK;
}
void esp_netif_sntp_deinit(void) {
    s_sntp_deinit_calls++;
    s_sntp_cb = NULL;
}

bool read_pico_pmu_ready(void) { return s_pmu.present; }
const pmu_snapshot_t *read_pico_pmu_get(void) { return &s_pmu; }
esp_err_t read_pico_pmu_refresh(void) { return ESP_OK; }
esp_err_t read_pico_pmu_cmd(uint16_t code, const uint8_t *p, uint8_t n) {
    (void)p; (void)n;
    if (code == PMU_CMD_TIME_GET) s_pmu.last_op_status = PMU_STATUS_OK;
    return ESP_OK;
}
esp_err_t read_pico_pmu_time_sync(uint32_t unix_seconds) {
    s_pmu_time_sync_calls++;
    if (s_pmu_time_sync_fail) {
        s_pmu.last_op_status = 0x00FF;
        return ESP_FAIL;
    }
    s_pmu.unix_sec = unix_seconds;
    s_pmu.time_ok = true;
    s_pmu.time_synced = 1;
    s_pmu.last_op_status = PMU_STATUS_OK;
    return ESP_OK;
}
bool read_pico_pmu_take_key_short(void) { return false; }

static esp_app_desc_t s_desc = { .version = "v0.1.19", .date = "Nov 21 2026", .time = "10:00:00" };
const esp_app_desc_t *esp_app_get_description(void) { return &s_desc; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "OK"; }

esp_err_t read_pico_transfer_get_saved_wifi_list(char out[5][33], size_t *count) {
    strcpy(out[0], "StudioWiFi");
    *count = 1;
    return ESP_OK;
}
esp_err_t read_pico_sd_get_info(read_pico_sd_info_t *info) { info->present = false; info->mounted = false; return ESP_OK; }
esp_err_t read_pico_sd_start_probe(void) { return ESP_OK; }
uint32_t reading_stats_total_seconds(void) { return 3600; }
bool reading_stats_get_last_book(char *buf, uint32_t n, uint8_t *pct) { snprintf(buf, n, "Demo"); *pct = 42; return true; }
uint32_t reading_stats_day_from_ymd(int year, int month, int day) {
    if (year < 2000 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
    int64_t y = (int64_t)year - (month <= 2 ? 1 : 0);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * ((int64_t)month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + doe - 719468);
}
void reading_stats_ymd_from_day(uint32_t day_index, int *year, int *month, int *day) {
    int64_t z = (int64_t)day_index + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp + (mp < 10 ? 3 : -9);
    y += (m <= 2);
    if (year) *year = (int)y;
    if (month) *month = (int)m;
    if (day) *day = (int)d;
}
uint8_t reading_stats_heatmap_level(uint32_t seconds) {
    if (seconds == 0) return 0;
    if (seconds < 900U) return 1;
    if (seconds < 1800U) return 2;
    if (seconds < 3600U) return 3;
    return 4;
}
bool reading_stats_get_heatmap(uint32_t anchor_day, reading_heatmap_t *out) {
    memset(out, 0, sizeof(*out));
    out->has_date = anchor_day != 0;
    out->anchor_day = anchor_day;
    out->daily_sec[READING_HEATMAP_DAYS - 1] = 1800; // today: 30 min (level 3)
    out->daily_sec[READING_HEATMAP_DAYS - 2] = 3600; // yesterday: 60 min (level 4)
    out->daily_sec[READING_HEATMAP_DAYS - 3] = 600;  // -2d: 10 min (level 1)
    out->today_sec = 1800;
    out->active_days = 3;
    out->current_streak = 3;
    out->longest_streak = 5;
    out->max_daily_sec = 3600;
    out->window_total_sec = 6000;
    return true;
}
bool ttf_font_ready(void) { return true; }
bool ttf_font_is_builtin(void) { return true; }
bool ttf_font_path_is_builtin(const char *p) { return !p || !p[0]; }
const char *ttf_font_path(void) { return ""; }
int ttf_font_open(const char *p) { (void)p; return 0; }
int ttf_font_open_builtin(void) { return 0; }
int ttf_text_width_px(int px, const char *t) { return px * (int)strlen(t) / 2; }
uint32_t ambient_sync_next_delay(uint32_t s, uint32_t i, uint8_t a, uint8_t b) { (void)s; (void)a; (void)b; return i; }

static ota_update_status_t s_ota = { .state = OTA_UPDATE_IDLE };
bool ota_update_check_start(void) { s_ota.state = OTA_UPDATE_CHECKING; return true; }
bool ota_update_install_start(void) { s_ota.state = OTA_UPDATE_DOWNLOADING; return true; }
void ota_update_cancel(void) { s_ota.state = OTA_UPDATE_CANCELLED; }
void ota_update_get_status(ota_update_status_t *out) { *out = s_ota; }

/* ---- 占位应用与主循环驱动 / Dummy sibling apps & real app_loop harness ---- */
const app_desc_t app_transfer = { .title = "transfer" };
const app_desc_t app_sleep = { .title = "sleep" };
const app_desc_t app_home = { .title = "home" };
const app_desc_t app_sd = { .title = "sd" };
const app_desc_t app_book = { .title = "book" };
static const app_desc_t s_font_app = { .title = "font" };
static const app_desc_t s_self_app = { .title = "selftest" };
static const app_desc_t s_pico_home = { .title = "pico_home" };

int app_count(void) { return 2; }
const app_desc_t *app_at(int i) { return i == 0 ? &s_pico_home : i == 1 ? &app_settings : NULL; }
int app_index_of(const app_desc_t *a) { return a == &s_pico_home ? 0 : a == &app_settings ? 1 : -1; }
const app_desc_t *app_home_page(void) { return &s_pico_home; }
const app_desc_t *app_personal_page(void) { return &app_personal; }
const app_desc_t *app_selftest_page(void) { return &s_self_app; }
const app_desc_t *app_font_page(void) { return &s_font_app; }
const app_desc_t *app_settings_page(void) { return &app_settings; }

int ui_key_hit_test(uint16_t x, uint16_t y) { return y >= UI_KEY_AREA_TOP && x < 480 ? x / 160 : -1; }

void guard_draw_result(EpdiyHighlevelState *hl, enum EpdDrawError err) { (void)hl; assert(err == EPD_DRAW_SUCCESS); }
enum EpdDrawError update_display_full(EpdiyHighlevelState *hl) { (void)hl; return EPD_DRAW_SUCCESS; }
enum EpdDrawError update_display_mode(EpdiyHighlevelState *hl, enum EpdDrawMode m) { (void)hl; (void)m; return EPD_DRAW_SUCCESS; }
enum EpdDrawError update_display_white(EpdiyHighlevelState *hl) { (void)hl; return EPD_DRAW_SUCCESS; }
enum EpdDrawError update_display_area_with(EpdiyHighlevelState *hl, const EpdWaveform *w, enum EpdDrawMode m, EpdRect a) {
    (void)hl; (void)w; (void)m; (void)a;
    return EPD_DRAW_SUCCESS;
}
bool display_take_white_exit(void) { return false; }
void rails_idle_check(int64_t n) { (void)n; }
void rails_keepalive(void) {}
bool app_lock_powers_down(void) { return false; }
void enter_lock_and_sleep(EpdiyHighlevelState *hl, int64_t *t, sc7a20h_lab_handle_t acc) { (void)hl; (void)t; (void)acc; }

/* ---- 主循环触摸回放 / Deterministic touch playback for app_loop ---- */
typedef struct { uint16_t x, y; uint8_t count; } touch_step_t;
static touch_step_t s_steps[64];
static int s_step_count, s_step;
static jmp_buf s_loop_exit;

esp_err_t cst836u_read(cst836u_handle_t h, cst836u_touch_t *t) {
    (void)h;
    touch_step_t st = s_steps[s_step];
    memset(t, 0, sizeof(*t));
    t->touched = st.count > 0;
    t->count = st.count;
    t->x = st.x;
    t->y = st.y;
    if (st.count > 0) {
        t->points[0].active = true;
        t->points[0].x = st.x;
        t->points[0].y = st.y;
    }
    return ESP_OK;
}
esp_err_t cst836u_get_info(cst836u_handle_t h, cst836u_info_t *i) { (void)h; (void)i; return ESP_OK; }
int64_t esp_timer_get_time(void) { return s_fake_timer_offset_us + (int64_t)(s_step + 1) * 10000; }
void vTaskDelay(int ticks) { (void)ticks; if (++s_step >= s_step_count) longjmp(s_loop_exit, 1); }

static void reset_steps(void) { s_step_count = 0; s_step = 0; }
static void push_tap(int x, int y) {
    s_steps[s_step_count++] = (touch_step_t){ (uint16_t)x, (uint16_t)y, 1 };
    s_steps[s_step_count++] = (touch_step_t){ 0, 0, 0 };
}
static void push_idle(void) { s_steps[s_step_count++] = (touch_step_t){ 0, 0, 0 }; }

static void test_time_math_and_unified_clock(void) {
    assert(app_time_days_in_month(2024, 2) == 29);
    assert(app_time_days_in_month(2026, 2) == 28);
    assert(app_time_days_in_month(2000, 2) == 29);
    assert(app_time_days_in_month(2100, 2) == 28);

    // 2026-11-21 18:30 UTC+8 -> 2026-11-21 10:30 UTC
    uint32_t utc = 0;
    assert(app_time_make_utc(2026, 11, 21, 18, 30, 480, &utc));
    time_t raw = (time_t)utc;
    struct tm tm_utc;
    assert(gmtime_r(&raw, &tm_utc) != NULL);
    assert(tm_utc.tm_year + 1900 == 2026 && tm_utc.tm_mon + 1 == 11 && tm_utc.tm_mday == 21);
    assert(tm_utc.tm_hour == 10 && tm_utc.tm_min == 30 && tm_utc.tm_sec == 0);

    // 模拟主机未同步、PMU 有有效时间：开机同步后 Focus 和 Personal 读到一致时间。
    // Simulate unsynced host with valid PMU RTC: after boot sync, Focus and Personal see identical time.
    s_fake_sys_time = 0;
    s_pmu.time_ok = true;
    s_pmu.unix_sec = utc;
    app_time_sync_from_pmu();
    assert(s_fake_sys_time == (time_t)utc);

    uint8_t fb[1216 * 684 / 2];
    app_ctx_t ctx = { .fb = fb, .now_ms = 1000 };
    app_focus.on_enter(&ctx);
    app_focus.render(&ctx, fb);
    assert(ui_saw_text("18:30"));
    assert(ui_saw_text("2026-11-21"));

    app_personal.on_enter(&ctx);
    app_personal.render(&ctx, fb);
    assert(ui_saw_text("18:30"));
    assert(ui_saw_text("2026-11-21"));

    // 通过 ui_time_edit 把时间改为 2026-11-22 19:30 并保存，验证 Focus 与 Personal 同时更新且同步 PMU。
    // Adjust via ui_time_edit to 2026-11-22 19:30 and save; verify both Focus and Personal update plus PMU sync.
    ui_time_edit_t ed;
    ui_time_edit_open(&ed);
    assert(ed.year == 2026 && ed.month == 11 && ed.day == 21 && ed.hour == 18 && ed.minute == 30);
    // 点按“日 +”与“时 +”以及“保存时间” / Tap Day +, Hour +, and Save.
    assert(ui_time_edit_tap(&ed, 540, 444 + 20) == UI_TIME_EDIT_CHANGED);
    assert(ui_time_edit_tap(&ed, 540, 532 + 20) == UI_TIME_EDIT_CHANGED);
    assert(ed.day == 22 && ed.hour == 19);
    int before_sync = s_pmu_time_sync_calls;
    EpdRect save_btn = ui_bar_rect(1, 2);
    assert(ui_time_edit_tap(&ed, (uint16_t)(save_btn.x + 20), (uint16_t)(save_btn.y + 20)) == UI_TIME_EDIT_SAVED);
    assert(s_pmu_time_sync_calls == before_sync + 1);
    assert(ed.pmu_synced);

    app_focus.render(&ctx, fb);
    assert(ui_saw_text("19:30") && ui_saw_text("2026-11-22"));
    app_personal.render(&ctx, fb);
    assert(ui_saw_text("19:30") && ui_saw_text("2026-11-22"));

    // 验证专注时钟底栏“调时间”与个人看板“日期与时间”卡片都能直接打开校准界面。
    // Verify Focus bottom-bar quick button and Personal time card both open the manual editor directly.
    EpdRect focus_time_btn = ui_bar_rect(2, 3);
    cst836u_touch_t t = { .touched = true, .count = 1, .x = (uint16_t)(focus_time_btn.x + 20), .y = (uint16_t)(focus_time_btn.y + 20) };
    assert(app_focus.on_touch(&ctx, &t) == APP_REDRAW_FULL);
    app_focus.render(&ctx, fb);
    assert(strcmp(s_last_header, "校准日期与时间") == 0);
    EpdRect cancel_btn = ui_bar_rect(0, 2);
    t.x = (uint16_t)(cancel_btn.x + 20); t.y = (uint16_t)(cancel_btn.y + 20);
    assert(app_focus.on_touch(&ctx, &t) == APP_REDRAW_FULL);

    app_personal.on_enter(&ctx);
    t.x = UI_MARGIN + 40; t.y = UI_CONTENT_TOP + 40;
    assert(app_personal.on_touch(&ctx, &t) == APP_REDRAW_FULL);
    app_personal.render(&ctx, fb);
    assert(strcmp(s_last_header, "校准日期与时间") == 0);

    // 验证专注时钟远程状态读取与启停钩子。/ Verify Focus timer remote status and start/stop hooks.
    app_focus_status_t fst = {0};
    app_focus_get_status(&fst);
    assert(!fst.running && fst.duration_min == 25);
    assert(!app_focus_remote_start(20));
    assert(app_focus_remote_start(15));
    app_focus_get_status(&fst);
    assert(fst.running && !fst.is_break && fst.duration_min == 15 && fst.remaining_sec > 0);
    app_focus_remote_stop();
    app_focus_get_status(&fst);
    assert(!fst.running && fst.remaining_sec == 0);

    // 验证统一三键：Focus 非调时间态 KEY1 返回 NONE（交由主循环返回），不误停番茄钟或改动时长。
    // Unified keys: KEY1 outside time-edit returns NONE (global back) without killing timer or changing duration.
    assert(app_focus_remote_start(15));
    assert(app_focus.on_key(&ctx, UI_KEY_1) == APP_REDRAW_NONE);
    app_focus_get_status(&fst);
    assert(fst.running && fst.duration_min == 15);
    app_focus_remote_stop();

    puts("unified_clock & ui_time_edit: passed");
}

static void test_settings_real_loop_navigation(void) {
    uint8_t *fb = malloc(1216 * 684 / 2);
    uint8_t *back = malloc(1216 * 684 / 2);
    assert(fb && back);
    EpdiyHighlevelState hl = { .front_fb = fb, .back_fb = back };

    // 场景 1：进入设置 -> 点按第 0 项“网络与时间” -> 必须真正打开子页，而不是仅闪烁后停在首页！
    // Scenario 1: Enter Settings -> tap Row 0 (Network & Time) -> must open the sub-page instead of flashing and cancelling!
    reset_steps();
    push_idle();
    push_tap(300, UI_CONTENT_TOP + 40); // Row 0: 网络与时间
    push_idle();
    app_loop_config_t cfg = { .hl = &hl, .fb = fb, .first_app = &app_settings };
    if (!setjmp(s_loop_exit)) app_loop_run(&cfg);
    assert(strcmp(s_last_header, "网络与时间") == 0);

    // 场景 2：进入设置 -> 点按“显示与阅读” -> 点按“字号 +”与“顶部状态栏” -> 验证不含已删“翻页效果” -> 返回设置。
    // Scenario 2: Settings -> Display & Reading -> tap Size + and Top Bar -> verify removed turn animation is absent -> return to root.
    s_book_px = 48;
    s_book_top = true;
    s_book_ind = 0;
    reset_steps();
    push_idle();
    push_tap(300, UI_CONTENT_TOP + (UI_BTN_H + UI_GAP) + 40); // Row 1: 显示与阅读
    EpdRect size_plus = ui_row_rect(1, 2, UI_CONTENT_TOP + (UI_BTN_H + UI_GAP), UI_BTN_H);
    push_tap(size_plus.x + 40, size_plus.y + 40); // 字号 +
    EpdRect top_bar_btn = ui_row_rect(0, 2, UI_CONTENT_TOP + 6 * (UI_BTN_H + UI_GAP), UI_BTN_H);
    push_tap(top_bar_btn.x + 40, top_bar_btn.y + 40); // 顶部状态栏
    EpdRect indent_btn = ui_row_rect(1, 2, UI_CONTENT_TOP + 7 * (UI_BTN_H + UI_GAP), UI_BTN_H);
    push_tap(indent_btn.x + 40, indent_btn.y + 40); // 缩进
    push_tap(114, UI_KEY_HINT_TOP + 20); // 点击底部说明带左格“返回”回设置首页
    push_idle();
    if (!setjmp(s_loop_exit)) app_loop_run(&cfg);
    assert(s_book_px == 52);
    assert(s_book_top == false);
    assert(s_book_ind == 1);
    assert(!ui_saw_text("翻页效果"));
    assert(!ui_saw_text("返回设置"));
    assert(strcmp(s_last_header, "设置") == 0);

    // 场景 3：进入设置 -> 点按“电源与提示” -> 点按“全局蜂鸣静音” -> 静音状态反转。
    // Scenario 3: Settings -> Power & Alerts -> toggle global mute.
    s_muted = false;
    reset_steps();
    push_idle();
    push_tap(300, UI_CONTENT_TOP + 3 * (UI_BTN_H + UI_GAP) + 40); // Row 3: 电源与提示
    int power_btn_y = UI_CONTENT_TOP + UI_SEC_HEAD + UI_ROW_H + UI_SECTION_GAP;
    push_tap(300, power_btn_y + 2 * (UI_BTN_H + UI_GAP) + 40); // 全局蜂鸣静音
    push_idle();
    if (!setjmp(s_loop_exit)) app_loop_run(&cfg);
    assert(strcmp(s_last_header, "电源与提示") == 0);
    assert(s_muted == true);

    // 场景 4：进入设置 -> 点按“系统更新” -> 立即显示真实版本 v0.1.19 而非 v0.0.0 或 未知。
    // Scenario 4: Settings -> System Update -> shows real version v0.1.19 immediately.
    reset_steps();
    push_idle();
    push_tap(300, UI_CONTENT_TOP + 6 * (UI_BTN_H + UI_GAP) + 40); // Row 6: 系统更新
    push_idle();
    if (!setjmp(s_loop_exit)) app_loop_run(&cfg);
    assert(strcmp(s_last_header, "系统更新") == 0);
    assert(ui_saw_text("v0.1.19"));

    // 场景 5：设置 -> 网络与时间 -> 手动校准日期与时间 -> 年 + -> 保存时间 -> 返回网络与时间并显示 2027。
    // Scenario 5: Settings -> Network & Time -> Manual date/time -> Year + -> Save -> returns showing 2027.
    reset_steps();
    push_idle();
    push_tap(300, UI_CONTENT_TOP + 40); // Row 0: 网络与时间
    int net_btn_y = UI_CONTENT_TOP + UI_SEC_HEAD + 4 * UI_ROW_H + UI_SECTION_GAP;
    push_tap(300, net_btn_y + (UI_BTN_H + UI_GAP) + 40); // 手动校准日期与时间
    push_tap(540, 268 + 20); // 年 +
    EpdRect save_btn = ui_bar_rect(1, 2);
    push_tap(save_btn.x + 40, save_btn.y + 40); // 保存时间
    push_idle();
    if (!setjmp(s_loop_exit)) app_loop_run(&cfg);
    assert(strcmp(s_last_header, "网络与时间") == 0);
    assert(ui_saw_text("2027-11-22"));

    // 场景 6：设置 -> 网络与时间 -> 点按“蓝牙 BLE”与“蓝牙配对码”两个独立按钮。
    // Scenario 6: Settings -> Network & Time -> tap BLE toggle and BLE PIN toggle independently.
    s_ble_enabled = true;
    s_ble_pin_required = false;
    reset_steps();
    push_idle();
    push_tap(300, UI_CONTENT_TOP + 40); // Row 0: 网络与时间
    EpdRect ble_btn = ui_row_rect(0, 2, net_btn_y, UI_BTN_H);
    EpdRect pin_btn = ui_row_rect(1, 2, net_btn_y, UI_BTN_H);
    push_tap(pin_btn.x + 40, pin_btn.y + 40); // 开启蓝牙配对码
    push_tap(ble_btn.x + 40, ble_btn.y + 40); // 关闭蓝牙 BLE
    push_idle();
    if (!setjmp(s_loop_exit)) app_loop_run(&cfg);
    assert(strcmp(s_last_header, "网络与时间") == 0);
    assert(s_ble_pin_required == true);
    assert(s_ble_enabled == false);
    assert(ui_saw_text("蓝牙控制已关闭"));

    // 场景 7：设置空闲时不阻止休眠；OTA 检查/下载期间设置 keep_awake 且离页时自动取消 OTA。
    // Scenario 7: Settings allows idle sleep normally, keeps awake only during active OTA, and cancels OTA on exit.
    assert(!app_settings.blocks_idle_sleep);
    app_ctx_t sctx = { .fb = fb, .hl = &hl, .now_ms = 1000 };
    app_settings.on_enter(&sctx);
    s_ota.state = OTA_UPDATE_IDLE;
    sctx.keep_awake = false;
    app_settings.on_tick(&sctx);
    assert(!sctx.keep_awake);
    s_ota.state = OTA_UPDATE_DOWNLOADING;
    app_settings.on_tick(&sctx);
    assert(sctx.keep_awake);
    app_settings.on_exit(&sctx);
    assert(s_ota.state == OTA_UPDATE_CANCELLED);

    free(fb);
    free(back);
    puts("app_settings (via real app_loop + ui_gesture): passed");
}

static uint8_t fb_logical_nibble(const uint8_t *fb, int lx, int ly) {
    int px = 0, py = 0;
    continuous_du_from_logical(lx, ly, &px, &py);
    uint8_t byte = fb[py * (epd_width() / 2) + px / 2];
    return (px & 1) ? (uint8_t)(byte >> 4) : (uint8_t)(byte & 0x0F);
}

static void test_sketch_drag_and_coordinates(void) {
    uint8_t *fb = malloc(1216 * 684 / 2);
    uint8_t *back = malloc(1216 * 684 / 2);
    assert(fb && back);
    memset(fb, 0xFF, 1216 * 684 / 2);
    memset(back, 0xFF, 1216 * 684 / 2);
    EpdiyHighlevelState hl = { .front_fb = fb, .back_fb = back };

    s_du_marks = 0;
    s_du_scans = 0;
    s_du_pending = 0;

    // 模拟在画布内从 (120, 300) 连续拖动到 (220, 360) 再抬手，验证无坐标偏移且插值连成实线。
    // Simulate dragging from (120, 300) to (220, 360) then lifting; verify exact rotated coords and continuous line.
    reset_steps();
    push_idle();
    s_steps[s_step_count++] = (touch_step_t){ 120, 300, 1 };
    s_steps[s_step_count++] = (touch_step_t){ 170, 330, 1 };
    s_steps[s_step_count++] = (touch_step_t){ 220, 360, 1 };
    for (int i = 0; i < 8; ++i) push_idle();

    app_loop_config_t cfg = { .hl = &hl, .fb = fb, .first_app = &app_sketch };
    if (!setjmp(s_loop_exit)) app_loop_run(&cfg);

    assert(s_du_marks > 10);
    assert(s_du_scans >= 6);
    // 起点、中点、终点和插值点都必须精确落墨（0=黑），且远端空白处保持 15=白。
    // Start, midpoint, interpolated points and end must be inked (0=black), while untouched areas stay white (15).
    assert(fb_logical_nibble(fb, 120, 300) == 0);
    assert(fb_logical_nibble(fb, 145, 315) == 0);
    assert(fb_logical_nibble(fb, 170, 330) == 0);
    assert(fb_logical_nibble(fb, 195, 345) == 0);
    assert(fb_logical_nibble(fb, 220, 360) == 0);
    assert(fb_logical_nibble(fb, 500, 700) == 15);
    // back_fb 在连续 DU 扫完后应同步为最新画面。/ back_fb must sync once continuous DU drains.
    assert(fb_logical_nibble(back, 170, 330) == 0);

    // 点按“清空”应清除笔迹，再点按“撤销”应恢复。/ Clear erases strokes, Undo restores them.
    app_ctx_t sk_ctx = { .fb = fb, .hl = &hl, .now_ms = 2000 };
    EpdRect clear_btn = ui_row_rect(2, 6, UI_CONTENT_BOTTOM - 76, 76);
    cst836u_touch_t t = { .touched = true, .count = 1, .x = (uint16_t)(clear_btn.x + 10), .y = (uint16_t)(clear_btn.y + 10) };
    assert(app_sketch.on_touch(&sk_ctx, &t) == APP_REDRAW_FULL);
    app_sketch.render(&sk_ctx, fb);
    assert(fb_logical_nibble(fb, 170, 330) == 15);
    EpdRect undo_btn = ui_row_rect(1, 6, UI_CONTENT_BOTTOM - 76, 76);
    t.x = (uint16_t)(undo_btn.x + 10); t.y = (uint16_t)(undo_btn.y + 10);
    assert(app_sketch.on_touch(&sk_ctx, &t) == APP_REDRAW_FULL);
    app_sketch.render(&sk_ctx, fb);
    assert(fb_logical_nibble(fb, 170, 330) == 0);

    app_sketch.on_exit(&sk_ctx);
    free(fb);
    free(back);
    puts("app_sketch drag & coordinate mapping: passed");
}

static void test_personal_heatmap_and_lock_saver(void) {
    uint8_t *fb = malloc(1216 * 684 / 2);
    assert(fb);
    app_ctx_t ctx = { .fb = fb, .now_ms = 1000 };
    s_dash_items = APP_DASH_TIME | APP_DASH_BATTERY | APP_DASH_LAST_BOOK | APP_DASH_NOTE | APP_DASH_HEATMAP;
    s_saver = APP_SAVER_BUILTIN;

    // 1) 看板首页应渲染热力图卡片与连续/今日统计，且填充了有记录的方格。
    // Dashboard home renders heatmap card with streak/today stats and shaded cells.
    app_personal.on_enter(&ctx);
    s_filled_round_rects = 0;
    app_personal.render(&ctx, fb);
    assert(ui_saw_text("阅读热力图"));
    assert(ui_saw_text("连续 3 天 · 今日 30 分"));
    assert(ui_saw_text("近 26 周 · 已读 3 天"));
    assert(s_filled_round_rects >= 7); // 3 active cells + 4 legend filled swatches.

    // 2) 点按热力图卡片打开详情页（leaf 4），再点按「设为锁屏内容」生效。
    // Tapping heatmap card opens detail leaf 4; tapping Set as Lock Screen updates saver_style.
    int heatmap_y = UI_CONTENT_TOP + (130 + 10) + (94 + 10) + (128 + 10);
    cst836u_touch_t t = { .touched = true, .count = 1, .x = UI_MARGIN + 80, .y = (uint16_t)(heatmap_y + 60) };
    assert(app_personal.on_touch(&ctx, &t) == APP_REDRAW_PAGE);
    assert(ctx.leaf == 4);
    app_personal.render(&ctx, fb);
    assert(strcmp(s_last_header, "阅读热力图") == 0);
    assert(ui_saw_text("连续阅读") && ui_saw_text("最长连续") && ui_saw_text("今日阅读") && ui_saw_text("累计阅读"));
    assert(ui_saw_text("设为锁屏内容"));
    assert(!ui_saw_text("返回看板"));

    EpdRect lock_btn = ui_bar_rect(0, 1);
    t.x = (uint16_t)(lock_btn.x + 100);
    t.y = (uint16_t)(lock_btn.y + 30);
    assert(app_personal.on_touch(&ctx, &t) == APP_REDRAW_PAGE);
    assert(s_saver == APP_SAVER_HEATMAP);
    app_personal.render(&ctx, fb);
    assert(ui_saw_text("已设为锁屏内容"));

    // KEY1 返回看板主视图。/ KEY1 returns to dashboard root.
    assert(app_personal.on_key(&ctx, UI_KEY_1) == APP_REDRAW_PAGE);
    assert(ctx.leaf == 0);

    // 3) 打开「配置看板」（leaf 1），可开关阅读热力图卡片并切换 5 项锁屏内容。
    // Open dashboard settings (leaf 1): toggle heatmap item and select lock-screen styles.
    EpdRect cfg_btn = ui_bar_rect(0, 2);
    t.x = (uint16_t)(cfg_btn.x + 20);
    t.y = (uint16_t)(cfg_btn.y + 20);
    assert(app_personal.on_touch(&ctx, &t) == APP_REDRAW_PAGE);
    assert(ctx.leaf == 1);
    app_personal.render(&ctx, fb);
    assert(ui_saw_text("阅读热力图") && ui_saw_text("当前书封面"));

    // 点按第 4 个看板开关（阅读热力图：第二行第 0 列）。/ Tap 4th dashboard toggle (heatmap).
    EpdRect hm_toggle = ui_row_rect(0, 2, 188 + 76 + UI_GAP, 76);
    t.x = (uint16_t)(hm_toggle.x + 20);
    t.y = (uint16_t)(hm_toggle.y + 20);
    assert(app_personal.on_touch(&ctx, &t) == APP_REDRAW_PAGE);
    assert((s_dash_items & APP_DASH_HEATMAP) == 0);
    assert(app_personal.on_touch(&ctx, &t) == APP_REDRAW_PAGE);
    assert((s_dash_items & APP_DASH_HEATMAP) != 0);

    // 切换锁屏内容至默认图案再切回阅读热力图。/ Switch saver to builtin then back to heatmap.
    EpdRect builtin_btn = ui_row_rect(0, 3, 428, 76);
    t.x = (uint16_t)(builtin_btn.x + 20);
    t.y = (uint16_t)(builtin_btn.y + 20);
    assert(app_personal.on_touch(&ctx, &t) == APP_REDRAW_PAGE);
    assert(s_saver == APP_SAVER_BUILTIN);

    EpdRect hm_saver_btn = ui_row_rect(1, 2, 428 + 76 + UI_GAP, 76);
    t.x = (uint16_t)(hm_saver_btn.x + 20);
    t.y = (uint16_t)(hm_saver_btn.y + 20);
    assert(app_personal.on_touch(&ctx, &t) == APP_REDRAW_PAGE);
    assert(s_saver == APP_SAVER_HEATMAP);

    // 4) 全屏锁屏渲染输出完整标题、日期、统计、最近在读与品牌页脚。
    // Full-screen lock renderer outputs title, date, stats, recent book, and footer.
    reading_heatmap_t hm;
    uint32_t today = reading_stats_day_from_ymd(2026, 11, 25);
    assert(reading_stats_get_heatmap(today, &hm));
    s_filled_round_rects = 0;
    ui_heatmap_render_lockscreen(fb, &hm, 5400, "三体.epub", 65);
    assert(ui_saw_text("阅读热力图"));
    assert(ui_saw_text("2026-11-25"));
    assert(ui_saw_text("3 天") && ui_saw_text("5 天") && ui_saw_text("30 分钟") && ui_saw_text("1 小时 30 分"));
    assert(ui_saw_text("三体") && ui_saw_text("已读 65%") && ui_saw_text("小纸 Pico"));
    assert(s_filled_round_rects >= 7);

    // 5) 验证「设置 → 睡眠与屏保」可循环切到「阅读热力图」再回到「默认图案」。
    // Verify Settings -> Sleep & Screensaver cycles through APP_SAVER_HEATMAP and wraps to builtin.
    s_saver = APP_SAVER_BOOK_COVER;
    uint8_t *full_fb = malloc(1216 * 684 / 2);
    uint8_t *full_back = malloc(1216 * 684 / 2);
    assert(full_fb && full_back);
    EpdiyHighlevelState hl = { .front_fb = full_fb, .back_fb = full_back };
    reset_steps();
    push_idle();
    push_tap(300, UI_CONTENT_TOP + 2 * (UI_BTN_H + UI_GAP) + 40); // Row 2: 睡眠与屏保
    push_tap(300, UI_CONTENT_TOP + 2 * (UI_BTN_H + UI_GAP) + 40); // Row 2: 锁屏内容 (3 -> 4)
    push_idle();
    app_loop_config_t cfg = { .hl = &hl, .fb = full_fb, .first_app = &app_settings };
    if (!setjmp(s_loop_exit)) app_loop_run(&cfg);
    assert(s_saver == APP_SAVER_HEATMAP);
    assert(ui_saw_text("阅读热力图"));

    reset_steps();
    push_idle();
    push_tap(300, UI_CONTENT_TOP + 2 * (UI_BTN_H + UI_GAP) + 40); // Row 2: 睡眠与屏保
    push_tap(300, UI_CONTENT_TOP + 2 * (UI_BTN_H + UI_GAP) + 40); // Row 2: 锁屏内容 (4 -> 0)
    push_idle();
    if (!setjmp(s_loop_exit)) app_loop_run(&cfg);
    assert(s_saver == APP_SAVER_BUILTIN);
    free(full_fb);
    free(full_back);
    free(fb);

    puts("personal_heatmap & lock_saver: passed");
}

static void test_sntp_callback_and_pmu_drift_guard(void) {
    // 1) 开机从偏快 5 分钟的 PMU 恢复时间后连上 Wi-Fi：SNTP 未回包前绝不可提前写回旧时间或锁死 s_synced_this_boot。
    // Boot from a +5 min drifted PMU clock and join Wi-Fi: poll before NTP reply must neither echo stale time nor lock out SNTP.
    uint32_t real_utc = 0;
    assert(app_time_make_utc(2026, 11, 25, 22, 0, 480, &real_utc));
    uint32_t drifted_pmu = real_utc + 300U;
    s_fake_sys_time = 0;
    s_pmu.time_ok = true;
    s_pmu.unix_sec = drifted_pmu;
    s_pmu.time_synced = 1;
    s_pmu_time_sync_fail = false;
    s_sntp_init_calls = 0;
    s_sntp_deinit_calls = 0;
    s_fake_timer_offset_us = 1000000LL;

    app_time_sync_from_pmu();
    assert(s_fake_sys_time == (time_t)drifted_pmu);

    int sync_before = s_pmu_time_sync_calls;
    app_time_sync_sta_ip();
    assert(s_sntp_init_calls == 1);
    assert(s_sntp_cb != NULL);
    app_time_sync_poll();
    assert(s_pmu_time_sync_calls == sync_before);
    assert(s_pmu.unix_sec == drifted_pmu);

    // 离线热点断开后再重连，仍可重新启动 SNTP。/ Stopping before NTP reply allows a subsequent STA connect to retry SNTP.
    app_time_sync_stop();
    assert(s_sntp_deinit_calls == 1);
    app_time_sync_sta_ip();
    assert(s_sntp_init_calls == 2);
    assert(s_sntp_cb != NULL);

    // 2) 收到真实 NTP 响应后，poll 把准确时间写回 PMU。/ Once NTP callback fires, poll flushes accurate UTC to PMU.
    s_fake_sys_time = (time_t)real_utc;
    struct timeval tv = { .tv_sec = (time_t)real_utc, .tv_usec = 0 };
    s_sntp_cb(&tv);
    app_time_sync_poll();
    assert(s_pmu_time_sync_calls == sync_before + 1);
    assert(s_pmu.unix_sec == real_utc);

    // 3) 清醒期间 CW32 LSI 漂移快了 3 分钟，切换页面调用 app_time_sync_from_pmu() 绝不反向污染 ESP32 晶振时钟。
    // If CW32 LSI drifts +3 min while awake, app_time_sync_from_pmu() must never overwrite the valid ESP32 clock.
    s_pmu.unix_sec = real_utc + 180U;
    app_time_sync_from_pmu();
    assert(s_fake_sys_time == (time_t)real_utc);

    // 4) 下电前 app_time_flush_to_pmu() 把准确的 ESP32 时间回写 PMU，清掉清醒期 LSI 漂移。
    // Pre-sleep app_time_flush_to_pmu() writes accurate host time back to PMU, clearing awake LSI drift.
    assert(app_time_flush_to_pmu());
    assert(s_pmu.unix_sec == real_utc);

    // 5) 30 分钟后再次联网允许重新启动 SNTP；若 NTP 回包后立刻退网且首次写 PMU 失败，切页时由 app_time_sync_from_pmu() 补写。
    // After 30 min awake, STA reconnect re-arms SNTP; if stop-time PMU write fails, app_time_sync_from_pmu() flushes it without overwriting host.
    app_time_sync_stop();
    app_time_sync_sta_ip();
    assert(s_sntp_init_calls == 2); // 刚同步过 30 分钟内不重复启动 / Skipped within 30 min of last sync.

    s_fake_timer_offset_us += 31LL * 60LL * 1000000LL;
    app_time_sync_sta_ip();
    assert(s_sntp_init_calls == 3);
    assert(s_sntp_cb != NULL);

    uint32_t next_utc = real_utc + 1860U;
    s_fake_sys_time = (time_t)next_utc;
    s_pmu.unix_sec = next_utc + 240U; // 模拟 PMU 又偏快 / Simulate drifted PMU again.
    tv.tv_sec = (time_t)next_utc;
    s_sntp_cb(&tv);

    s_pmu_time_sync_fail = true;
    app_time_sync_stop();
    assert(s_pmu.unix_sec == next_utc + 240U);

    s_pmu_time_sync_fail = false;
    app_time_sync_from_pmu();
    assert(s_fake_sys_time == (time_t)next_utc);
    assert(s_pmu.unix_sec == next_utc);

    s_fake_timer_offset_us = 0;
    puts("sntp_callback & pmu_drift_guard: passed");
}

int main(void) {
    test_time_math_and_unified_clock();
    test_settings_real_loop_navigation();
    test_sketch_drag_and_coordinates();
    test_personal_heatmap_and_lock_saver();
    test_sntp_callback_and_pmu_drift_guard();
    puts("ALL ui_fixes host regression tests passed!");
    return 0;
}
