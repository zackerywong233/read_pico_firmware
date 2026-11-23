/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * NVS 读写。打开失败就用深睡默认值，不擦除整个分区。
 *
 * NVS load/store. A failed open keeps the soft-sleep default; the
 * partition is not erased.
 */

#include "settings.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "read_pico_pmu.h"
#include "read_pico_buzzer.h"

#define TAG "settings"
#define NVS_NS "read_pico"
#define NVS_KEY_SLEEP "sleep"
#define NVS_KEY_AUTO_SLEEP "idle_s"
#define NVS_KEY_FONT "font"
#define NVS_KEY_WAKE "lwake"
#define NVS_KEY_BOOT "lboot"
#define NVS_KEY_PICKUP "pickup"
#define NVS_KEY_BOOK_PX "bk_px"
#define NVS_KEY_BOOK_SHAKE "bk_shake"
#define NVS_KEY_BOOK_GESTURE "bk_gest"
#define NVS_KEY_BOOK_REFRESH "bk_ref"
#define NVS_KEY_BOOK_LINE_HEIGHT "bk_lh"
#define NVS_KEY_BOOK_MARGIN "bk_mg"
#define NVS_KEY_BOOK_WEIGHT "bk_wght"
#define NVS_KEY_BOOK_ROTATION "bk_rot"
#define NVS_KEY_BOOK_TOP "bk_top"
#define NVS_KEY_BOOK_BOT "bk_bot"
#define NVS_KEY_BOOK_PARA "bk_para"
#define NVS_KEY_BOOK_INDENT "bk_ind"
#define NVS_KEY_BOOK_SHELF "bk_shelf"
#define NVS_KEY_BOOK_CBZ_DIR "bk_cbz_dir"
#define NVS_KEY_BOOK_CBZ_DITH "bk_cbz_dith"
#define NVS_KEY_SAVER "saver"
#define NVS_KEY_DASH "dash"
#define NVS_KEY_TZ_MIN "tz_min"
#define NVS_KEY_CHARGE_LED "chg_led"
#define NVS_KEY_LOW_SOC "low_soc"
#define NVS_KEY_MUTE "mute"
#define NVS_KEY_TRANSFER_PIN "tr_pin"
#define NVS_KEY_BLE_EN "ble_en"
#define NVS_KEY_BLE_PIN "ble_pin"
#define FONT_PATH_MAX 160
#define AMBIENT_NS "rp_ambient"
#define AMBIENT_KEY_ENABLED "enabled"
#define AMBIENT_KEY_INTERVAL "interval_min"
#define AMBIENT_KEY_QUIET_START "quiet_start_h"
#define AMBIENT_KEY_QUIET_END "quiet_end_h"
#define AMBIENT_KEY_SOURCE "source_type"
#define AMBIENT_KEY_URL "url"
#define AMBIENT_KEY_AUTH "auth_header"
#define AMBIENT_KEY_LAST_OK "last_ok"
#define AMBIENT_KEY_LAST_TS "last_ts"

static app_ambient_settings_t s_ambient = {
    .enabled = false, .interval_min = 60, .quiet_start_h = 23,
    .quiet_end_h = 7, .source_type = 2,
};
static bool s_ambient_has_result;
static bool s_ambient_last_success;
static uint32_t s_ambient_last_ts;

static app_sleep_mode_t s_sleep = APP_SLEEP_DEEP;
static uint16_t s_auto_sleep_sec = 120;
static char s_font[FONT_PATH_MAX];
static uint8_t s_last_wake;
static uint8_t s_last_boot;
static bool s_pickup_wake;
static uint8_t s_book_px = 48;
static bool s_book_shake;
static uint8_t s_book_gesture;
static uint8_t s_book_refresh = 1;
static uint8_t s_book_line_height = 1;
static uint8_t s_book_margin = 1;
static uint16_t s_book_weight;
static uint8_t s_book_rotation;
static bool s_book_top = true;
static bool s_book_bot = true;
static uint8_t s_book_para = 1;
static uint8_t s_book_indent;
static uint8_t s_book_shelf;
static uint8_t s_book_cbz_dir;
static bool s_book_cbz_dith = true;
static app_saver_style_t s_saver = APP_SAVER_BUILTIN;
static uint8_t s_dashboard = APP_DASH_TIME | APP_DASH_BATTERY | APP_DASH_LAST_BOOK | APP_DASH_NOTE;
static int16_t s_tz_min = 480;
static bool s_charge_led = true;
static uint8_t s_low_soc = 10;
static bool s_muted;
static bool s_transfer_pin_required;
static bool s_ble_enabled = true;
static bool s_ble_pin_required;

