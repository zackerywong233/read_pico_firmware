/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：负责自动唤醒时的定时看板拉取与推屏。
 * English: Fetches and presents the dashboard on an automatic wake.
 *
 * 冻结：任何联网或解析失败都保留上一屏；运行总时长不超过 25 秒。
 * Frozen: Preserve the previous screen on network or parse failure; cap total runtime at 25 seconds.
 */
#include "ambient_sync.h"
#include "ambient_sync_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdio.h>

#include "book_image.h"
#include "book_store.h"
#include "cJSON.h"
#include "display.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "read_pico_pmu.h"
#include "read_pico_sd.h"
#include "read_pico_transfer.h"
#include "reading_stats.h"
#include "settings.h"
#include "time_sync.h"
#include "ui_heatmap.h"
#include "ui_kit.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AMBIENT_HTTP_MAX (2u * 1024u * 1024u)
#define AMBIENT_RUN_MAX_MS 25000

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
    bool overflow;
} ambient_response_t;

static esp_err_t ambient_http_event(esp_http_client_event_t *event) {
    ambient_response_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->data_len) return ESP_OK;
    size_t add = (size_t)event->data_len;
    if (response->length > AMBIENT_HTTP_MAX || add > AMBIENT_HTTP_MAX - response->length) {
        response->overflow = true;
        return ESP_ERR_INVALID_SIZE;
    }
    size_t need = response->length + add;
    if (need > response->capacity) {
        size_t cap = response->capacity ? response->capacity : 4096;
        while (cap < need) cap = cap > AMBIENT_HTTP_MAX / 2 ? AMBIENT_HTTP_MAX : cap * 2;
        uint8_t *next = heap_caps_realloc(response->data, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!next) next = realloc(response->data, cap);
        if (!next) return ESP_ERR_NO_MEM;
        response->data = next;
        response->capacity = cap;
    }
    memcpy(response->data + response->length, event->data, add);
    response->length += add;
    return ESP_OK;
}

