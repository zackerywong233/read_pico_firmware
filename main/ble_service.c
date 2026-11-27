/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：Pico OS 蓝牙配套控制服务实现：连接 read_pico_ble 与 Wi-Fi 配网、远程传书、对时、设置和短文本直传。
 *
 * English: Pico OS BLE companion service bridging read_pico_ble with Wi-Fi provisioning, remote transfer, time sync, settings and short memos.
 *
 * 冻结：NimBLE 回调不触碰 I2C/NVS/文件系统；Wi-Fi 密码与配对码不回显；VCOM 永不暴露。
 * Frozen: NimBLE callbacks never touch I2C, NVS or filesystems; never echo Wi-Fi passwords or PINs; never expose VCOM.
 */
#include "ble_service.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "app_focus.h"
#include "app_personal.h"
#include "app_transfer.h"
#include "app_web.h"
#include "book_store.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "ota_update.h"
#include "read_pico_ble.h"
#include "read_pico_ble_framing.h"
#include "read_pico_ble_protocol.h"
#include "read_pico_companion_policy.h"
#include "read_pico_pmu.h"
#include "read_pico_sd.h"
#include "read_pico_transfer.h"
#include "reading_stats.h"
#include "settings.h"
#include "time_sync.h"
#include "web_bookmarks.h"

extern const app_desc_t app_pico_home;
extern const app_desc_t app_book;
extern const app_desc_t app_feeds;
extern const app_desc_t app_files;
extern const app_desc_t app_sketch;
extern const app_desc_t app_agent;
extern const app_desc_t app_settings;
extern const app_desc_t app_onboarding;
extern void app_book_sync_runtime_settings(app_ctx_t *ctx, bool apply_px_to_open_book);

static const char *TAG = "ble_service";

#define BLE_STATUS_REFRESH_MS 1500LL
#define BLE_PMU_REFRESH_MS 15000LL
#define BLE_DISABLE_FLUSH_MS 120LL
#define BLE_LOCK_FLUSH_MS 100LL
#define BLE_PIN_LOCKOUT_MS 3000LL
#define BLE_CACHED_STATUS_MAX 512U

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static bool s_initialized;
static bool s_suspended;
static bool s_start_failed;
static bool s_disable_pending;
static int64_t s_disable_at_ms;
static bool s_lock_pending;
static int64_t s_lock_at_ms;
static const app_desc_t *s_deferred_app;
static int64_t s_deferred_app_at_ms;

static char s_device_id[13];
static char s_ble_pin[7];
static bool s_conn_authorized;
static unsigned s_pin_failures;
static int64_t s_pin_lock_until_ms;

static char *s_cmd_mailbox;
static size_t s_cmd_len;
static bool s_cmd_pending;
static bool s_cmd_busy_reply;
static bool s_conn_event_dirty;

static char s_cached_status[BLE_CACHED_STATUS_MAX];
static size_t s_cached_status_len;
static uint8_t s_cached_battery_soc;
static bool s_cached_battery_valid;
static uint8_t s_last_notified_battery = 0xFFU;

static int64_t s_last_status_refresh_ms;
static int64_t s_last_pmu_refresh_ms;

static bool s_last_tr_active;
static read_pico_transfer_mode_t s_last_tr_mode;
static read_pico_transfer_state_t s_last_tr_state;
static bool s_last_tr_net_ready;
static bool s_last_tr_pin_req;
static esp_err_t s_last_tr_err;
static char s_last_tr_url[64];
static char s_last_tr_ssid[33];

static void *ble_svc_alloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static void wipe_secret(void *ptr, size_t n) {
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (n--) *p++ = 0;
}

static const char *transfer_state_str(read_pico_transfer_state_t st) {
    switch (st) {
        case READ_PICO_TRANSFER_STARTING: return "starting";
        case READ_PICO_TRANSFER_READY: return "ready";
        case READ_PICO_TRANSFER_UPLOADING: return "uploading";
        case READ_PICO_TRANSFER_ERROR: return "error";
        case READ_PICO_TRANSFER_STOPPED:
        default: return "stopped";
    }
}

static void send_json_and_free(cJSON *obj) {
    if (!obj) return;
    char *text = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!text) return;
    size_t len = strlen(text);
    if (len > 0U && len <= READ_PICO_BLE_PAYLOAD_MAX) {
        esp_err_t err = read_pico_ble_send_response(text, len);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "BLE notify send failed: %s", esp_err_to_name(err));
        }
    } else if (len > READ_PICO_BLE_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "BLE response truncated/dropped: %u > %u",
                 (unsigned)len, (unsigned)READ_PICO_BLE_PAYLOAD_MAX);
    }
    cJSON_free(text);
}

static void read_device_settings_snapshot(read_pico_device_settings_t *out) {
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
    out->book_top_bar = app_settings_book_top_bar();
    out->book_bottom_bar = app_settings_book_bottom_bar();
    out->book_para_gap = app_settings_book_para_gap();
    out->book_indent = app_settings_book_indent();
    out->book_shelf_view = app_settings_book_shelf_view();
    out->book_cbz_dir = app_settings_book_cbz_dir();
    out->book_cbz_dither = app_settings_book_cbz_dither();
    out->saver_style = (uint8_t)app_settings_saver_style();
    out->dashboard_items = app_settings_dashboard_items();
    out->timezone_min = app_settings_timezone_min();
    out->charge_led = app_settings_charge_led();
    out->low_soc_notify_percent = app_settings_low_soc_notify_percent();
    out->muted = app_settings_muted();
    out->transfer_pin_required = app_settings_transfer_pin_required();
    out->ble_enabled = app_settings_ble_enabled();
    out->ble_pin_required = app_settings_ble_pin_required();
    const char *font = app_settings_font_path();
    snprintf(out->font_path, sizeof(out->font_path), "%s", font ? font : "");
}

static void apply_device_settings_snapshot(const read_pico_device_settings_t *next) {
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
    app_settings_set_book_top_bar(next->book_top_bar);
    app_settings_set_book_bottom_bar(next->book_bottom_bar);
    app_settings_set_book_para_gap(next->book_para_gap);
    app_settings_set_book_indent(next->book_indent);
    app_settings_set_book_shelf_view(next->book_shelf_view);
    app_settings_set_book_cbz_dir(next->book_cbz_dir);
    app_settings_set_book_cbz_dither(next->book_cbz_dither);
    app_settings_set_saver_style((app_saver_style_t)next->saver_style);
    app_settings_set_dashboard_items(next->dashboard_items);
    app_settings_set_timezone_min_deferred(next->timezone_min);
    app_settings_set_charge_led_deferred(next->charge_led);
    app_settings_set_low_soc_notify_percent_deferred(next->low_soc_notify_percent);
    app_settings_set_muted(next->muted);
    app_settings_set_transfer_pin_required(next->transfer_pin_required);
    app_settings_set_ble_enabled(next->ble_enabled);
    app_settings_set_ble_pin_required(next->ble_pin_required);
    app_settings_apply_runtime();
}

// Wi-Fi 页面持有射频期间 BLE 必须关闭（Wi-Fi/BLE 共存会耗尽内部内存）。
// BLE stays off while a Wi-Fi page owns the radio (coexistence exhausts internal RAM).
static bool s_wifi_hold;

static bool ota_is_busy(void) {
    ota_update_status_t st = {0};
    ota_update_get_status(&st);
    return st.state == OTA_UPDATE_CHECKING || st.state == OTA_UPDATE_DOWNLOADING;
}

static bool is_wifi_page(const app_desc_t *page) {
    return page == &app_transfer || page == &app_web || page == &app_feeds || page == &app_agent;
}

static bool wifi_is_busy(const app_desc_t *current) {
    if (is_wifi_page(current)) return true;
    if (ota_is_busy()) return true;
    read_pico_transfer_status_t tr = {0};
    read_pico_transfer_get_status(&tr);
    return tr.state != READ_PICO_TRANSFER_STOPPED;
}

static bool sd_is_mounted(void) {
    read_pico_sd_info_t sd = {0};
    return read_pico_sd_get_info(&sd) == ESP_OK && sd.mounted;
}