static void apply_timezone(void) {
    int minutes = s_tz_min;
    char value[24];
    snprintf(value, sizeof(value), "UTC%c%d:%02d", minutes <= 0 ? '+' : '-',
             abs(minutes) / 60, abs(minutes) % 60);
    setenv("TZ", value, 1);
    tzset();
}

static uint8_t valid_book_px(uint8_t px) {
    return px >= 36 && px <= 72 && (px - 36) % 4 == 0 ? px : 48;
}

static bool valid_auto_sleep_sec(uint16_t seconds) {
    switch (seconds) {
        case 0:
        case 30:
        case 60:
        case 120:
        case 300:
        case 600:
        case 900:
        case 1800:
        case 3600:
            return true;
        default:
            return false;
    }
}

void app_settings_init(void) {
    apply_timezone();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs init %s, use soft-sleep default", esp_err_to_name(err));
        return;
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t raw = APP_SLEEP_DEEP;
    if (nvs_get_u8(h, NVS_KEY_SLEEP, &raw) == ESP_OK && raw <= APP_SLEEP_OFF) {
        s_sleep = (app_sleep_mode_t)raw;
    }
    uint16_t auto_sleep = 120;
    if (nvs_get_u16(h, NVS_KEY_AUTO_SLEEP, &auto_sleep) == ESP_OK
        && valid_auto_sleep_sec(auto_sleep)) {
        s_auto_sleep_sec = auto_sleep;
    }
    size_t font_len = sizeof(s_font);
    if (nvs_get_str(h, NVS_KEY_FONT, s_font, &font_len) != ESP_OK) {
        s_font[0] = '\0';
    }
    uint8_t wake = 0;
    if (nvs_get_u8(h, NVS_KEY_WAKE, &wake) == ESP_OK) s_last_wake = wake;
    uint8_t boot = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOT, &boot) == ESP_OK) s_last_boot = boot;
    uint8_t pickup = 0;
    if (nvs_get_u8(h, NVS_KEY_PICKUP, &pickup) == ESP_OK) s_pickup_wake = pickup != 0;
    uint8_t book_px = 48, book_shake = 0, book_gesture = 0, saver = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOK_PX, &book_px) == ESP_OK) s_book_px = valid_book_px(book_px);
    if (nvs_get_u8(h, NVS_KEY_BOOK_SHAKE, &book_shake) == ESP_OK) s_book_shake = book_shake != 0;
    if (nvs_get_u8(h, NVS_KEY_BOOK_GESTURE, &book_gesture) == ESP_OK && book_gesture <= 2)
        s_book_gesture = book_gesture;
    else if (s_book_shake) s_book_gesture = 1;
    s_book_shake = s_book_gesture == 1;
    uint8_t book_refresh = 1, book_line_height = 1, book_margin = 1, book_rotation = 0;
    uint16_t book_weight = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOK_REFRESH, &book_refresh) == ESP_OK && book_refresh <= 2)
        s_book_refresh = book_refresh;
    if (nvs_get_u8(h, NVS_KEY_BOOK_LINE_HEIGHT, &book_line_height) == ESP_OK && book_line_height <= 2)
        s_book_line_height = book_line_height;
    if (nvs_get_u8(h, NVS_KEY_BOOK_MARGIN, &book_margin) == ESP_OK && book_margin <= 2)
        s_book_margin = book_margin;
    if (nvs_get_u16(h, NVS_KEY_BOOK_WEIGHT, &book_weight) == ESP_OK &&
        (book_weight == 0 || (book_weight >= 300 && book_weight <= 800 && (book_weight - 300) % 100 == 0)))
        s_book_weight = book_weight;
    if (nvs_get_u8(h, NVS_KEY_BOOK_ROTATION, &book_rotation) == ESP_OK && book_rotation <= 2)
        s_book_rotation = book_rotation;
    uint8_t bk_u8 = 0;
    if (nvs_get_u8(h, NVS_KEY_BOOK_TOP, &bk_u8) == ESP_OK) s_book_top = bk_u8 != 0;
    if (nvs_get_u8(h, NVS_KEY_BOOK_BOT, &bk_u8) == ESP_OK) s_book_bot = bk_u8 != 0;
    if (nvs_get_u8(h, NVS_KEY_BOOK_PARA, &bk_u8) == ESP_OK && bk_u8 <= 2) s_book_para = bk_u8;
    if (nvs_get_u8(h, NVS_KEY_BOOK_INDENT, &bk_u8) == ESP_OK && bk_u8 <= 1) s_book_indent = bk_u8;
    if (nvs_get_u8(h, NVS_KEY_BOOK_SHELF, &bk_u8) == ESP_OK && bk_u8 <= 1) s_book_shelf = bk_u8;
    if (nvs_get_u8(h, NVS_KEY_BOOK_CBZ_DIR, &bk_u8) == ESP_OK && bk_u8 <= 1) s_book_cbz_dir = bk_u8;
    if (nvs_get_u8(h, NVS_KEY_BOOK_CBZ_DITH, &bk_u8) == ESP_OK) s_book_cbz_dith = bk_u8 != 0;
    if (nvs_get_u8(h, NVS_KEY_SAVER, &saver) == ESP_OK && saver <= APP_SAVER_BOOK_COVER) s_saver = (app_saver_style_t)saver;
    if (nvs_get_u8(h, NVS_KEY_DASH, &s_dashboard) != ESP_OK) {
        s_dashboard = APP_DASH_TIME | APP_DASH_BATTERY | APP_DASH_LAST_BOOK | APP_DASH_NOTE;
    }
    int16_t tz = 480;
    if (nvs_get_i16(h, NVS_KEY_TZ_MIN, &tz) == ESP_OK && tz >= -720 && tz <= 840) s_tz_min = tz;
    uint8_t led = 1, low = 10, mute = 0, tr_pin = 0, ble_en = 1, ble_pin = 0;
    if (nvs_get_u8(h, NVS_KEY_CHARGE_LED, &led) == ESP_OK) s_charge_led = led != 0;
    if (nvs_get_u8(h, NVS_KEY_LOW_SOC, &low) == ESP_OK && (low == 0 || (low >= 5 && low <= 30 && low % 5 == 0))) s_low_soc = low;
    if (nvs_get_u8(h, NVS_KEY_MUTE, &mute) == ESP_OK) s_muted = mute != 0;
    if (nvs_get_u8(h, NVS_KEY_TRANSFER_PIN, &tr_pin) == ESP_OK) s_transfer_pin_required = tr_pin != 0;
    if (nvs_get_u8(h, NVS_KEY_BLE_EN, &ble_en) == ESP_OK) s_ble_enabled = ble_en != 0;
    if (nvs_get_u8(h, NVS_KEY_BLE_PIN, &ble_pin) == ESP_OK) s_ble_pin_required = ble_pin != 0;
    nvs_close(h);
    if (nvs_open(AMBIENT_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t enabled = 0, start = 23, end = 7, source = 2, last_ok = 0;
        uint16_t interval = 60;
        nvs_get_u8(h, AMBIENT_KEY_ENABLED, &enabled);
        nvs_get_u16(h, AMBIENT_KEY_INTERVAL, &interval);
        nvs_get_u8(h, AMBIENT_KEY_QUIET_START, &start);
        nvs_get_u8(h, AMBIENT_KEY_QUIET_END, &end);
        nvs_get_u8(h, AMBIENT_KEY_SOURCE, &source);
        size_t len = sizeof(s_ambient.url);
        (void)nvs_get_str(h, AMBIENT_KEY_URL, s_ambient.url, &len);
        len = sizeof(s_ambient.auth_header);
        (void)nvs_get_str(h, AMBIENT_KEY_AUTH, s_ambient.auth_header, &len);
        (void)nvs_get_u8(h, AMBIENT_KEY_LAST_OK, &last_ok);
        (void)nvs_get_u32(h, AMBIENT_KEY_LAST_TS, &s_ambient_last_ts);
        s_ambient_has_result = s_ambient_last_ts != 0;
        s_ambient_last_success = last_ok != 0;
        s_ambient.enabled = enabled != 0;
        if (interval == 15 || interval == 30 || interval == 60 || interval == 120 ||
            interval == 360 || interval == 720 || interval == 1440) s_ambient.interval_min = interval;
        if (start < 24) s_ambient.quiet_start_h = start;
        if (end < 24) s_ambient.quiet_end_h = end;
        if (source <= 2) s_ambient.source_type = source;
        nvs_close(h);
    }
    apply_timezone();
    ESP_LOGI(
        TAG, "sleep mode %s, auto lock %u s, font %s",
        app_sleep_mode_name(s_sleep),
        (unsigned)s_auto_sleep_sec,
        s_font[0] != '\0' ? s_font : "(builtin)"
    );
}