static bool ambient_fetch(const app_ambient_settings_t *config, ambient_response_t *response) {
    char ssid[33]; bool configured = false;
    if (read_pico_transfer_get_saved_wifi(ssid, &configured) != ESP_OK || !configured) return false;
    book_store_root_t root;
    if (book_store_upload_root(&root) != ESP_OK) return false;
    read_pico_transfer_cfg_t transfer = {
        .mode = READ_PICO_TRANSFER_MODE_STA, .root_dir = root.path, .is_flash = root.is_flash,
        .file_limit = book_store_file_limit(&root), .sta_ip_cb = app_time_sync_sta_ip,
        .service_poll_cb = app_time_sync_poll, .network_stop_cb = app_time_sync_stop,
    };
    if (read_pico_transfer_start(&transfer) != ESP_OK) return false;
    int64_t started = esp_timer_get_time() / 1000;
    int64_t deadline = started + AMBIENT_RUN_MAX_MS;
    int64_t connect_deadline = started + 12000;
    if (connect_deadline > deadline) connect_deadline = deadline;
    read_pico_transfer_status_t status = {0};
    while (esp_timer_get_time() / 1000 < connect_deadline) {
        read_pico_transfer_service_poll();
        read_pico_transfer_get_status(&status);
        app_time_sync_poll();
        if (status.network_ready) break;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    bool ok = false;
    if (status.network_ready && esp_timer_get_time() / 1000 < deadline) {
        int remaining = (int)(deadline - esp_timer_get_time() / 1000);
        int timeout = remaining < 5000 ? remaining : 5000;
        esp_http_client_config_t http = {
            .url = config->url, .timeout_ms = timeout, .buffer_size = 4096,
            .event_handler = ambient_http_event, .user_data = response,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t client = esp_http_client_init(&http);
        if (client) {
            if (config->auth_header[0]) {
                if (strncasecmp(config->auth_header, "Bearer ", 7) == 0)
                    (void)esp_http_client_set_header(client, "Authorization", config->auth_header);
                else (void)esp_http_client_set_header(client, "Authorization", config->auth_header);
            }
            esp_err_t err = esp_http_client_perform(client);
            ok = err == ESP_OK && esp_http_client_get_status_code(client) >= 200 &&
                esp_http_client_get_status_code(client) < 300 && !response->overflow;
            esp_http_client_cleanup(client);
            if (ok) {
                int64_t sync_deadline = esp_timer_get_time() / 1000 + 2500;
                if (sync_deadline > deadline) sync_deadline = deadline;
                do {
                    app_time_sync_poll();
                    vTaskDelay(pdMS_TO_TICKS(200));
                } while (esp_timer_get_time() / 1000 < sync_deadline);
            }
        }
    }
    read_pico_transfer_stop();
    app_time_sync_stop();
    memset(ssid, 0, sizeof(ssid));
    memset(&root, 0, sizeof(root));
    return ok;
}

static bool draw_json_card(uint8_t *fb, const uint8_t *data, size_t length) {
    if (length > 8192 || !length) return false;
    char *terminated = malloc(length + 1);
    if (!terminated) return false;
    memcpy(terminated, data, length);
    terminated[length] = '\0';
    cJSON *json = cJSON_ParseWithLengthOpts(terminated, length + 1, NULL, true);
    free(terminated);
    if (!json) return false;
    const char *keys[] = {"title", "subtitle", "body", "footer"};
    char *values[4] = {0};
    bool valid = true;
    for (int i = 0; i < 4; ++i) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(json, keys[i]);
        if (item && !cJSON_IsString(item)) valid = false;
        if (cJSON_IsString(item) && item->valuestring) {
            size_t n = strlen(item->valuestring);
            if (n > 512) valid = false;
            else values[i] = strdup(item->valuestring);
        }
    }
    if (valid) {
        ui_clear_page(fb);
        ui_draw_header(fb, values[0] && values[0][0] ? values[0] : "定时看板", values[1] ? values[1] : "");
        ui_text_vc(fb, epd_rotated_display_width() / 2, epd_rotated_display_height() / 2,
                   UI_PX_SECTION, values[2] ? values[2] : "", EPD_DRAW_ALIGN_CENTER, false);
        if (values[3]) ui_text(fb, UI_MARGIN, epd_rotated_display_height() - 72,
                               UI_PX_CAPTION, values[3], EPD_DRAW_ALIGN_LEFT, false);
    }
    for (int i = 0; i < 4; ++i) free(values[i]);
    cJSON_Delete(json);
    return valid;
}

static bool draw_image_card(uint8_t *fb, const uint8_t *data, size_t length) {
    uint8_t *pixels = NULL;
    uint16_t width = 0, height = 0;
    if (!book_image_decode(data, length, AMBIENT_HTTP_MAX, &pixels, &width, &height) || !pixels) return false;
    int sw = epd_rotated_display_width(), sh = epd_rotated_display_height();
    int w = width, h = height;
    if (w > sw) { h = h * sw / w; w = sw; }
    if (h > sh) { w = w * sh / h; h = sh; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int left = (sw - w) / 2, top = (sh - h) / 2;
    ui_clear_page(fb);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        size_t src = (size_t)((int64_t)y * height / h) * width + (size_t)((int64_t)x * width / w);
        epd_draw_pixel(left + x, top + y, pixels[src], fb);
    }
    free(pixels);
    return true;
}

bool ambient_sync_run(EpdiyHighlevelState *hl, uint8_t *framebuffer) {
    if (!hl || !framebuffer) return false;
    app_ambient_settings_t config;
    app_settings_ambient_get(&config);
    bool success = false;
    if (config.source_type == 2) {
        const pmu_snapshot_t *pmu = read_pico_pmu_get();
        epd_hl_set_all_white(hl);
        ui_clear_page(framebuffer);
        ui_draw_header(framebuffer, "我的看板", "定时刷新");
        char line[128];
        time_t now = pmu && pmu->time_ok && pmu->unix_sec
            ? (time_t)pmu->unix_sec : time(NULL);
        struct tm local;
        time_t local_now = now + (time_t)app_settings_timezone_min() * 60;
        if (now >= 946684800 && gmtime_r(&local_now, &local)) {
            strftime(line, sizeof(line), "%Y-%m-%d  %H:%M", &local);
            ui_text_vc(framebuffer, epd_rotated_display_width() / 2, 260, UI_PX_TITLE, line, EPD_DRAW_ALIGN_CENTER, false);
        }
        if (pmu && pmu->status_ok) {
            snprintf(line, sizeof(line), "电量 %u%%  ·  电池 %u.%03u V", pmu->soc_permille / 10,
                     pmu->battery_mv / 1000, pmu->battery_mv % 1000);
            ui_text_vc(framebuffer, epd_rotated_display_width() / 2, 400, UI_PX_BODY, line, EPD_DRAW_ALIGN_CENTER, false);
        }
        char book[128] = {0}; uint8_t percent = 0;
        if (reading_stats_get_last_book(book, sizeof(book), &percent))
            snprintf(line, sizeof(line), "最近阅读：%s  %u%%", book, percent);
        else snprintf(line, sizeof(line), "累计阅读：%lu 分钟", (unsigned long)(reading_stats_total_seconds() / 60));
        ui_text(framebuffer, UI_MARGIN, 520, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
        read_pico_sd_info_t sd = {0};
        int note_bottom_y = 570;
        if (read_pico_sd_get_info(&sd) == ESP_OK && sd.mounted) {
            FILE *file = fopen("/sdcard/dashboard.txt", "r");
            if (file) {
                int y = 570;
                int lines = 0;
                while (lines < 4 && y < epd_rotated_display_height() - 320 && fgets(line, sizeof(line), file)) {
                    line[strcspn(line, "\r\n")] = '\0';
                    if (line[0]) {
                        ui_text(framebuffer, UI_MARGIN, y, UI_PX_CAPTION, line, EPD_DRAW_ALIGN_LEFT, false);
                        y += 38;
                        ++lines;
                    }
                }
                note_bottom_y = y;
                fclose(file);
            }
        }
        if (app_settings_dashboard_items() & APP_DASH_HEATMAP) {
            uint32_t today_day = 0;
            if (now >= 946684800 && gmtime_r(&local_now, &local)) {
                today_day = reading_stats_day_from_ymd(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
            }
            reading_heatmap_t hm;
            (void)reading_stats_get_heatmap(today_day, &hm);
            int hy = note_bottom_y > 570 ? note_bottom_y + 24 : 600;
            ui_heatmap_draw_card(framebuffer, (EpdRect){UI_MARGIN, hy, ui_content_width(), 222}, &hm);
        }
        success = true;
    } else {
        ambient_response_t response = {0};
        success = ambient_fetch(&config, &response);
        if (success) {
            epd_hl_set_all_white(hl);
            success = config.source_type == 0
                ? draw_json_card(framebuffer, response.data, response.length)
                : draw_image_card(framebuffer, response.data, response.length);
        }
        free(response.data);
    }
    if (success) {
        success = epd_hl_update_screen_from_white(hl, MODE_GC16, 25) == EPD_DRAW_SUCCESS;
    }
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    time_t result_time = pmu && pmu->time_ok && pmu->unix_sec
        ? (time_t)pmu->unix_sec : time(NULL);
    if (success) app_settings_ambient_result(true, (uint32_t)result_time);
    else app_settings_ambient_result(false, (uint32_t)result_time);
    memset(&config, 0, sizeof(config));
    return success;
}