static void ensure_device_id(void) {
    if (s_device_id[0]) return;
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void ensure_ble_pin(void) {
    if (companion_pin_valid_format(s_ble_pin)) return;
    uint32_t value = esp_random() % 1000000U;
    snprintf(s_ble_pin, sizeof(s_ble_pin), "%06u", (unsigned)value);
}

static const char *active_page_key(const app_desc_t *current) {
    if (!current || current == &app_pico_home) return "home";
    if (current == &app_book) return "books";
    if (current == &app_transfer) return "transfer";
    if (current == &app_web) return "web";
    if (current == &app_feeds) return "feeds";
    if (current == &app_files) return "files";
    if (current == &app_sketch) return "sketch";
    if (current == &app_focus) return "focus";
    if (current == &app_agent) return "agent";
    if (current == &app_personal) return "personal";
    if (current == &app_settings) return "settings";
    if (current == &app_onboarding) return "onboarding";
    return "other";
}

static const app_desc_t *resolve_app_target(ble_app_target_t target) {
    switch (target) {
        case BLE_APP_TARGET_HOME: return &app_pico_home;
        case BLE_APP_TARGET_BOOKS: return &app_book;
        case BLE_APP_TARGET_TRANSFER: return &app_transfer;
        case BLE_APP_TARGET_WEB: return &app_web;
        case BLE_APP_TARGET_FEEDS: return &app_feeds;
        case BLE_APP_TARGET_FILES: return &app_files;
        case BLE_APP_TARGET_SKETCH: return &app_sketch;
        case BLE_APP_TARGET_FOCUS: return &app_focus;
        case BLE_APP_TARGET_AGENT: return &app_agent;
        case BLE_APP_TARGET_PERSONAL: return &app_personal;
        case BLE_APP_TARGET_SETTINGS: return &app_settings;
        case BLE_APP_TARGET_NONE:
        default: return NULL;
    }
}

static cJSON *build_status_object(const app_desc_t *current, bool include_storage,
                                  read_pico_ble_adv_state_t *adv_out) {
    ensure_device_id();
    read_pico_ble_status_t ble = {0};
    read_pico_ble_get_status(&ble);

    read_pico_transfer_status_t tr = {0};
    read_pico_transfer_get_status(&tr);
    bool transfer_active = (current == &app_transfer) || (tr.state != READ_PICO_TRANSFER_STOPPED);

    char saved_ssids[5][33];
    size_t saved_count = 0;
    (void)read_pico_transfer_get_saved_wifi_list(saved_ssids, &saved_count);
    bool wifi_configured = saved_count > 0U;

    const esp_app_desc_t *app = esp_app_get_description();
    const char *fw_ver = (app && app->version[0]) ? app->version : "unknown";

    const pmu_snapshot_t *pmu = read_pico_pmu_ready() ? read_pico_pmu_get() : NULL;
    bool bat_ok = pmu && pmu->status_ok;
    uint8_t soc = 0;
    bool charging = false;
    uint16_t mv = 0;
    if (bat_ok) {
        uint32_t s = ((uint32_t)pmu->soc_permille + 5U) / 10U;
        soc = (uint8_t)(s > 100U ? 100U : s);
        charging = pmu->charge_state == PMU_CHARGE_CHARGING;
        mv = pmu->battery_mv;
    }

    bool sd_mounted = sd_is_mounted();
    time_t utc = 0;
    bool time_valid = app_time_now_utc(&utc);
    bool pin_required = transfer_active ? tr.pin_required : app_settings_transfer_pin_required();
    bool ble_pin_required = app_settings_ble_pin_required();

    if (adv_out) {
        memset(adv_out, 0, sizeof(*adv_out));
        adv_out->battery_soc = bat_ok ? soc : 0xFFU;
        adv_out->battery_valid = bat_ok;
        adv_out->charging = charging;
        adv_out->transfer_active = transfer_active;
        adv_out->transfer_mode = transfer_active
            ? (tr.mode == READ_PICO_TRANSFER_MODE_STA ? 2U : 1U) : 0U;
        adv_out->wifi_configured = wifi_configured;
        adv_out->sd_mounted = sd_mounted;
        adv_out->pin_required = pin_required || ble_pin_required;
        ble_parse_semver(fw_ver, &adv_out->fw_major, &adv_out->fw_minor, &adv_out->fw_patch);
    }

    cJSON *json = cJSON_CreateObject();
    if (!json) return NULL;
    cJSON_AddNumberToObject(json, "ble_api", READ_PICO_BLE_API_VERSION);
    cJSON_AddNumberToObject(json, "api_version", READ_PICO_TRANSFER_API_VERSION);
    cJSON_AddStringToObject(json, "device_id", s_device_id);
    cJSON_AddStringToObject(json, "name", ble.device_name);
    cJSON_AddStringToObject(json, "firmware_version", fw_ver);
    cJSON_AddStringToObject(json, "active_page", active_page_key(current));
    cJSON_AddBoolToObject(json, "auth_required", ble_pin_required);
    cJSON_AddBoolToObject(json, "authorized", !ble_pin_required || s_conn_authorized);
    if (bat_ok) {
        cJSON_AddNumberToObject(json, "battery_soc", soc);
        cJSON_AddNumberToObject(json, "battery_mv", mv);
        cJSON_AddBoolToObject(json, "charging", charging);
    }
    cJSON_AddBoolToObject(json, "sd_mounted", sd_mounted);
    if (include_storage) {
        bool flash_ready = book_store_flash_ready();
        cJSON_AddBoolToObject(json, "flash_available", flash_ready);
        if (flash_ready) {
            book_store_root_t froot = { .path = "/flash/books", .is_flash = true };
            cJSON_AddNumberToObject(json, "flash_free_bytes", (double)book_store_free_bytes(&froot));
        }
        cJSON_AddBoolToObject(json, "sd_available", sd_mounted);
        if (sd_mounted) {
            book_store_root_t sroot = { .path = "/sdcard/books", .is_flash = false };
            cJSON_AddNumberToObject(json, "sd_free_bytes", (double)book_store_free_bytes(&sroot));
        }
    }
    cJSON_AddBoolToObject(json, "wifi_configured", wifi_configured);
    cJSON_AddNumberToObject(json, "wifi_count", (double)saved_count);
    cJSON_AddBoolToObject(json, "transfer_active", transfer_active);
    cJSON_AddStringToObject(json, "transfer_mode",
                            tr.mode == READ_PICO_TRANSFER_MODE_STA ? "sta" : "ap");
    cJSON_AddStringToObject(json, "transfer_state", transfer_state_str(tr.state));
    cJSON_AddBoolToObject(json, "network_ready", tr.network_ready);
    cJSON_AddStringToObject(json, "url", tr.network_ready ? tr.url : "");
    cJSON_AddStringToObject(json, "mdns_host", tr.mdns_host[0] ? tr.mdns_host : "readpico");
    if (transfer_active && tr.mode == READ_PICO_TRANSFER_MODE_AP && tr.ssid[0]) {
        cJSON_AddStringToObject(json, "ap_ssid", tr.ssid);
    } else if (transfer_active && tr.mode == READ_PICO_TRANSFER_MODE_STA && tr.wifi_ssid[0]) {
        cJSON_AddStringToObject(json, "wifi_ssid", tr.wifi_ssid);
    }
    cJSON_AddBoolToObject(json, "pin_required", pin_required);
    cJSON_AddBoolToObject(json, "time_valid", time_valid);
    cJSON_AddNumberToObject(json, "timezone_min", app_settings_timezone_min());
    return json;
}

static void refresh_cached_status(const app_desc_t *current, bool push_notify) {
    read_pico_ble_adv_state_t adv = {0};
    cJSON *obj = build_status_object(current, false, &adv);
    if (!obj) return;
    char *printed = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!printed) return;

    size_t len = strlen(printed);
    if (len >= BLE_CACHED_STATUS_MAX) len = BLE_CACHED_STATUS_MAX - 1U;

    bool status_changed = false;
    portENTER_CRITICAL(&s_lock);
    if (len != s_cached_status_len || memcmp(s_cached_status, printed, len) != 0) {
        memcpy(s_cached_status, printed, len);
        s_cached_status[len] = '\0';
        s_cached_status_len = len;
        status_changed = true;
    }
    s_cached_battery_valid = adv.battery_valid;
    if (adv.battery_valid) s_cached_battery_soc = adv.battery_soc;
    portEXIT_CRITICAL(&s_lock);

    read_pico_ble_update_adv_state(&adv);

    if (push_notify && status_changed) {
        (void)read_pico_ble_notify_status(printed, len);
    }
    if (adv.battery_valid && adv.battery_soc != s_last_notified_battery) {
        if (read_pico_ble_notify_battery(adv.battery_soc) == ESP_OK) {
            s_last_notified_battery = adv.battery_soc;
        }
    }
    cJSON_free(printed);
}