static bool ambient_interval_valid(uint16_t v) {
    return v == 15 || v == 30 || v == 60 || v == 120 || v == 360 || v == 720 || v == 1440;
}

void app_settings_ambient_get(app_ambient_settings_t* out) {
    nvs_handle_t h;
    if (nvs_open(AMBIENT_NS, NVS_READONLY, &h) == ESP_OK) {
        app_ambient_settings_t next = {.interval_min = 60, .quiet_start_h = 23,
                                       .quiet_end_h = 7, .source_type = 2};
        uint8_t enabled = 0, start = 23, end = 7, source = 2;
        (void)nvs_get_u8(h, AMBIENT_KEY_ENABLED, &enabled);
        (void)nvs_get_u16(h, AMBIENT_KEY_INTERVAL, &next.interval_min);
        (void)nvs_get_u8(h, AMBIENT_KEY_QUIET_START, &start);
        (void)nvs_get_u8(h, AMBIENT_KEY_QUIET_END, &end);
        (void)nvs_get_u8(h, AMBIENT_KEY_SOURCE, &source);
        size_t len = sizeof(next.url); (void)nvs_get_str(h, AMBIENT_KEY_URL, next.url, &len);
        len = sizeof(next.auth_header); (void)nvs_get_str(h, AMBIENT_KEY_AUTH, next.auth_header, &len);
        nvs_close(h);
        next.enabled = enabled != 0;
        if (!ambient_interval_valid(next.interval_min)) next.interval_min = 60;
        next.quiet_start_h = start < 24 ? start : 23;
        next.quiet_end_h = end < 24 ? end : 7;
        next.source_type = source <= 2 ? source : 2;
        s_ambient = next;
    }
    if (out) *out = s_ambient;
}

bool app_settings_ambient_set(const app_ambient_settings_t* config) {
    if (!config || !ambient_interval_valid(config->interval_min) || config->quiet_start_h > 23 ||
        config->quiet_end_h > 23 || config->source_type > 2 ||
        strnlen(config->url, sizeof(config->url)) > 256 ||
        strnlen(config->auth_header, sizeof(config->auth_header)) > 256) return false;
    nvs_handle_t h;
    if (nvs_open(AMBIENT_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(h, AMBIENT_KEY_ENABLED, config->enabled);
    if (err == ESP_OK) err = nvs_set_u16(h, AMBIENT_KEY_INTERVAL, config->interval_min);
    if (err == ESP_OK) err = nvs_set_u8(h, AMBIENT_KEY_QUIET_START, config->quiet_start_h);
    if (err == ESP_OK) err = nvs_set_u8(h, AMBIENT_KEY_QUIET_END, config->quiet_end_h);
    if (err == ESP_OK) err = nvs_set_u8(h, AMBIENT_KEY_SOURCE, config->source_type);
    if (err == ESP_OK) err = nvs_set_str(h, AMBIENT_KEY_URL, config->url);
    if (err == ESP_OK) err = nvs_set_str(h, AMBIENT_KEY_AUTH, config->auth_header);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) return false;
    s_ambient = *config;
    return true;
}

void app_settings_ambient_result(bool success, uint32_t unix_seconds) {
    nvs_handle_t h;
    if (nvs_open(AMBIENT_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u8(h, AMBIENT_KEY_LAST_OK, success ? 1 : 0) == ESP_OK &&
            nvs_set_u32(h, AMBIENT_KEY_LAST_TS, unix_seconds) == ESP_OK && nvs_commit(h) == ESP_OK) {
            s_ambient_has_result = true; s_ambient_last_success = success; s_ambient_last_ts = unix_seconds;
        }
        nvs_close(h);
    }
}

void app_settings_ambient_result_get(bool* has_result, bool* success, uint32_t* unix_seconds) {
    if (has_result) *has_result = s_ambient_has_result;
    if (success) *success = s_ambient_last_success;
    if (unix_seconds) *unix_seconds = s_ambient_last_ts;
}

app_sleep_mode_t app_settings_sleep_mode(void) {
    return s_sleep;
}

const char* app_sleep_mode_name(app_sleep_mode_t mode) {
    switch (mode) {
        case APP_SLEEP_LIGHT: return "light";
        case APP_SLEEP_DEEP: return "deep";
        case APP_SLEEP_OFF: return "off";
        default: return "?";
    }
}

void app_settings_set_sleep_mode(app_sleep_mode_t mode) {
    if (mode > APP_SLEEP_OFF) mode = APP_SLEEP_OFF;
    s_sleep = mode;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_SLEEP, (uint8_t)mode);
    nvs_commit(h);
    nvs_close(h);
}