static void check_and_push_transfer_event(const app_desc_t *current) {
    read_pico_transfer_status_t tr = {0};
    read_pico_transfer_get_status(&tr);
    bool active = (current == &app_transfer) || (tr.state != READ_PICO_TRANSFER_STOPPED);
    const char *ssid = tr.mode == READ_PICO_TRANSFER_MODE_AP ? tr.ssid : tr.wifi_ssid;

    bool changed = active != s_last_tr_active ||
                   tr.mode != s_last_tr_mode ||
                   tr.state != s_last_tr_state ||
                   tr.network_ready != s_last_tr_net_ready ||
                   tr.pin_required != s_last_tr_pin_req ||
                   tr.last_error != s_last_tr_err ||
                   strcmp(tr.url, s_last_tr_url) != 0 ||
                   strcmp(ssid, s_last_tr_ssid) != 0;
    if (!changed) return;

    s_last_tr_active = active;
    s_last_tr_mode = tr.mode;
    s_last_tr_state = tr.state;
    s_last_tr_net_ready = tr.network_ready;
    s_last_tr_pin_req = tr.pin_required;
    s_last_tr_err = tr.last_error;
    snprintf(s_last_tr_url, sizeof(s_last_tr_url), "%.*s",
             (int)(sizeof(s_last_tr_url) - 1), tr.url);
    snprintf(s_last_tr_ssid, sizeof(s_last_tr_ssid), "%.*s",
             (int)(sizeof(s_last_tr_ssid) - 1), ssid);

    refresh_cached_status(current, true);

    read_pico_ble_status_t ble = {0};
    read_pico_ble_get_status(&ble);
    if (!ble.peer_connected || !ble.rsp_notify_enabled) return;

    cJSON *ev = cJSON_CreateObject();
    if (!ev) return;
    cJSON_AddStringToObject(ev, "event", "transfer_status");
    cJSON_AddBoolToObject(ev, "active", active);
    cJSON_AddStringToObject(ev, "mode", tr.mode == READ_PICO_TRANSFER_MODE_STA ? "sta" : "ap");
    cJSON_AddStringToObject(ev, "state", transfer_state_str(tr.state));
    cJSON_AddBoolToObject(ev, "network_ready", tr.network_ready);
    cJSON_AddStringToObject(ev, "url", tr.network_ready ? tr.url : "");
    cJSON_AddStringToObject(ev, "mdns_host", tr.mdns_host[0] ? tr.mdns_host : "readpico");
    cJSON_AddStringToObject(ev, "ssid", ssid);
    cJSON_AddBoolToObject(ev, "pin_required", tr.pin_required);
    cJSON_AddStringToObject(ev, "error",
                            tr.state == READ_PICO_TRANSFER_ERROR ? esp_err_to_name(tr.last_error) : "");
    send_json_and_free(ev);
}

/* ---- NimBLE 回调（仅缓存读取与投递，不阻塞不碰 I2C）/ NimBLE callbacks ---- */
static void ble_on_command(const char *json, size_t len, void *ctx) {
    (void)ctx;
    if (!json || !len || len > READ_PICO_BLE_PAYLOAD_MAX) return;
    portENTER_CRITICAL(&s_lock);
    if (!s_cmd_mailbox) {
        s_cmd_busy_reply = true;
    } else if (s_cmd_pending) {
        s_cmd_busy_reply = true;
    } else {
        memcpy(s_cmd_mailbox, json, len);
        s_cmd_mailbox[len] = '\0';
        s_cmd_len = len;
        s_cmd_pending = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

static size_t ble_on_status_read(char *buf, size_t cap, void *ctx) {
    (void)ctx;
    if (!buf || !cap) return 0;
    size_t len = 0;
    portENTER_CRITICAL(&s_lock);
    if (s_cached_status_len > 0U) {
        len = s_cached_status_len < cap - 1U ? s_cached_status_len : cap - 1U;
        memcpy(buf, s_cached_status, len);
        buf[len] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);
    if (len == 0U) {
        len = (size_t)snprintf(buf, cap, "{\"ble_api\":%u}", (unsigned)READ_PICO_BLE_API_VERSION);
    }
    return len;
}

static uint8_t ble_on_battery_read(void *ctx) {
    (void)ctx;
    uint8_t soc = 0;
    portENTER_CRITICAL(&s_lock);
    if (s_cached_battery_valid) soc = s_cached_battery_soc;
    portEXIT_CRITICAL(&s_lock);
    return soc;
}

static void ble_on_conn_change(bool connected, void *ctx) {
    (void)connected;
    (void)ctx;
    portENTER_CRITICAL(&s_lock);
    s_conn_event_dirty = true;
    portEXIT_CRITICAL(&s_lock);
}

/* ---- 文件写入工具 / File write helpers ---- */
static bool atomic_write_text_file(const char *path, const char *data, size_t len) {
    char part[320];
    char backup[320];
    if (snprintf(part, sizeof(part), "%s.part", path) >= (int)sizeof(part) ||
        snprintf(backup, sizeof(backup), "%s.rename-backup", path) >= (int)sizeof(backup)) {
        return false;
    }
    FILE *f = fopen(part, "wb");
    if (!f) return false;
    bool ok = (len == 0U || fwrite(data, 1, len, f) == len);
    if (fclose(f) != 0) ok = false;
    if (!ok) {
        (void)remove(part);
        return false;
    }
    if (rename(part, path) == 0) return true;
    // FatFs 不支持覆盖重命名：保留旧文件直至新文件就位。
    // FatFs cannot overwrite via rename: keep the existing file until commit succeeds.
    struct stat st;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        (void)remove(backup);
        if (rename(path, backup) == 0) {
            if (rename(part, path) == 0) {
                (void)remove(backup);
                return true;
            }
            (void)rename(backup, path);
        }
    }
    (void)remove(part);
    return false;
}

static bool pick_unique_clip_path(const char *root, const char *base_name, bool overwrite,
                                  char final_name[READ_PICO_BLE_CLIP_NAME_MAX],
                                  char full_path[288]) {
    snprintf(final_name, READ_PICO_BLE_CLIP_NAME_MAX, "%.*s",
             READ_PICO_BLE_CLIP_NAME_MAX - 1, base_name);
    if (snprintf(full_path, 288, "%s/%s", root, final_name) >= 288) return false;
    struct stat st;
    if (overwrite || stat(full_path, &st) != 0) return true;

    size_t n = strlen(base_name);
    if (n <= 4U) return false;
    char stem[READ_PICO_BLE_CLIP_TITLE_MAX + 1];
    size_t slen = n - 4U;
    if (slen > READ_PICO_BLE_CLIP_TITLE_MAX - 5U) slen = READ_PICO_BLE_CLIP_TITLE_MAX - 5U;
    memcpy(stem, base_name, slen);
    stem[slen] = '\0';

    for (unsigned idx = 2U; idx <= 99U; ++idx) {
        int written = snprintf(final_name, READ_PICO_BLE_CLIP_NAME_MAX, "%.60s_%u.txt", stem, idx);
        if (written <= 0 || (size_t)written >= READ_PICO_BLE_CLIP_NAME_MAX) continue;
        if (snprintf(full_path, 288, "%s/%s", root, final_name) >= 288) continue;
        if (stat(full_path, &st) != 0) return true;
    }
    return false;
}

/* ---- 指令分发（主循环执行）/ Command handlers (main loop) ---- */
static void add_wifi_list_fields(cJSON *res) {
    char ssids[5][33];
    size_t count = 0;
    (void)read_pico_transfer_get_saved_wifi_list(ssids, &count);
    cJSON_AddBoolToObject(res, "configured", count > 0U);
    cJSON_AddNumberToObject(res, "count", (double)count);
    cJSON *arr = cJSON_AddArrayToObject(res, "ssids");
    for (size_t i = 0; arr && i < count; ++i) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(ssids[i]));
    }
}