uint16_t app_settings_auto_sleep_sec(void) {
    return s_auto_sleep_sec;
}

void app_settings_set_auto_sleep_sec(uint16_t seconds) {
    if (!valid_auto_sleep_sec(seconds) || s_auto_sleep_sec == seconds) return;
    s_auto_sleep_sec = seconds;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, NVS_KEY_AUTO_SLEEP, seconds);
    nvs_commit(h);
    nvs_close(h);
}

const char* app_settings_font_path(void) {
    return s_font;
}

static void nvs_put_u8(const char* key, uint8_t value) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, key, value);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t app_settings_last_wake(void) {
    return s_last_wake;
}

void app_settings_set_last_wake(uint8_t src) {
    if (src == s_last_wake) return;
    s_last_wake = src;
    nvs_put_u8(NVS_KEY_WAKE, src);
}

uint8_t app_settings_last_boot(void) {
    return s_last_boot;
}

void app_settings_set_last_boot(uint8_t reason) {
    if (reason == 0 || reason == s_last_boot) return;
    s_last_boot = reason;
    nvs_put_u8(NVS_KEY_BOOT, reason);
}

bool app_settings_pickup_wake(void) {
    return s_pickup_wake;
}

void app_settings_set_pickup_wake(bool on) {
    if (s_pickup_wake == on) return;
    s_pickup_wake = on;
    nvs_put_u8(NVS_KEY_PICKUP, on ? 1 : 0);
}

void app_settings_set_font_path(const char* path) {
    if (path == NULL) path = "";
    strlcpy(s_font, path, sizeof(s_font));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_FONT, s_font);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t app_settings_book_px(void) {
    return s_book_px;
}

void app_settings_set_book_px(uint8_t px) {
    px = valid_book_px(px);
    if (s_book_px == px) return;
    s_book_px = px;
    nvs_put_u8(NVS_KEY_BOOK_PX, px);
}

bool app_settings_book_shake(void) {
    return s_book_shake;
}

void app_settings_set_book_shake(bool on) {
    if (s_book_shake == on && (on || s_book_gesture == 0)) return;
    s_book_shake = on;
    s_book_gesture = on ? 1 : 0;
    nvs_put_u8(NVS_KEY_BOOK_SHAKE, on ? 1 : 0);
    nvs_put_u8(NVS_KEY_BOOK_GESTURE, s_book_gesture);
}

uint8_t app_settings_book_refresh(void) { return s_book_refresh; }
void app_settings_set_book_refresh(uint8_t profile) {
    if (profile > 2 || s_book_refresh == profile) return;
    s_book_refresh = profile;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_BOOK_REFRESH, profile);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t app_settings_book_line_height(void) { return s_book_line_height; }
void app_settings_set_book_line_height(uint8_t preset) {
    if (preset > 2 || s_book_line_height == preset) return;
    s_book_line_height = preset;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_BOOK_LINE_HEIGHT, preset);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t app_settings_book_margin(void) { return s_book_margin; }
void app_settings_set_book_margin(uint8_t preset) {
    if (preset > 2 || s_book_margin == preset) return;
    s_book_margin = preset;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_BOOK_MARGIN, preset);
    nvs_commit(h);
    nvs_close(h);
}

uint16_t app_settings_book_weight(void) { return s_book_weight; }
void app_settings_set_book_weight(uint16_t weight) {
    if (weight != 0 && (weight < 300 || weight > 800 || (weight - 300) % 100 != 0)) return;
    if (s_book_weight == weight) return;
    s_book_weight = weight;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, NVS_KEY_BOOK_WEIGHT, weight);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t app_settings_book_rotation(void) { return s_book_rotation; }
void app_settings_set_book_rotation(uint8_t rotation) {
    if (rotation > 2 || s_book_rotation == rotation) return;
    s_book_rotation = rotation;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_BOOK_ROTATION, rotation);
    nvs_commit(h);
    nvs_close(h);
}

uint8_t app_settings_book_gesture(void) { return s_book_gesture; }
void app_settings_set_book_gesture(uint8_t gesture) {
    if (gesture > 2 || s_book_gesture == gesture) return;
    s_book_gesture = gesture;
    s_book_shake = gesture == 1;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY_BOOK_GESTURE, gesture);
    nvs_set_u8(h, NVS_KEY_BOOK_SHAKE, gesture == 1);
    nvs_commit(h);
    nvs_close(h);
}

bool app_settings_book_top_bar(void) { return s_book_top; }
void app_settings_set_book_top_bar(bool on) {
    if (s_book_top == on) return;
    s_book_top = on;
    nvs_put_u8(NVS_KEY_BOOK_TOP, on ? 1 : 0);
}
bool app_settings_book_bottom_bar(void) { return s_book_bot; }
void app_settings_set_book_bottom_bar(bool on) {
    if (s_book_bot == on) return;
    s_book_bot = on;
    nvs_put_u8(NVS_KEY_BOOK_BOT, on ? 1 : 0);
}
uint8_t app_settings_book_para_gap(void) { return s_book_para; }
void app_settings_set_book_para_gap(uint8_t preset) {
    if (preset > 2 || s_book_para == preset) return;
    s_book_para = preset;
    nvs_put_u8(NVS_KEY_BOOK_PARA, preset);
}
uint8_t app_settings_book_indent(void) { return s_book_indent; }
void app_settings_set_book_indent(uint8_t mode) {
    if (mode > 1 || s_book_indent == mode) return;
    s_book_indent = mode;
    nvs_put_u8(NVS_KEY_BOOK_INDENT, mode);
}
uint8_t app_settings_book_shelf_view(void) { return s_book_shelf; }
void app_settings_set_book_shelf_view(uint8_t view) {
    if (view > 1 || s_book_shelf == view) return;
    s_book_shelf = view;
    nvs_put_u8(NVS_KEY_BOOK_SHELF, view);
}
uint8_t app_settings_book_cbz_dir(void) { return s_book_cbz_dir; }
void app_settings_set_book_cbz_dir(uint8_t dir) {
    if (dir > 1 || s_book_cbz_dir == dir) return;
    s_book_cbz_dir = dir;
    nvs_put_u8(NVS_KEY_BOOK_CBZ_DIR, dir);
}
bool app_settings_book_cbz_dither(void) { return s_book_cbz_dith; }
void app_settings_set_book_cbz_dither(bool on) {
    if (s_book_cbz_dith == on) return;
    s_book_cbz_dith = on;
    nvs_put_u8(NVS_KEY_BOOK_CBZ_DITH, on ? 1 : 0);
}

app_saver_style_t app_settings_saver_style(void) { return s_saver; }

void app_settings_set_saver_style(app_saver_style_t style) {
    if (style > APP_SAVER_BOOK_COVER) style = APP_SAVER_BUILTIN;
    if (s_saver == style) return;
    s_saver = style;
    nvs_put_u8(NVS_KEY_SAVER, (uint8_t)style);
}

uint8_t app_settings_dashboard_items(void) { return s_dashboard; }

void app_settings_set_dashboard_items(uint8_t items) {
    const uint8_t valid = APP_DASH_TIME | APP_DASH_BATTERY | APP_DASH_LAST_BOOK | APP_DASH_NOTE;
    items &= valid;
    if (s_dashboard == items) return;
    s_dashboard = items;
    nvs_put_u8(NVS_KEY_DASH, items);
}