static void handle_command(app_ctx_t *ctx, const app_desc_t *current,
                           const char *raw, size_t len) {
    cJSON *root = NULL;
    bool has_id = false;
    int id = 0;
    ble_cmd_type_t cmd = BLE_CMD_UNKNOWN;
    char cmd_name[32] = {0};

    if (!ble_parse_command_envelope(raw, len, &root, &has_id, &id, &cmd, cmd_name)) {
        send_json_and_free(ble_make_error_response(false, 0, NULL, "bad_json", "无效的 JSON 命令"));
        return;
    }

    if (app_settings_ble_pin_required() && ble_cmd_requires_auth(cmd) && !s_conn_authorized) {
        send_json_and_free(ble_make_error_response(
            has_id, id, cmd_name, "auth_required", "请先通过 auth_verify 提交屏幕显示的 6 位蓝牙配对码"));
        cJSON_Delete(root);
        return;
    }

    const int64_t now_ms = ctx ? ctx->now_ms : (esp_timer_get_time() / 1000LL);

    switch (cmd) {
        case BLE_CMD_PING: {
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON_AddNumberToObject(res, "ble_api", READ_PICO_BLE_API_VERSION);
                cJSON_AddNumberToObject(res, "uptime_ms", (double)(esp_timer_get_time() / 1000LL));
                send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_STATUS: {
            if (read_pico_pmu_ready() && (!current || !current->holds_pmu)) {
                (void)read_pico_pmu_poll();
            }
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                read_pico_ble_adv_state_t adv = {0};
                cJSON *st = build_status_object(current, true, &adv);
                if (st) cJSON_AddItemToObject(res, "status", st);
                read_pico_ble_update_adv_state(&adv);
                send_json_and_free(res);
            }
            refresh_cached_status(current, false);
            break;
        }

        case BLE_CMD_AUTH_VERIFY: {
            ensure_ble_pin();
            const cJSON *pin_item = cJSON_GetObjectItemCaseSensitive(root, "pin");
            if (!cJSON_IsString(pin_item) || !companion_pin_valid_format(pin_item->valuestring)) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "请提供 6 位数字配对码"));
                break;
            }
            if (now_ms < s_pin_lock_until_ms) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "rate_limited", "配对码尝试过于频繁，请 3 秒后重试"));
                break;
            }
            if (!companion_pin_equals(s_ble_pin, pin_item->valuestring)) {
                if (++s_pin_failures >= 3U) {
                    s_pin_failures = 0U;
                    s_pin_lock_until_ms = now_ms + BLE_PIN_LOCKOUT_MS;
                }
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_pin", "蓝牙配对码错误"));
                break;
            }
            s_pin_failures = 0U;
            s_pin_lock_until_ms = 0;
            s_conn_authorized = true;
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON_AddBoolToObject(res, "authorized", true);
                cJSON_AddBoolToObject(res, "auth_required", app_settings_ble_pin_required());
                send_json_and_free(res);
            }
            refresh_cached_status(current, true);
            break;
        }

        case BLE_CMD_WIFI_LIST: {
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                add_wifi_list_fields(res);
                send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_WIFI_SCAN: {
            if (wifi_is_busy(current)) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "busy", "请先停止当前 Wi-Fi 任务后再扫描网络"));
                break;
            }
            read_pico_transfer_network_t *nets = ble_svc_alloc(
                sizeof(read_pico_transfer_network_t) * READ_PICO_TRANSFER_SCAN_MAX);
            if (!nets) {
                send_json_and_free(ble_make_error_response(has_id, id, cmd_name, "no_mem", "内存不足"));
                break;
            }
            size_t count = 0;
            esp_err_t err = read_pico_transfer_scan_wifi(nets, &count);
            if (err != ESP_OK) {
                free(nets);
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "scan_failed", esp_err_to_name(err)));
                break;
            }
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON *arr = cJSON_AddArrayToObject(res, "networks");
                for (size_t i = 0; arr && i < count; ++i) {
                    cJSON *item = cJSON_CreateObject();
                    if (!item) continue;
                    cJSON_AddStringToObject(item, "ssid", nets[i].ssid);
                    cJSON_AddNumberToObject(item, "rssi", nets[i].rssi);
                    cJSON_AddNumberToObject(item, "authmode", nets[i].authmode);
                    cJSON_AddBoolToObject(item, "requires_password", nets[i].requires_password);
                    cJSON_AddBoolToObject(item, "supported", nets[i].supported);
                    cJSON_AddItemToArray(arr, item);
                }
                send_json_and_free(res);
            }
            free(nets);
            break;
        }

        case BLE_CMD_WIFI_SAVE: {
            if (wifi_is_busy(current)) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "busy", "请先停止当前 Wi-Fi 传输再修改凭据"));
                break;
            }
            const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
            const cJSON *pwd_item = cJSON_GetObjectItemCaseSensitive(root, "password");
            const char *ssid = (cJSON_IsString(ssid_item) && ssid_item->valuestring) ? ssid_item->valuestring : NULL;
            const char *pwd = "";
            if (pwd_item) {
                if (!cJSON_IsString(pwd_item) || !pwd_item->valuestring) {
                    send_json_and_free(ble_make_error_response(
                        has_id, id, cmd_name, "invalid_arg", "password 须为字符串"));
                    break;
                }
                pwd = pwd_item->valuestring;
            }
            if (!ble_valid_wifi_credentials(ssid, pwd)) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "SSID 须 1..32 字节，密码为空或 8..63 位可见 ASCII"));
                break;
            }
            char ssid_buf[33] = {0};
            char pwd_buf[64] = {0};
            snprintf(ssid_buf, sizeof(ssid_buf), "%s", ssid);
            snprintf(pwd_buf, sizeof(pwd_buf), "%s", pwd);
            esp_err_t err = read_pico_transfer_save_wifi(ssid_buf, pwd_buf);
            wipe_secret(pwd_buf, sizeof(pwd_buf));
            if (err == ESP_ERR_NO_MEM) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "limit_reached", "最多保存 5 组 Wi-Fi，请先遗忘旧网络"));
            } else if (err != ESP_OK) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "save_failed", esp_err_to_name(err)));
            } else {
                cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
                if (res) {
                    add_wifi_list_fields(res);
                    send_json_and_free(res);
                }
                refresh_cached_status(current, true);
            }
            break;
        }

        case BLE_CMD_WIFI_FORGET: {
            const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
            const cJSON *all_item = cJSON_GetObjectItemCaseSensitive(root, "all");
            esp_err_t err;
            if ((all_item && cJSON_IsBool(all_item) && cJSON_IsTrue(all_item)) || !ssid_item) {
                err = read_pico_transfer_forget_wifi();
            } else if (cJSON_IsString(ssid_item) && ssid_item->valuestring && ssid_item->valuestring[0]) {
                err = read_pico_transfer_forget_wifi_ssid(ssid_item->valuestring);
            } else {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "请提供有效的 ssid 或设置 all=true"));
                break;
            }
            if (err == ESP_ERR_INVALID_STATE) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "busy", "当前网络运行中，请先停止传书再遗忘网络"));
            } else if (err == ESP_ERR_NOT_FOUND) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "not_found", "未找到该已保存 Wi-Fi"));
            } else if (err != ESP_OK) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "forget_failed", esp_err_to_name(err)));
            } else {
                cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
                if (res) {
                    add_wifi_list_fields(res);
                    send_json_and_free(res);
                }
                refresh_cached_status(current, true);
            }
            break;
        }

        case BLE_CMD_TRANSFER_START: {
            if ((current && current->holds_pmu) || ota_is_busy()) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "busy", "设备正在执行自检或系统更新，暂不能启动传书"));
                break;
            }
            char saved_ssid[33] = {0};
            bool configured = false;
            (void)read_pico_transfer_get_saved_wifi(saved_ssid, &configured);

            read_pico_transfer_mode_t mode = configured
                ? READ_PICO_TRANSFER_MODE_STA : READ_PICO_TRANSFER_MODE_AP;
            const cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "mode");
            if (mode_item) {
                if (!cJSON_IsString(mode_item) || !mode_item->valuestring) {
                    send_json_and_free(ble_make_error_response(
                        has_id, id, cmd_name, "invalid_arg", "mode 须为 ap、sta 或 auto"));
                    break;
                }
                if (!strcmp(mode_item->valuestring, "ap")) {
                    mode = READ_PICO_TRANSFER_MODE_AP;
                } else if (!strcmp(mode_item->valuestring, "sta")) {
                    if (!configured) {
                        send_json_and_free(ble_make_error_response(
                            has_id, id, cmd_name, "no_wifi", "尚未配置 Wi-Fi，请先通过蓝牙配网或改用 ap 模式"));
                        break;
                    }
                    mode = READ_PICO_TRANSFER_MODE_STA;
                } else if (strcmp(mode_item->valuestring, "auto") != 0) {
                    send_json_and_free(ble_make_error_response(
                        has_id, id, cmd_name, "invalid_arg", "mode 须为 ap、sta 或 auto"));
                    break;
                }
            }

            if (current == &app_transfer) {
                if (!app_transfer_remote_switch_mode(mode)) {
                    send_json_and_free(ble_make_error_response(
                        has_id, id, cmd_name, "busy", "上传正在进行中，无法切换模式"));
                    break;
                }
            } else {
                app_transfer_set_preferred_mode(mode);
                // 延迟 120 ms 再切入传书页，确保 BLE 响应包先发给客户端后再释放射频。
                // Defer entering Transfer by 120 ms so the BLE OK response flushes before BLE yields the radio.
                s_deferred_app = &app_transfer;
                s_deferred_app_at_ms = now_ms + 120;
            }

            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                read_pico_transfer_status_t tr = {0};
                read_pico_transfer_get_status(&tr);
                cJSON_AddStringToObject(res, "mode", mode == READ_PICO_TRANSFER_MODE_STA ? "sta" : "ap");
                cJSON_AddStringToObject(res, "state",
                                        current == &app_transfer ? transfer_state_str(tr.state) : "starting");
                cJSON_AddBoolToObject(res, "network_ready", current == &app_transfer && tr.network_ready);
                cJSON_AddStringToObject(res, "url", (current == &app_transfer && tr.network_ready) ? tr.url : "");
                cJSON_AddBoolToObject(res, "pin_required", app_settings_transfer_pin_required());
                send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_TRANSFER_STOP: {
            if (current == &app_transfer) {
                if (!app_transfer_remote_stop(ctx)) {
                    send_json_and_free(ble_make_error_response(
                        has_id, id, cmd_name, "busy", "文件传输正在进行中，暂不能停止"));
                    break;
                }
            }
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON_AddBoolToObject(res, "active", false);
                send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_TIME_SYNC: {
            const cJSON *unix_item = cJSON_GetObjectItemCaseSensitive(root, "unix_sec");
            const cJSON *tz_item = cJSON_GetObjectItemCaseSensitive(root, "timezone_min");
            uint32_t unix_sec = 0;
            if (!companion_json_u32_range(unix_item, APP_TIME_MIN_VALID, APP_TIME_MAX_VALID, &unix_sec)) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "unix_sec 超出有效时间范围"));
                break;
            }
            int tz_val = app_settings_timezone_min();
            bool has_tz = false;
            if (tz_item) {
                if (!companion_json_int_range(tz_item, -720, 840, &tz_val)) {
                    send_json_and_free(ble_make_error_response(
                        has_id, id, cmd_name, "invalid_arg", "timezone_min 须在 -720..840 分钟内"));
                    break;
                }
                has_tz = true;
            }
            if (has_tz) app_settings_set_timezone_min((int16_t)tz_val);
            bool pmu_synced = false;
            bool ok = app_time_set_utc(unix_sec, &pmu_synced);
            if (!ok) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "time_failed", "设置主机时间失败"));
                break;
            }
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON_AddNumberToObject(res, "unix_sec", (double)unix_sec);
                cJSON_AddNumberToObject(res, "timezone_min", app_settings_timezone_min());
                cJSON_AddBoolToObject(res, "pmu_synced", pmu_synced);
                send_json_and_free(res);
            }
            if (ctx && (current == &app_focus || current == &app_personal || current == &app_pico_home)) {
                ctx->request_redraw = true;
            }
            refresh_cached_status(current, true);
            break;
        }

        case BLE_CMD_SETTINGS_GET: {
            read_pico_device_settings_t ds;
            read_device_settings_snapshot(&ds);
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON *settings_json = companion_settings_to_json(&ds);
                if (settings_json) cJSON_AddItemToObject(res, "settings", settings_json);
                send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_SETTINGS_PATCH: {
            const cJSON *patch = cJSON_GetObjectItemCaseSensitive(root, "settings");
            read_pico_device_settings_t ds;
            read_device_settings_snapshot(&ds);
            bool was_ble_enabled = ds.ble_enabled;
            uint8_t prev_px = ds.book_px;
            const char *err_msg = NULL;
            if (!companion_settings_patch_object(&ds, patch, &err_msg)) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", err_msg));
                break;
            }
            apply_device_settings_snapshot(&ds);
            if (current == &app_book) {
                app_book_sync_runtime_settings(ctx, ds.book_px != prev_px);
            }
            if (ctx && (current == &app_book || current == &app_settings || current == &app_personal)) {
                ctx->request_redraw = true;
            }
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON *settings_json = companion_settings_to_json(&ds);
                if (settings_json) cJSON_AddItemToObject(res, "settings", settings_json);
                send_json_and_free(res);
            }
            if (was_ble_enabled && !ds.ble_enabled) {
                // 等待响应包发出后再停用 NimBLE，让 iOS 端收到确认。
                // Delay teardown briefly so the iOS client receives the confirmation first.
                s_disable_pending = true;
                s_disable_at_ms = esp_timer_get_time() / 1000LL + BLE_DISABLE_FLUSH_MS;
            } else {
                refresh_cached_status(current, true);
            }
            break;
        }

        case BLE_CMD_MEMO_SEND: {
            ble_memo_target_t target = BLE_MEMO_TARGET_CLIP;
            const cJSON *target_item = cJSON_GetObjectItemCaseSensitive(root, "target");
            if (target_item) {
                if (!cJSON_IsString(target_item) || !target_item->valuestring) {
                    send_json_and_free(ble_make_error_response(
                        has_id, id, cmd_name, "invalid_arg", "target 须为 clip 或 dashboard"));
                    break;
                }
                if (!strcmp(target_item->valuestring, "dashboard")) {
                    target = BLE_MEMO_TARGET_DASHBOARD;
                } else if (strcmp(target_item->valuestring, "clip") != 0) {
                    send_json_and_free(ble_make_error_response(
                        has_id, id, cmd_name, "invalid_arg", "target 须为 clip 或 dashboard"));
                    break;
                }
            }

            const cJSON *text_item = cJSON_GetObjectItemCaseSensitive(root, "text");
            if (!text_item) text_item = cJSON_GetObjectItemCaseSensitive(root, "note");
            if (!cJSON_IsString(text_item) || !text_item->valuestring) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "缺少文本内容字段 text"));
                break;
            }

            if (target == BLE_MEMO_TARGET_DASHBOARD) {
                if (!sd_is_mounted()) {
                    send_json_and_free(ble_make_error_response(
                        has_id, id, cmd_name, "no_sd", "看板文字需要已挂载的 TF 卡"));
                    break;
                }
                char *note = ble_svc_alloc(TRANSFER_DASHBOARD_NOTE_MAX + 1U);
                if (!note) {
                    send_json_and_free(ble_make_error_response(has_id, id, cmd_name, "no_mem", "内存不足"));
                    break;
                }
                if (!companion_normalize_dashboard_note_text(text_item->valuestring, note)) {
                    free(note);
                    send_json_and_free(ble_make_error_response(
                        has_id, id, cmd_name, "invalid_arg", "看板文字须为合法 UTF-8，最多 4 行且不超过 1024 字节"));
                    break;
                }
                size_t note_len = strlen(note);
                bool ok = note_len == 0U
                    ? (remove("/sdcard/dashboard.txt") == 0 || errno == ENOENT)
                    : atomic_write_text_file("/sdcard/dashboard.txt", note, note_len);
                free(note);
                if (!ok) {
                    send_json_and_free(ble_make_error_response(
                        has_id, id, cmd_name, "io_error", "写入看板文字失败"));
                    break;
                }
                cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
                if (res) {
                    cJSON_AddStringToObject(res, "target", "dashboard");
                    cJSON_AddNumberToObject(res, "bytes", (double)note_len);
                    send_json_and_free(res);
                }
                if (current == &app_personal) {
                    app_personal_reload_note();
                    if (ctx) ctx->request_redraw = true;
                }
                break;
            }

            // 短文本剪藏直传：直接保存为书架 .txt，无需开启 Wi-Fi。
            // Short-text clip over BLE: save directly as a bookshelf .txt without Wi-Fi.
            read_pico_transfer_status_t tr = {0};
            read_pico_transfer_get_status(&tr);
            if (tr.state == READ_PICO_TRANSFER_UPLOADING) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "busy", "局域网上传进行中，请稍后重试"));
                break;
            }

            char *clip = ble_svc_alloc(READ_PICO_BLE_CLIP_TEXT_MAX + 1U);
            if (!clip) {
                send_json_and_free(ble_make_error_response(has_id, id, cmd_name, "no_mem", "内存不足"));
                break;
            }
            size_t clip_len = 0;
            if (!ble_normalize_clip_text(text_item->valuestring, clip,
                                         READ_PICO_BLE_CLIP_TEXT_MAX + 1U, &clip_len)) {
                free(clip);
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "正文须为非空合法 UTF-8 且不超过 3584 字节"));
                break;
            }

            book_store_root_t store = {0};
            if (book_store_upload_root(&store) != ESP_OK) {
                free(clip);
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "no_storage", "无可用图书存储目录"));
                break;
            }
            if (book_store_free_bytes(&store) < (uint64_t)clip_len + 512U) {
                free(clip);
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "no_space", "存储空间不足"));
                break;
            }

            char fallback_stem[32];
            struct tm local_tm;
            if (app_time_now_local(&local_tm, NULL)) {
                strftime(fallback_stem, sizeof(fallback_stem), "ble_%Y%m%d_%H%M%S", &local_tm);
            } else {
                snprintf(fallback_stem, sizeof(fallback_stem), "ble_%llu",
                         (unsigned long long)(esp_timer_get_time() / 1000000ULL));
            }
            const cJSON *title_item = cJSON_GetObjectItemCaseSensitive(root, "title");
            const char *title = (cJSON_IsString(title_item) && title_item->valuestring)
                ? title_item->valuestring : "";
            const cJSON *ow_item = cJSON_GetObjectItemCaseSensitive(root, "overwrite");
            bool overwrite = cJSON_IsBool(ow_item) && cJSON_IsTrue(ow_item);

            char base_name[READ_PICO_BLE_CLIP_NAME_MAX];
            char final_name[READ_PICO_BLE_CLIP_NAME_MAX];
            char full_path[288];
            if (!ble_sanitize_clip_filename(title, fallback_stem, base_name) ||
                !pick_unique_clip_path(store.path, base_name, overwrite, final_name, full_path)) {
                free(clip);
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "无法生成有效文件名"));
                break;
            }

            bool ok = atomic_write_text_file(full_path, clip, clip_len);
            free(clip);
            if (!ok) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "io_error", "保存短文本文件失败"));
                break;
            }
            if (overwrite) (void)book_store_forget_changed_path(full_path);
            book_store_notify_changed();

            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON_AddStringToObject(res, "target", "clip");
                cJSON_AddStringToObject(res, "name", final_name);
                cJSON_AddNumberToObject(res, "bytes", (double)clip_len);
                cJSON_AddStringToObject(res, "storage", store.is_flash ? "flash" : "sd");
                send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_DASHBOARD_GET: {
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                bool note_enabled = sd_is_mounted();
                cJSON_AddBoolToObject(res, "note_enabled", note_enabled);
                cJSON_AddNumberToObject(res, "note_max_lines", TRANSFER_DASHBOARD_NOTE_LINES);
                cJSON_AddNumberToObject(res, "note_max_bytes", TRANSFER_DASHBOARD_NOTE_MAX);
                if (note_enabled) {
                    char *note = ble_svc_alloc(TRANSFER_DASHBOARD_NOTE_MAX + 1U);
                    if (note) {
                        note[0] = '\0';
                        FILE *f = fopen("/sdcard/dashboard.txt", "rb");
                        if (f) {
                            size_t n = fread(note, 1, TRANSFER_DASHBOARD_NOTE_MAX, f);
                            note[n] = '\0';
                            fclose(f);
                        }
                        cJSON_AddStringToObject(res, "note", note);
                        free(note);
                    } else {
                        cJSON_AddStringToObject(res, "note", "");
                    }
                } else {
                    cJSON_AddStringToObject(res, "note", "");
                }

                cJSON_AddNumberToObject(res, "total_reading_sec", (double)reading_stats_total_seconds());
                char title[128] = {0};
                uint8_t pct = 0;
                if (reading_stats_get_last_book(title, sizeof(title), &pct)) {
                    cJSON *lb = cJSON_AddObjectToObject(res, "last_book");
                    if (lb) {
                        cJSON_AddStringToObject(lb, "title", title);
                        cJSON_AddNumberToObject(lb, "percent", pct);
                    }
                }
                char ex_title[128] = {0};
                char ex_text[256] = {0};
                if (reading_stats_get_excerpt(ex_title, sizeof(ex_title), ex_text, sizeof(ex_text))) {
                    cJSON *ex = cJSON_AddObjectToObject(res, "excerpt");
                    if (ex) {
                        cJSON_AddStringToObject(ex, "title", ex_title);
                        cJSON_AddStringToObject(ex, "text", ex_text);
                    }
                }
                send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_WEBOPEN: {
            if ((current && current->holds_pmu) || ota_is_busy()) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "busy", "设备正在执行自检或系统更新，暂不能打开网页"));
                break;
            }
            read_pico_transfer_status_t tr = {0};
            read_pico_transfer_get_status(&tr);
            if (tr.state == READ_PICO_TRANSFER_UPLOADING) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "busy", "文件传输正在进行中，暂不能切换至网页阅读"));
                break;
            }
            const cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
            if (!cJSON_IsString(url_item) ||
                !ble_valid_http_url(url_item->valuestring, READ_PICO_WEBOPEN_URL_MAX)) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "url 须为合法的 http(s):// 地址"));
                break;
            }
            app_web_queue_url(url_item->valuestring);
            s_deferred_app = &app_web;
            s_deferred_app_at_ms = now_ms + 120;
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON_AddStringToObject(res, "url", url_item->valuestring);
                send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_OPEN_APP: {
            if ((current && current->holds_pmu) || ota_is_busy()) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "busy", "设备正在执行自检或系统更新，暂不能切换页面"));
                break;
            }
            read_pico_transfer_status_t tr = {0};
            read_pico_transfer_get_status(&tr);
            if (tr.state == READ_PICO_TRANSFER_UPLOADING) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "busy", "文件传输正在进行中，暂不能切换页面"));
                break;
            }
            const cJSON *app_item = cJSON_GetObjectItemCaseSensitive(root, "app");
            ble_app_target_t target = (cJSON_IsString(app_item) && app_item->valuestring)
                ? ble_parse_app_target(app_item->valuestring) : BLE_APP_TARGET_NONE;
            const app_desc_t *page = resolve_app_target(target);
            if (!page) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg",
                    "app 须为 home/books/transfer/web/feeds/files/sketch/focus/agent/personal/settings"));
                break;
            }
            if (ctx && page != current) {
                if (is_wifi_page(page)) {
                    s_deferred_app = page;
                    s_deferred_app_at_ms = now_ms + 120;
                } else {
                    ctx->request_app = page;
                }
            }
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON_AddStringToObject(res, "app", app_item->valuestring);
                send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_AGENT_GET: {
            read_pico_agent_config_t cfg = {0};
            esp_err_t err = read_pico_transfer_get_agent_config(&cfg);
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON_AddBoolToObject(res, "configured", err == ESP_OK);
                if (err == ESP_OK) {
                    cJSON_AddStringToObject(res, "uri", cfg.broker_uri);
                    cJSON_AddStringToObject(res, "username", cfg.username);
                    cJSON_AddStringToObject(res, "topic", cfg.topic);
                    cJSON_AddBoolToObject(res, "has_password", cfg.password[0] != '\0');
                }
                send_json_and_free(res);
            }
            wipe_secret(&cfg, sizeof(cfg));
            break;
        }

        case BLE_CMD_AGENT_SET: {
            const cJSON *clear_item = cJSON_GetObjectItemCaseSensitive(root, "clear");
            if (cJSON_IsBool(clear_item) && cJSON_IsTrue(clear_item)) {
                esp_err_t err = read_pico_transfer_set_agent_config(NULL);
                if (err != ESP_OK) {
                    send_json_and_free(ble_make_error_response(
                        has_id, id, cmd_name, "save_failed", esp_err_to_name(err)));
                } else {
                    cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
                    if (res) {
                        cJSON_AddBoolToObject(res, "configured", false);
                        send_json_and_free(res);
                    }
                }
                break;
            }
            read_pico_agent_config_t next = {0}, old = {0};
            const cJSON *uri_i = cJSON_GetObjectItemCaseSensitive(root, "uri");
            const cJSON *user_i = cJSON_GetObjectItemCaseSensitive(root, "username");
            const cJSON *pwd_i = cJSON_GetObjectItemCaseSensitive(root, "password");
            const cJSON *topic_i = cJSON_GetObjectItemCaseSensitive(root, "topic");
            const cJSON *clrpwd_i = cJSON_GetObjectItemCaseSensitive(root, "clear_password");
            bool clear_pwd = cJSON_IsBool(clrpwd_i) && cJSON_IsTrue(clrpwd_i);
            bool valid = cJSON_IsString(uri_i) && uri_i->valuestring &&
                         strlen(uri_i->valuestring) < sizeof(next.broker_uri) &&
                         cJSON_IsString(topic_i) && topic_i->valuestring &&
                         strlen(topic_i->valuestring) < sizeof(next.topic) &&
                         (!user_i || (cJSON_IsString(user_i) && user_i->valuestring &&
                                      strlen(user_i->valuestring) < sizeof(next.username))) &&
                         (!pwd_i || (cJSON_IsString(pwd_i) && pwd_i->valuestring &&
                                     strlen(pwd_i->valuestring) < sizeof(next.password)));
            if (!valid) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "请提供有效的 mqtts:// uri 与精确 topic"));
                break;
            }
            snprintf(next.broker_uri, sizeof(next.broker_uri), "%s", uri_i->valuestring);
            snprintf(next.topic, sizeof(next.topic), "%s", topic_i->valuestring);
            if (user_i && user_i->valuestring) {
                snprintf(next.username, sizeof(next.username), "%s", user_i->valuestring);
            }
            if (pwd_i && pwd_i->valuestring && pwd_i->valuestring[0]) {
                snprintf(next.password, sizeof(next.password), "%s", pwd_i->valuestring);
            } else if (!clear_pwd && read_pico_transfer_get_agent_config(&old) == ESP_OK) {
                memcpy(next.password, old.password, sizeof(next.password));
            }
            wipe_secret(&old, sizeof(old));
            esp_err_t err = read_pico_transfer_set_agent_config(&next);
            wipe_secret(&next, sizeof(next));
            if (err == ESP_ERR_INVALID_ARG) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "仅支持有效的 mqtts:// 地址与无通配符主题"));
            } else if (err != ESP_OK) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "save_failed", esp_err_to_name(err)));
            } else {
                cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
                if (res) {
                    cJSON_AddBoolToObject(res, "configured", true);
                    send_json_and_free(res);
                }
            }
            break;
        }

        case BLE_CMD_AMBIENT_GET: {
            app_ambient_settings_t amb = {0};
            app_settings_ambient_get(&amb);
            bool has_res = false, last_ok = false;
            uint32_t last_ts = 0;
            app_settings_ambient_result_get(&has_res, &last_ok, &last_ts);
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON_AddBoolToObject(res, "enabled", amb.enabled);
                cJSON_AddNumberToObject(res, "interval_min", amb.interval_min);
                cJSON_AddNumberToObject(res, "quiet_start_h", amb.quiet_start_h);
                cJSON_AddNumberToObject(res, "quiet_end_h", amb.quiet_end_h);
                cJSON_AddNumberToObject(res, "source_type", amb.source_type);
                cJSON_AddStringToObject(res, "url", amb.url);
                cJSON_AddBoolToObject(res, "has_auth", amb.auth_header[0] != '\0');
                cJSON_AddBoolToObject(res, "has_result", has_res);
                if (has_res) {
                    cJSON_AddBoolToObject(res, "last_success", last_ok);
                    cJSON_AddNumberToObject(res, "last_ts", (double)last_ts);
                }
                send_json_and_free(res);
            }
            wipe_secret(&amb, sizeof(amb));
            break;
        }

        case BLE_CMD_AMBIENT_SET: {
            app_ambient_settings_t next = {0};
            app_settings_ambient_get(&next);
            const cJSON *en_i = cJSON_GetObjectItemCaseSensitive(root, "enabled");
            const cJSON *iv_i = cJSON_GetObjectItemCaseSensitive(root, "interval_min");
            const cJSON *qs_i = cJSON_GetObjectItemCaseSensitive(root, "quiet_start_h");
            const cJSON *qe_i = cJSON_GetObjectItemCaseSensitive(root, "quiet_end_h");
            const cJSON *st_i = cJSON_GetObjectItemCaseSensitive(root, "source_type");
            const cJSON *url_i = cJSON_GetObjectItemCaseSensitive(root, "url");
            const cJSON *auth_i = cJSON_GetObjectItemCaseSensitive(root, "auth_header");
            const cJSON *clra_i = cJSON_GetObjectItemCaseSensitive(root, "clear_auth");
            bool ok = true;
            int iv = 0;
            if (en_i) {
                ok = cJSON_IsBool(en_i);
                if (ok) next.enabled = cJSON_IsTrue(en_i);
            }
            if (ok && iv_i) {
                ok = companion_json_int_range(iv_i, 15, 1440, &iv);
                if (ok) next.interval_min = (uint16_t)iv;
            }
            if (ok && qs_i) {
                ok = companion_json_int_range(qs_i, 0, 23, &iv);
                if (ok) next.quiet_start_h = (uint8_t)iv;
            }
            if (ok && qe_i) {
                ok = companion_json_int_range(qe_i, 0, 23, &iv);
                if (ok) next.quiet_end_h = (uint8_t)iv;
            }
            if (ok && st_i) {
                ok = companion_json_int_range(st_i, 0, 2, &iv);
                if (ok) next.source_type = (uint8_t)iv;
            }
            if (ok && url_i) {
                ok = cJSON_IsString(url_i) && url_i->valuestring &&
                     strlen(url_i->valuestring) < sizeof(next.url) &&
                     (!url_i->valuestring[0] || ble_valid_http_url(url_i->valuestring, sizeof(next.url)));
                if (ok) snprintf(next.url, sizeof(next.url), "%s", url_i->valuestring);
            }
            if (ok && clra_i && cJSON_IsBool(clra_i) && cJSON_IsTrue(clra_i)) {
                next.auth_header[0] = '\0';
            } else if (ok && auth_i) {
                ok = cJSON_IsString(auth_i) && auth_i->valuestring &&
                     strlen(auth_i->valuestring) < sizeof(next.auth_header);
                if (ok && auth_i->valuestring[0]) {
                    snprintf(next.auth_header, sizeof(next.auth_header), "%s", auth_i->valuestring);
                }
            }
            bool saved = ok && app_settings_ambient_set(&next);
            wipe_secret(&next, sizeof(next));
            if (!saved) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "定时看板参数无效（周期须为 15/30/60/120/360/720/1440）"));
            } else {
                cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
                if (res) send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_REFRESH_SCREEN: {
            if (ctx) ctx->request_redraw = true;
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) send_json_and_free(res);
            break;
        }

        case BLE_CMD_LOCK: {
            if ((current && current->holds_pmu) || ota_is_busy()) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "busy", "当前页面独占电源管理或正在更新，无法锁屏"));
                break;
            }
            read_pico_transfer_status_t tr = {0};
            read_pico_transfer_get_status(&tr);
            if (tr.state == READ_PICO_TRANSFER_UPLOADING) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "busy", "文件传输正在进行中，无法锁屏"));
                break;
            }
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) send_json_and_free(res);
            s_lock_pending = true;
            s_lock_at_ms = (ctx ? ctx->now_ms : (esp_timer_get_time() / 1000LL)) + BLE_LOCK_FLUSH_MS;
            break;
        }

        case BLE_CMD_WEBMARKS_GET: {
            web_bookmark_t *items = ble_svc_alloc(sizeof(web_bookmark_t) * WEB_BOOKMARK_MAX);
            if (!items) {
                send_json_and_free(ble_make_error_response(has_id, id, cmd_name, "no_mem", "内存不足"));
                break;
            }
            size_t count = 0;
            web_bookmarks_load(items, &count);
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON_AddNumberToObject(res, "count", (double)count);
                cJSON_AddNumberToObject(res, "max", (double)WEB_BOOKMARK_MAX);
                cJSON *arr = cJSON_AddArrayToObject(res, "items");
                for (size_t i = 0; arr && i < count && i < WEB_BOOKMARK_MAX; ++i) {
                    cJSON *obj = cJSON_CreateObject();
                    if (!obj) continue;
                    cJSON_AddStringToObject(obj, "title", items[i].title);
                    cJSON_AddStringToObject(obj, "url", items[i].url);
                    cJSON_AddItemToArray(arr, obj);
                }
                send_json_and_free(res);
            }
            free(items);
            break;
        }

        case BLE_CMD_WEBMARKS_ADD: {
            const cJSON *url_i = cJSON_GetObjectItemCaseSensitive(root, "url");
            const cJSON *title_i = cJSON_GetObjectItemCaseSensitive(root, "title");
            if (!cJSON_IsString(url_i) || !url_i->valuestring ||
                (title_i && (!cJSON_IsString(title_i) || !title_i->valuestring))) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "请提供合法的 http(s):// url 与可选 title"));
                break;
            }
            esp_err_t err = web_bookmarks_upsert(title_i ? title_i->valuestring : "", url_i->valuestring);
            if (err == ESP_ERR_NO_MEM) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "limit_reached", "网页收藏夹已满（最多 20 条）"));
            } else if (err != ESP_OK) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "网址无效或保存失败"));
            } else {
                cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
                if (res) send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_WEBMARKS_DEL: {
            const cJSON *url_i = cJSON_GetObjectItemCaseSensitive(root, "url");
            if (!cJSON_IsString(url_i) || !url_i->valuestring || !url_i->valuestring[0]) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "请提供要删除的 url"));
                break;
            }
            esp_err_t err = web_bookmarks_delete(url_i->valuestring);
            if (err == ESP_ERR_NOT_FOUND) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "not_found", "未找到该网页收藏"));
            } else if (err != ESP_OK) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "delete_failed", esp_err_to_name(err)));
            } else {
                cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
                if (res) send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_FOCUS_GET: {
            app_focus_status_t st = {0};
            app_focus_get_status(&st);
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON_AddBoolToObject(res, "running", st.running);
                cJSON_AddStringToObject(res, "phase", st.running ? (st.is_break ? "break" : "focus") : "idle");
                cJSON_AddNumberToObject(res, "duration_min", st.duration_min);
                cJSON_AddNumberToObject(res, "remaining_sec", (double)st.remaining_sec);
                cJSON_AddNumberToObject(res, "completed_count", (double)st.completed_count);
                send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_FOCUS_START: {
            if ((current && current->holds_pmu) || ota_is_busy()) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "busy", "当前正在自检或更新，暂不能启动专注时钟"));
                break;
            }
            read_pico_transfer_status_t tr = {0};
            read_pico_transfer_get_status(&tr);
            if (tr.state == READ_PICO_TRANSFER_UPLOADING) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "busy", "文件传输正在进行中，暂不能启动专注时钟"));
                break;
            }
            int minutes = 25;
            const cJSON *mins_i = cJSON_GetObjectItemCaseSensitive(root, "minutes");
            if (mins_i) {
                if (!companion_json_int_range(mins_i, 15, 45, &minutes) ||
                    (minutes != 15 && minutes != 25 && minutes != 45)) {
                    send_json_and_free(ble_make_error_response(
                        has_id, id, cmd_name, "invalid_arg", "minutes 须为 15、25 或 45"));
                    break;
                }
            }
            if (!app_focus_remote_start(minutes)) {
                send_json_and_free(ble_make_error_response(
                    has_id, id, cmd_name, "invalid_arg", "启动专注时钟失败"));
                break;
            }
            if (ctx) {
                if (current != &app_focus) ctx->request_app = &app_focus;
                else ctx->request_redraw = true;
            }
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON_AddNumberToObject(res, "minutes", minutes);
                cJSON_AddBoolToObject(res, "running", true);
                send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_FOCUS_STOP: {
            app_focus_remote_stop();
            if (ctx && current == &app_focus) ctx->request_redraw = true;
            cJSON *res = ble_make_response_base(has_id, id, cmd_name, true);
            if (res) {
                cJSON_AddBoolToObject(res, "running", false);
                send_json_and_free(res);
            }
            break;
        }

        case BLE_CMD_UNKNOWN:
        default:
            send_json_and_free(ble_make_error_response(
                has_id, id, cmd_name, "unknown_cmd", "不支持的蓝牙指令"));
            break;
    }

    cJSON_Delete(root);
}

/* ---- 生命周期管理 / Lifecycle ---- */
static void start_ble_internal(void) {
    if (read_pico_ble_is_running()) return;
    ensure_device_id();
    ensure_ble_pin();
    if (!s_cmd_mailbox) {
        s_cmd_mailbox = ble_svc_alloc(READ_PICO_BLE_PAYLOAD_MAX + 1U);
        if (!s_cmd_mailbox) {
            ESP_LOGE(TAG, "failed to allocate BLE command mailbox");
            return;
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_cmd_pending = false;
    s_cmd_len = 0;
    s_cmd_busy_reply = false;
    s_conn_event_dirty = false;
    s_last_notified_battery = 0xFFU;
    portEXIT_CRITICAL(&s_lock);

    s_conn_authorized = false;
    s_pin_failures = 0U;
    s_pin_lock_until_ms = 0;
    s_disable_pending = false;
    s_lock_pending = false;
    refresh_cached_status(NULL, false);

    read_pico_ble_callbacks_t cbs = {
        .on_command = ble_on_command,
        .on_status_read = ble_on_status_read,
        .on_battery_read = ble_on_battery_read,
        .on_conn_change = ble_on_conn_change,
        .ctx = NULL,
    };
    esp_err_t err = read_pico_ble_start(&cbs);
    if (err != ESP_OK) {
        s_start_failed = true;
        ESP_LOGW(TAG, "BLE start failed: %s", esp_err_to_name(err));
        return;
    }
    s_start_failed = false;
    refresh_cached_status(NULL, false);
}

static void stop_ble_internal(void) {
    s_disable_pending = false;
    s_lock_pending = false;
    s_start_failed = false;
    s_conn_authorized = false;
    if (read_pico_ble_is_running()) read_pico_ble_stop();
    portENTER_CRITICAL(&s_lock);
    s_cmd_pending = false;
    s_cmd_len = 0;
    s_cmd_busy_reply = false;
    s_conn_event_dirty = false;
    s_last_notified_battery = 0xFFU;
    portEXIT_CRITICAL(&s_lock);
    if (s_cmd_mailbox) {
        free(s_cmd_mailbox);
        s_cmd_mailbox = NULL;
    }
}

void app_ble_init(void) {
    s_initialized = true;
    s_suspended = false;
    s_last_status_refresh_ms = 0;
    s_last_pmu_refresh_ms = 0;
    if (app_settings_ble_enabled()) start_ble_internal();
}

void app_ble_set_enabled(bool enabled) {
    app_settings_set_ble_enabled(enabled);
    s_start_failed = false;
    if (!s_initialized || s_suspended || s_wifi_hold) return;
    if (enabled) start_ble_internal();
    else stop_ble_internal();
}

void app_ble_set_pin_required(bool required) {
    app_settings_set_ble_pin_required(required);
    if (required) {
        s_ble_pin[0] = '\0';
        ensure_ble_pin();
        s_conn_authorized = false;
        s_pin_failures = 0U;
        s_pin_lock_until_ms = 0;
    }
    if (s_initialized && !s_suspended && read_pico_ble_is_running()) {
        refresh_cached_status(NULL, true);
    }
}

void app_ble_get_pin(char out[7]) {
    if (!out) return;
    ensure_ble_pin();
    memcpy(out, s_ble_pin, 7);
}

void app_ble_suspend(void) {
    s_suspended = true;
    s_deferred_app = NULL;
    stop_ble_internal();
}

void app_ble_resume(void) {
    s_suspended = false;
    s_start_failed = false;
    if (s_initialized && !s_wifi_hold && app_settings_ble_enabled()) start_ble_internal();
}

void app_ble_wifi_hold(bool hold) {
    s_wifi_hold = hold;
    s_start_failed = false;
    if (hold) s_deferred_app = NULL;
    if (!s_initialized) return;
    if (hold) {
        stop_ble_internal();
    }
    // 释放射频时不在 on_exit 里同步拉起 NimBLE，交由下一拍 app_ble_poll 恢复，
    // 避免离页卡顿及在两个 Wi-Fi 页间直跳时反复启停 BLE。
    // Do not restart NimBLE synchronously inside on_exit; let the next app_ble_poll tick resume BLE,
    // avoiding exit stalls and BLE start/stop thrash when switching between Wi-Fi pages.
}

void app_ble_yield_to_ota(void) {
    s_deferred_app = NULL;
    if (!s_initialized) return;
    stop_ble_internal();
}

bool app_ble_poll(app_ctx_t *ctx, const app_desc_t *current, bool menu_open) {
    (void)menu_open;
    if (!s_initialized || s_suspended) return false;

    int64_t now_ms = ctx ? ctx->now_ms : (esp_timer_get_time() / 1000LL);
    bool want_enabled = app_settings_ble_enabled();
    bool running = read_pico_ble_is_running();

    if (s_disable_pending) {
        if (now_ms >= s_disable_at_ms) {
            stop_ble_internal();
        }
        return false;
    }
    if (s_lock_pending) {
        if (now_ms >= s_lock_at_ms) {
            s_lock_pending = false;
            if (ctx) ctx->request_lock = true;
        }
        return false;
    }
    if (s_deferred_app) {
        if (now_ms >= s_deferred_app_at_ms) {
            const app_desc_t *target = s_deferred_app;
            s_deferred_app = NULL;
            if (ctx) ctx->request_app = target;
        }
        return false;
    }

    if (s_wifi_hold || ota_is_busy()) {
        if (running) stop_ble_internal();
        return false;
    }

    if (want_enabled && !running && !s_start_failed) {
        start_ble_internal();
        return false;
    }
    if (!want_enabled && running) {
        stop_ble_internal();
        return false;
    }
    if (!running) return false;

    bool activity = false;

    bool busy_reply = false;
    bool has_cmd = false;
    size_t cmd_len = 0;
    char *cmd_copy = NULL;
    bool conn_dirty = false;

    portENTER_CRITICAL(&s_lock);
    if (s_cmd_busy_reply) {
        busy_reply = true;
        s_cmd_busy_reply = false;
    }
    if (s_cmd_pending && s_cmd_mailbox && s_cmd_len > 0U) {
        cmd_len = s_cmd_len;
        has_cmd = true;
    }
    if (s_conn_event_dirty) {
        conn_dirty = true;
        s_conn_event_dirty = false;
    }
    portEXIT_CRITICAL(&s_lock);

    if (has_cmd) {
        cmd_copy = ble_svc_alloc(cmd_len + 1U);
        portENTER_CRITICAL(&s_lock);
        if (cmd_copy && s_cmd_mailbox && s_cmd_pending && s_cmd_len == cmd_len) {
            memcpy(cmd_copy, s_cmd_mailbox, cmd_len);
            cmd_copy[cmd_len] = '\0';
        } else {
            has_cmd = false;
        }
        s_cmd_pending = false;
        s_cmd_len = 0;
        portEXIT_CRITICAL(&s_lock);
    }

    if (busy_reply) {
        send_json_and_free(ble_make_error_response(
            false, 0, NULL, "busy", "上一条蓝牙指令正在处理中，请稍候重试"));
    }

    if (has_cmd && cmd_copy) {
        handle_command(ctx, current, cmd_copy, cmd_len);
        activity = true;
    }
    free(cmd_copy);

    if (conn_dirty) {
        read_pico_ble_status_t cur_ble = {0};
        read_pico_ble_get_status(&cur_ble);
        if (!cur_ble.peer_connected) s_conn_authorized = false;
        s_last_notified_battery = 0xFFU;
        refresh_cached_status(current, true);
        activity = true;
    }

    // 监控传输页连接与 IP 变化，第一时间向 iOS 推送异步事件。
    // Watch Transfer state and IP changes so iOS receives immediate handoff events.
    check_and_push_transfer_event(current);

    if (now_ms - s_last_status_refresh_ms >= BLE_STATUS_REFRESH_MS) {
        s_last_status_refresh_ms = now_ms;
        if (read_pico_pmu_ready() && (!current || !current->holds_pmu) &&
            now_ms - s_last_pmu_refresh_ms >= BLE_PMU_REFRESH_MS) {
            s_last_pmu_refresh_ms = now_ms;
            (void)read_pico_pmu_poll();
        }
        refresh_cached_status(current, true);
    }

    return activity;
}

void app_ble_format_status(char *buf, size_t cap) {
    if (!buf || !cap) return;
    if (!app_settings_ble_enabled()) {
        snprintf(buf, cap, "已关闭");
        return;
    }
    read_pico_ble_status_t st = {0};
    read_pico_ble_get_status(&st);
    const char *name = st.device_name[0] ? st.device_name : "ReadPico";
    if (app_settings_ble_pin_required()) {
        char pin[7];
        app_ble_get_pin(pin);
        snprintf(buf, cap, "%.14s · 码 %s", name, pin);
        return;
    }
    switch (st.state) {
        case READ_PICO_BLE_CONNECTED:
            snprintf(buf, cap, "%.18s · 已连接", name);
            break;
        case READ_PICO_BLE_ADVERTISING:
            snprintf(buf, cap, "%.18s · 广播中", name);
            break;
        case READ_PICO_BLE_STARTING:
            snprintf(buf, cap, "%.18s · 启动中", name);
            break;
        case READ_PICO_BLE_ERROR:
            snprintf(buf, cap, "启动失败");
            break;
        case READ_PICO_BLE_STOPPED:
        default:
            snprintf(buf, cap, "未运行");
            break;
    }
}