int16_t app_settings_timezone_min(void) { return s_tz_min; }
void app_settings_set_timezone_min(int16_t minutes) {
    if (minutes < -720 || minutes > 840 || s_tz_min == minutes) return;
    s_tz_min = minutes;
    apply_timezone();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i16(h, NVS_KEY_TZ_MIN, minutes); nvs_commit(h); nvs_close(h);
}
bool app_settings_charge_led(void) { return s_charge_led; }
void app_settings_set_charge_led(bool enabled) {
    if (s_charge_led == enabled) return;
    s_charge_led = enabled; nvs_put_u8(NVS_KEY_CHARGE_LED, enabled ? 1 : 0);
    uint8_t value = enabled ? 1 : 0;
    (void)read_pico_pmu_cmd(PMU_CMD_CONFIG_SET_CHARGE_LED, &value, 1);
}
uint8_t app_settings_low_soc_notify_percent(void) { return s_low_soc; }
void app_settings_set_low_soc_notify_percent(uint8_t percent) {
    if ((percent && (percent < 5 || percent > 30 || percent % 5)) || s_low_soc == percent) return;
    s_low_soc = percent; nvs_put_u8(NVS_KEY_LOW_SOC, percent);
    uint8_t value[3] = {percent ? 1 : 0, (uint8_t)(percent * 10), 0};
    (void)read_pico_pmu_cmd(PMU_CMD_CONFIG_SET_LOW_SOC_NOTIFY, value, sizeof(value));
}
bool app_settings_muted(void) { return s_muted; }
void app_settings_set_muted(bool muted) {
    if (s_muted == muted) return;
    s_muted = muted; nvs_put_u8(NVS_KEY_MUTE, muted ? 1 : 0);
    read_pico_buzzer_set_muted(muted);
}
bool app_settings_transfer_pin_required(void) { return s_transfer_pin_required; }
void app_settings_set_transfer_pin_required(bool required) {
    if (s_transfer_pin_required == required) return;
    s_transfer_pin_required = required;
    nvs_put_u8(NVS_KEY_TRANSFER_PIN, required ? 1 : 0);
}
bool app_settings_ble_enabled(void) { return s_ble_enabled; }
void app_settings_set_ble_enabled(bool enabled) {
    if (s_ble_enabled == enabled) return;
    s_ble_enabled = enabled;
    nvs_put_u8(NVS_KEY_BLE_EN, enabled ? 1 : 0);
}
bool app_settings_ble_pin_required(void) { return s_ble_pin_required; }
void app_settings_set_ble_pin_required(bool required) {
    if (s_ble_pin_required == required) return;
    s_ble_pin_required = required;
    nvs_put_u8(NVS_KEY_BLE_PIN, required ? 1 : 0);
}

void app_settings_sync_pmu(void) {
    uint8_t led = s_charge_led ? 1 : 0;
    uint8_t low[3] = {s_low_soc ? 1 : 0, (uint8_t)(s_low_soc * 10), 0};
    (void)read_pico_pmu_cmd(PMU_CMD_CONFIG_SET_CHARGE_LED, &led, 1);
    (void)read_pico_pmu_cmd(PMU_CMD_CONFIG_SET_LOW_SOC_NOTIFY, low, sizeof(low));
    read_pico_buzzer_set_muted(s_muted);
}

void app_settings_set_timezone_min_deferred(int16_t minutes) {
    if (minutes < -720 || minutes > 840 || s_tz_min == minutes) return;
    s_tz_min = minutes;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i16(h, NVS_KEY_TZ_MIN, minutes);
    nvs_commit(h);
    nvs_close(h);
}

void app_settings_set_charge_led_deferred(bool enabled) {
    if (s_charge_led == enabled) return;
    s_charge_led = enabled;
    nvs_put_u8(NVS_KEY_CHARGE_LED, enabled ? 1 : 0);
}

void app_settings_set_low_soc_notify_percent_deferred(uint8_t percent) {
    if ((percent && (percent < 5 || percent > 30 || percent % 5)) || s_low_soc == percent) return;
    s_low_soc = percent;
    nvs_put_u8(NVS_KEY_LOW_SOC, percent);
}

void app_settings_apply_runtime(void) {
    apply_timezone();
    app_settings_sync_pmu();
}
