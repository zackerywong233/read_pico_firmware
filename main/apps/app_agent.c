/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：在页面活动期间连接 MQTT/TLS，接收 Agent 文字与 HTTPS 屏保图片。
 * English: Connect to MQTT/TLS while active and receive Agent text and HTTPS saver images.
 *
 * 冻结：仅使用已保存 WiFi；离页或媒体失效时停网并等待后台任务退出。
 * 只接受有界 JSON、HTTPS 图片和证书校验；Broker 密码不写入日志。
 * 决策变更：联网等待期间阻止空闲睡眠；图片下载先取消并等待退出，再停网络。
 * Decision change: Block idle sleep while awaiting messages; cancel and join image downloads before stopping networking.
 * Frozen: Use saved WiFi only; stop networking and join workers on exit or media loss.
 * Accept bounded JSON and HTTPS images with certificate verification; never log broker passwords.
 */

#include "app.h"
#include "time_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "book_image.h"
#include "book_layout.h"
#include "book_store.h"
#include "cJSON.h"
#include "display.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "mqtt_client.h"
#include "esp_timer.h"
#include "read_pico_sd.h"
#include "read_pico_buzzer.h"
#include "read_pico_transfer.h"
#include "settings.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_wifi_qr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define AGENT_MESSAGE_MAX 8192
#define AGENT_TEXT_MAX 6144
#define AGENT_IMAGE_MAX (2u * 1024u * 1024u)
#define AGENT_BODY_PX 44
#define AGENT_RECV_TIMEOUT_MS 20000
#define AGENT_IO_TIMEOUT_MS 2000
#define AGENT_HISTORY_COUNT 20

typedef struct {
    uint32_t timestamp;
    bool image;
    bool replied;
    char title[128];
    char id[65];
    char actions[4][25];
    uint8_t action_count;
    char *body_or_path;
} agent_history_entry_t;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool too_large;
} agent_download_t;

typedef struct {
    esp_err_t err;
    uint8_t *pixels;
    uint16_t width;
    uint16_t height;
    bool saved;
} agent_image_result_t;

static book_store_root_t s_root;
static read_pico_transfer_status_t s_net;
static read_pico_agent_config_t s_config;
static bool s_net_started;
static bool s_mqtt_started;
static bool s_connected;
static bool s_subscribed;
static bool s_subscribe_failed;
static portMUX_TYPE s_agent_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_config_loaded;
static bool s_message_receiving;
static bool s_message_bad;
static volatile bool s_image_running;
static volatile bool s_image_cancel;
static bool s_last_connected;
static uint32_t s_agent_config_version;
static bool s_storage_changed;
static int64_t s_poll_ms;
static esp_mqtt_client_handle_t s_mqtt;
static QueueHandle_t s_messages;
static QueueHandle_t s_image_results;
static char *s_rx_message;
static size_t s_rx_len;
static size_t s_rx_total;
static char s_title[128];
static char *s_text;
static size_t s_page;
static uint8_t *s_image_pixels;
static uint16_t s_image_width;
static uint16_t s_image_height;
static char s_notice[128];
static char s_message_id[65];
static char s_actions[4][25];
static uint8_t s_action_count;
static bool s_message_sound;
static bool s_beep_pending;
static bool s_message_replied;
static app_redraw_t send_reply_action(int index);
static agent_history_entry_t *s_history;
static uint8_t s_history_next;
static uint8_t s_history_size;
static int8_t s_history_selected = -1;
static int8_t s_current_history = -1;
static bool s_history_list;

static char *history_strdup(const char *text) {
    if (!text) return NULL;
    size_t n = strlen(text) + 1;
    char *copy = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) copy = malloc(n);
    if (copy) memcpy(copy, text, n);
    return copy;
}

static EpdRect body_rect(void) {
    return (EpdRect){UI_MARGIN, UI_CONTENT_TOP + 40, ui_content_width(),
                     UI_CONTENT_BOTTOM - UI_CONTENT_TOP - 64};
}

static uint64_t agent_free_bytes(void *ctx) { return book_store_free_bytes(ctx); }

static bool valid_https_url(const char *url) {
    if (!url || strncmp(url, "https://", 8) || strlen(url) > 512) return false;
    const char *host = url + 8;
    const char *end = strpbrk(host, "/?#");
    if ((end ? end : host + strlen(host)) == host) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p <= 32 || *p == 127) return false;
    return true;
}

static esp_err_t append_image(agent_download_t *d, const uint8_t *data, size_t add) {
    if (!d || d->len > AGENT_IMAGE_MAX || add > AGENT_IMAGE_MAX - d->len) {
        if (d) d->too_large = true;
        return ESP_ERR_INVALID_SIZE;
    }
    if (!add) return ESP_OK;
    size_t need = d->len + add;
    if (need > d->cap) {
        size_t cap = d->cap ? d->cap : 8192;
        while (cap < need) cap = cap > AGENT_IMAGE_MAX / 2 ? AGENT_IMAGE_MAX : cap * 2;
        uint8_t *next = heap_caps_realloc(d->data, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!next) next = realloc(d->data, cap);
        if (!next) return ESP_ERR_NO_MEM;
        d->data = (char *)next;
        d->cap = cap;
    }
    memcpy(d->data + d->len, data, add);
    d->len += add;
    return ESP_OK;
}

static esp_err_t save_agent_image(const uint8_t *data, size_t len) {
    read_pico_sd_info_t sd = {0};
    if (read_pico_sd_get_info(&sd) != ESP_OK || !sd.mounted) return ESP_ERR_NOT_FOUND;
    const uint8_t png_magic[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    const char *ext = len >= sizeof(png_magic) && memcmp(data, png_magic, sizeof(png_magic)) == 0 ? "png" : "jpg";
    char path[40], part[48], backup[48];
    snprintf(path, sizeof(path), "/sdcard/agent_screensaver.%s", ext);
    snprintf(part, sizeof(part), "%s.part", path);
    snprintf(backup, sizeof(backup), "%s.bak", path);
    FILE *f = fopen(part, "wb");
    if (!f) return ESP_FAIL;
    bool ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = false;
    if (!ok) { unlink(part); return ESP_FAIL; }
    unlink(backup);
    bool had_old = rename(path, backup) == 0;
    if (rename(part, path) != 0) {
        if (had_old) rename(backup, path);
        unlink(part);
        return ESP_FAIL;
    }
    char other[40];
    snprintf(other, sizeof(other), "/sdcard/agent_screensaver.%s", !strcmp(ext, "png") ? "jpg" : "png");
    unlink(other);
    unlink(backup);
    return ESP_OK;
}

static void image_task(void *arg) {
    char *url = arg;
    agent_download_t download = {0};
    esp_http_client_config_t cfg = {
        .url = url, .timeout_ms = AGENT_IO_TIMEOUT_MS, .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    int64_t deadline = esp_timer_get_time() + (int64_t)AGENT_RECV_TIMEOUT_MS * 1000;
    esp_err_t err = client ? esp_http_client_open(client, 0) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        int64_t hdr = esp_http_client_fetch_headers(client);
        while (hdr == -ESP_ERR_HTTP_EAGAIN && !s_image_cancel && esp_timer_get_time() < deadline) {
            hdr = esp_http_client_fetch_headers(client);
        }
        if (hdr < 0) err = hdr == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
    }
    int status = (client && err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    uint8_t chunk[4096];
    while (err == ESP_OK && !s_image_cancel && !download.too_large
           && esp_timer_get_time() < deadline) {
        int n = esp_http_client_read(client, (char *)chunk, sizeof(chunk));
        if (n == 0) break;
        if (n == -ESP_ERR_HTTP_EAGAIN) continue;
        if (n < 0) { err = ESP_FAIL; break; }
        err = append_image(&download, chunk, (size_t)n);
    }
    if (s_image_cancel) err = ESP_ERR_INVALID_STATE;
    else if (err == ESP_OK && esp_timer_get_time() >= deadline) err = ESP_ERR_TIMEOUT;
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (err == ESP_OK && (status < 200 || status >= 300)) err = ESP_ERR_INVALID_RESPONSE;
    if (download.too_large) err = ESP_ERR_INVALID_SIZE;
    agent_image_result_t *result = calloc(1, sizeof(*result));
    if (result && err == ESP_OK) {
        if (!download.len || !book_image_decode((const uint8_t *)download.data, download.len,
                AGENT_IMAGE_MAX, &result->pixels, &result->width, &result->height)) err = ESP_ERR_INVALID_RESPONSE;
        else {
            esp_err_t save_err = save_agent_image((const uint8_t *)download.data, download.len);
            if (save_err == ESP_OK) result->saved = true;
            else if (save_err != ESP_ERR_NOT_FOUND) err = save_err;
        }
    }
    if (result) {
        result->err = err;
        if (err != ESP_OK) { free(result->pixels); result->pixels = NULL; }
    }
    if (!s_image_cancel && s_image_results && xQueueSend(s_image_results, &result, 0) == pdTRUE)
        result = NULL;
    if (result) {
        free(result->pixels);
        free(result);
    }
    free(download.data);
    free(url);
    s_image_running = false;
    vTaskDelete(NULL);
}

static void mqtt_event(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    (void)handler_args; (void)base;
    esp_mqtt_event_handle_t event = event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        portENTER_CRITICAL(&s_agent_mux);
        s_connected = true;
        s_subscribed = false;
        s_subscribe_failed = false;
        portEXIT_CRITICAL(&s_agent_mux);
        int id = esp_mqtt_client_subscribe(event->client, s_config.topic, 1);
        if (id < 0) {
            portENTER_CRITICAL(&s_agent_mux);
            s_subscribe_failed = true;
            portEXIT_CRITICAL(&s_agent_mux);
        }
        return;
    }
    if (event_id == MQTT_EVENT_DISCONNECTED) {
        portENTER_CRITICAL(&s_agent_mux);
        s_connected = s_subscribed = s_subscribe_failed = false;
        portEXIT_CRITICAL(&s_agent_mux);
        return;
    }
    if (event_id == MQTT_EVENT_SUBSCRIBED) {
        portENTER_CRITICAL(&s_agent_mux);
        s_subscribed = true;
        s_subscribe_failed = false;
        portEXIT_CRITICAL(&s_agent_mux);
        return;
    }
    if (event_id != MQTT_EVENT_DATA) return;
    if (event->current_data_offset == 0) {
        free(s_rx_message); s_rx_message = NULL;
        s_rx_len = 0; s_rx_total = event->total_data_len;
        s_message_receiving = true;
        s_message_bad = s_rx_total <= 0 || s_rx_total > AGENT_MESSAGE_MAX || !event->topic || event->topic_len != (int)strlen(s_config.topic) ||
            memcmp(event->topic, s_config.topic, (size_t)event->topic_len) != 0;
        if (!s_message_bad) s_rx_message = malloc(s_rx_total + 1);
        if (!s_rx_message) s_message_bad = true;
    }
    if (!s_message_receiving || s_message_bad || !event->data || event->current_data_offset < 0 || event->data_len < 0 ||
        (size_t)event->current_data_offset != s_rx_len || (size_t)event->data_len > s_rx_total - s_rx_len) {
        s_message_bad = true;
    } else {
        memcpy(s_rx_message + s_rx_len, event->data, (size_t)event->data_len);
        s_rx_len += (size_t)event->data_len;
    }
    if (s_message_receiving && s_rx_len == s_rx_total) {
        if (!s_message_bad) {
            s_rx_message[s_rx_len] = 0;
            if (xQueueSend(s_messages, &s_rx_message, 0) == pdTRUE) s_rx_message = NULL;
        }
        free(s_rx_message); s_rx_message = NULL;
        s_message_receiving = false; s_message_bad = false; s_rx_len = s_rx_total = 0;
    } else if (s_message_bad) {
        free(s_rx_message); s_rx_message = NULL;
        s_message_receiving = false; s_message_bad = false; s_rx_len = s_rx_total = 0;
    }
}

static void mqtt_stop(void) {
    if (!s_mqtt) return;
    (void)esp_mqtt_client_stop(s_mqtt);
    (void)esp_mqtt_client_destroy(s_mqtt);
    s_mqtt = NULL; s_mqtt_started = false;
    portENTER_CRITICAL(&s_agent_mux);
    s_connected = s_subscribed = s_subscribe_failed = false;
    portEXIT_CRITICAL(&s_agent_mux);
}

static void start_mqtt(void) {
    if (s_mqtt || !s_net.network_ready) return;
    esp_mqtt_client_config_t cfg = {0};
    cfg.broker.address.uri = s_config.broker_uri;
    cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.credentials.username = s_config.username[0] ? s_config.username : NULL;
    cfg.credentials.authentication.password = s_config.password[0] ? s_config.password : NULL;
    cfg.session.keepalive = 60;
    cfg.buffer.size = 4096;
    s_mqtt = esp_mqtt_client_init(&cfg);
    if (!s_mqtt) { snprintf(s_notice, sizeof(s_notice), "MQTT 初始化失败"); return; }
    esp_err_t err = esp_mqtt_client_register_event(s_mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    if (err == ESP_OK) err = esp_mqtt_client_start(s_mqtt);
    if (err != ESP_OK) {
        snprintf(s_notice, sizeof(s_notice), "MQTT 启动失败：%s", esp_err_to_name(err));
        mqtt_stop();
    } else {
        s_mqtt_started = true;
        snprintf(s_notice, sizeof(s_notice), "正在连接 MQTT/TLS…");
    }
}

static void process_message(char *payload) {
    cJSON *json = cJSON_ParseWithLengthOpts(payload, strlen(payload) + 1, NULL, true);
    if (!json) { snprintf(s_notice, sizeof(s_notice), "消息不是有效 JSON"); free(payload); return; }
    cJSON *type = cJSON_GetObjectItemCaseSensitive(json, "type");
    s_message_id[0] = '\0'; s_action_count = 0; s_message_sound = false;
    s_message_replied = false;
    cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "id");
    bool metadata_valid = !id || (cJSON_IsString(id) && id->valuestring && strlen(id->valuestring) <= 64);
    if (metadata_valid && cJSON_IsString(id))
        snprintf(s_message_id, sizeof(s_message_id), "%s", id->valuestring);
    cJSON *sound = cJSON_GetObjectItemCaseSensitive(json, "sound");
    if (sound && !cJSON_IsBool(sound)) metadata_valid = false;
    s_message_sound = cJSON_IsTrue(sound);
    cJSON *actions = cJSON_GetObjectItemCaseSensitive(json, "actions");
    if (actions && (!cJSON_IsArray(actions) || cJSON_GetArraySize(actions) > 4)) metadata_valid = false;
    if (cJSON_IsArray(actions)) {
        cJSON *action = NULL;
        cJSON_ArrayForEach(action, actions) {
            if (s_action_count >= 4) { metadata_valid = false; break; }
            if (!cJSON_IsString(action) || !action->valuestring || strlen(action->valuestring) > 24) {
                metadata_valid = false; s_action_count = 0; break;
            }
            snprintf(s_actions[s_action_count++], sizeof(s_actions[0]), "%s", action->valuestring);
        }
    }
    if (!metadata_valid) {
        snprintf(s_notice, sizeof(s_notice), "Agent 消息扩展字段无效");
        cJSON_Delete(json); free(payload); return;
    }
    if (cJSON_IsString(type) && !strcmp(type->valuestring, "text")) {
        cJSON *title = cJSON_GetObjectItemCaseSensitive(json, "title");
        cJSON *text = cJSON_GetObjectItemCaseSensitive(json, "text");
        if (!cJSON_IsString(text) || !text->valuestring || strlen(text->valuestring) > AGENT_TEXT_MAX) {
            snprintf(s_notice, sizeof(s_notice), "文字消息缺少正文或超过 6 KiB");
        } else {
            free(s_image_pixels); s_image_pixels = NULL; s_image_width = s_image_height = 0;
            book_layout_free();
            free(s_text); s_text = strdup(text->valuestring); s_page = 0;
            snprintf(s_title, sizeof(s_title), "%s", cJSON_IsString(title) && title->valuestring ? title->valuestring : "Agent 消息");
            if (!s_text || !book_layout_build(s_text, strlen(s_text), body_rect(), AGENT_BODY_PX)) {
                free(s_text); s_text = NULL; snprintf(s_notice, sizeof(s_notice), "文字排版失败");
            } else {
                snprintf(s_notice, sizeof(s_notice), "收到 Agent 文字消息"); s_beep_pending = s_message_sound;
                agent_history_entry_t *entry = s_history ? &s_history[s_history_next] : NULL;
                if (entry) {
                free(entry->body_or_path); memset(entry, 0, sizeof(*entry));
                entry->timestamp = (uint32_t)time(NULL); entry->image = false;
                snprintf(entry->title, sizeof(entry->title), "%s", s_title);
                snprintf(entry->id, sizeof(entry->id), "%s", s_message_id);
                entry->action_count = s_action_count;
                memcpy(entry->actions, s_actions, sizeof(entry->actions));
                entry->body_or_path = history_strdup(text->valuestring);
                s_current_history = (int8_t)s_history_next;
                s_history_selected = -1;
                s_history_next = (uint8_t)((s_history_next + 1) % AGENT_HISTORY_COUNT);
                if (s_history_size < AGENT_HISTORY_COUNT) ++s_history_size;
                }
            }
        }
    } else if (cJSON_IsString(type) && !strcmp(type->valuestring, "image")) {
        cJSON *url = cJSON_GetObjectItemCaseSensitive(json, "url");
        if (!cJSON_IsString(url) || !valid_https_url(url->valuestring) || s_image_running) {
            snprintf(s_notice, sizeof(s_notice), "图片地址无效或上一张仍在下载");
        } else {
            char *copy = strdup(url->valuestring);
            s_image_cancel = false;
            s_image_running = true;
            if (!copy || xTaskCreate(image_task, "agent_image", 10240, copy, 4, NULL) != pdPASS) {
                s_image_running = false;
                free(copy); snprintf(s_notice, sizeof(s_notice), "无法启动图片下载");
            } else {
                snprintf(s_notice, sizeof(s_notice), "正在下载 Agent 图片…");
                agent_history_entry_t *entry = s_history ? &s_history[s_history_next] : NULL;
                if (entry) {
                free(entry->body_or_path); memset(entry, 0, sizeof(*entry));
                entry->timestamp = (uint32_t)time(NULL); entry->image = true;
                snprintf(entry->title, sizeof(entry->title), "Agent 图片");
                snprintf(entry->id, sizeof(entry->id), "%s", s_message_id);
                entry->action_count = s_action_count;
                memcpy(entry->actions, s_actions, sizeof(entry->actions));
                entry->body_or_path = history_strdup(url->valuestring);
                s_current_history = (int8_t)s_history_next;
                s_history_selected = -1;
                s_history_next = (uint8_t)((s_history_next + 1) % AGENT_HISTORY_COUNT);
                if (s_history_size < AGENT_HISTORY_COUNT) ++s_history_size;
                }
            }
        }
    } else snprintf(s_notice, sizeof(s_notice), "不支持的消息类型");
    cJSON_Delete(json);
    free(payload);
}

static void draw_image(uint8_t *fb) {
    ui_clear_page(fb);
    ui_draw_header(fb, "Agent 图片屏保", s_notice);
    if (!s_image_pixels || !s_image_width || !s_image_height) {
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 40, UI_PX_BODY, "尚未收到图片", EPD_DRAW_ALIGN_LEFT, false);
        return;
    }
    EpdRect area = body_rect();
    int w = s_image_width, h = s_image_height;
    if (w > area.width) { h = h * area.width / w; w = area.width; }
    if (h > area.height) { w = w * area.height / h; h = area.height; }
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int left = area.x + (area.width - w) / 2, top = area.y + (area.height - h) / 2;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        size_t src = (size_t)((int64_t)y * s_image_height / h) * s_image_width +
                     (size_t)((int64_t)x * s_image_width / w);
        epd_draw_pixel(left + x, top + y, s_image_pixels[src], fb);
    }
}

static void draw_actions(uint8_t *fb) {
    if (!s_action_count) return;
    int width = ui_content_width() / s_action_count;
    for (int i = 0; i < s_action_count; ++i) {
        EpdRect r = {UI_MARGIN + i * width, UI_CONTENT_BOTTOM - 74, width - UI_GAP, 56};
        char label[40];
        snprintf(label, sizeof(label), "%s%s", s_message_replied && i == 0 ? "✓ " : "", s_actions[i]);
        ui_draw_button(fb, r, label, false);
    }
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    if (!ttf_font_ready()) { ui_draw_no_font_page(fb, false, false); return; }
    if (s_history_list) {
        ui_clear_page(fb);
        ui_draw_header(fb, "Agent 历史收件", "点选消息查看详情，点顶部返回消息");
        uint8_t oldest = (uint8_t)((s_history_next + AGENT_HISTORY_COUNT - s_history_size) % AGENT_HISTORY_COUNT);
        int y = UI_CONTENT_TOP + 12;
        for (int i = 0; i < s_history_size; ++i) {
            uint8_t index = (uint8_t)((oldest + i) % AGENT_HISTORY_COUNT);
            agent_history_entry_t *entry = &s_history[index];
            char label[170];
            snprintf(label, sizeof(label), "%s%s · %s", entry->image ? "图片" : "文字",
                     entry->replied ? " · 已回复" : "", entry->title);
            EpdRect row = {UI_MARGIN, y, ui_content_width(), 38};
            ui_draw_button(fb, row, label, index == s_history_selected);
            y += 41;
            if (y > UI_CONTENT_BOTTOM - 40) break;
        }
        ui_draw_menu_handle(fb, false);
        return;
    }
    if (s_history_selected >= 0 && s_history[s_history_selected].body_or_path) {
        agent_history_entry_t *entry = &s_history[s_history_selected];
        ui_clear_page(fb);
        ui_draw_header(fb, entry->title, entry->replied ? "已回复" : "历史消息");
        if (entry->image) {
            char source[96];
            snprintf(source, sizeof(source), "图片来源：%.72s", entry->body_or_path);
            ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 40, UI_PX_CAPTION, source, EPD_DRAW_ALIGN_LEFT, false);
        }
        else if (s_text) book_layout_draw_page(fb, s_page, body_rect(), AGENT_BODY_PX);
        if (!entry->image && s_text) {
            char page[48]; snprintf(page, sizeof(page), "第 %u / %u 页", (unsigned)s_page + 1,
                                    (unsigned)book_layout_page_count());
            ui_text(fb, UI_MARGIN, UI_CONTENT_BOTTOM - 12, UI_PX_CAPTION, page, EPD_DRAW_ALIGN_LEFT, false);
        }
        draw_actions(fb);
        ui_draw_menu_handle(fb, false);
        return;
    }
    if (s_image_pixels) { draw_image(fb); draw_actions(fb); ui_draw_menu_handle(fb, false); return; }
    ui_clear_page(fb);
    ui_draw_header(fb, s_title[0] ? s_title : "Agent 收件", s_notice);
    if (s_text) {
        book_layout_draw_page(fb, s_page, body_rect(), AGENT_BODY_PX);
        char page[48]; snprintf(page, sizeof(page), "第 %u / %u 页", (unsigned)s_page + 1, (unsigned)book_layout_page_count());
        ui_text(fb, UI_MARGIN, UI_CONTENT_BOTTOM - 12, UI_PX_CAPTION, page, EPD_DRAW_ALIGN_LEFT, false);
    } else {
        read_pico_transfer_status_t status; read_pico_transfer_get_status(&status);
        portENTER_CRITICAL(&s_agent_mux);
        bool subscribed = s_subscribed;
        portEXIT_CRITICAL(&s_agent_mux);
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 38, UI_PX_BODY,
            !status.wifi_configured ? "请先在 Wi-Fi 传输页连接已有 WiFi" :
            !s_config_loaded ? "请在屏幕显示的网址配置 MQTT/TLS" :
            !subscribed ? "正在连接或订阅主题…" : "等待 Agent 投递文字或图片…",
            EPD_DRAW_ALIGN_LEFT, false);
        if (status.url[0]) {
            char local_url[96];
            snprintf(local_url, sizeof(local_url), "http://%s.local", status.mdns_host[0] ? status.mdns_host : "readpico");
            ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 116, UI_PX_CAPTION, local_url, EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 148, UI_PX_CAPTION, status.url, EPD_DRAW_ALIGN_LEFT, false);
        }
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 176, UI_PX_CAPTION, "JSON: {\"type\":\"text\",\"title\":\"标题\",\"text\":\"内容\"}", EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 226, UI_PX_CAPTION, "图片: {\"type\":\"image\",\"url\":\"https://…\"}", EPD_DRAW_ALIGN_LEFT, false);
    }
    draw_actions(fb);
    ui_draw_menu_handle(fb, false);
}

static void network_start(void) {
    if (read_pico_transfer_get_saved_wifi(s_net.wifi_ssid, &s_net.wifi_configured) != ESP_OK || !s_net.wifi_configured) {
        snprintf(s_notice, sizeof(s_notice), "请先在 Wi-Fi 传输页连接已有 WiFi"); return;
    }
    if (book_store_upload_root(&s_root) != ESP_OK) { snprintf(s_notice, sizeof(s_notice), "图书存储不可用"); return; }
    read_pico_transfer_cfg_t cfg = {.mode = READ_PICO_TRANSFER_MODE_STA, .root_dir = s_root.path,
        .is_flash = s_root.is_flash, .file_limit = book_store_file_limit(&s_root),
        .free_bytes_cb = agent_free_bytes, .free_bytes_ctx = &s_root,
        .file_changed_cb = book_store_forget_changed_path,
        .sta_ip_cb = app_time_sync_sta_ip,
        .service_poll_cb = app_time_sync_poll,
        .network_stop_cb = app_time_sync_stop};
    esp_err_t err = read_pico_transfer_start(&cfg);
    if (err != ESP_OK) { snprintf(s_notice, sizeof(s_notice), "WiFi 启动失败：%s", esp_err_to_name(err)); return; }
    s_net_started = true;
    err = read_pico_transfer_get_agent_config(&s_config);
    s_config_loaded = err == ESP_OK;
    s_agent_config_version = read_pico_transfer_agent_config_version();
    snprintf(s_notice, sizeof(s_notice), err == ESP_OK ? "配置已加载，等待网络连接…" : "请在屏幕显示的网址配置 MQTT/TLS");
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    s_messages = xQueueCreate(2, sizeof(char *));
    s_image_results = xQueueCreate(1, sizeof(agent_image_result_t *));
    s_history = heap_caps_calloc(AGENT_HISTORY_COUNT, sizeof(*s_history), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_history) s_history = calloc(AGENT_HISTORY_COUNT, sizeof(*s_history));
    s_history_next = s_history_size = 0; s_history_selected = s_current_history = -1; s_history_list = false;
    s_title[0] = 0; s_page = 0; s_poll_ms = 0;
    s_config_loaded = false;
    s_storage_changed = false;
    s_image_cancel = false;
    s_last_connected = false;
    portENTER_CRITICAL(&s_agent_mux);
    s_connected = s_subscribed = s_subscribe_failed = false;
    portEXIT_CRITICAL(&s_agent_mux);
    snprintf(s_notice, sizeof(s_notice), "正在启动 Agent 收件…");
    (void)ttf_font_suspend_sd(true); display_set_bulk_io(true);
    if (!s_messages || !s_image_results) { snprintf(s_notice, sizeof(s_notice), "内存不足"); return; }
    network_start();
}

static void network_stop(void) {
    mqtt_stop();
    if (s_net_started) {
        read_pico_transfer_stop();
        read_pico_transfer_get_status(&s_net);
        s_storage_changed = s_storage_changed || s_net.changed_count != 0;
    }
    s_net_started = false;
    s_net.network_ready = false;
    ui_wifi_qr_clear();
}

static void cleanup(void) {
    s_image_cancel = true;
    while (s_image_running) vTaskDelay(pdMS_TO_TICKS(20));
    network_stop();
    if (s_messages) {
        char *message = NULL;
        while (xQueueReceive(s_messages, &message, 0) == pdTRUE) free(message);
        vQueueDelete(s_messages); s_messages = NULL;
    }
    if (s_image_results) {
        agent_image_result_t *result = NULL;
        while (xQueueReceive(s_image_results, &result, 0) == pdTRUE) { if (result) { free(result->pixels); free(result); } }
        vQueueDelete(s_image_results); s_image_results = NULL;
    }
    free(s_rx_message); s_rx_message = NULL;
    if (s_history) {
        for (int i = 0; i < AGENT_HISTORY_COUNT; ++i) {
            free(s_history[i].body_or_path);
            memset(&s_history[i], 0, sizeof(s_history[i]));
        }
        free(s_history);
        s_history = NULL;
    }
    s_history_next = s_history_size = 0; s_history_selected = s_current_history = -1; s_history_list = false;
    book_layout_free(); free(s_text); s_text = NULL;
    free(s_image_pixels); s_image_pixels = NULL; s_image_width = s_image_height = 0;
    memset(&s_config, 0, sizeof(s_config));
    s_config_loaded = false;
    s_last_connected = false;
    if (s_storage_changed) {
        book_store_notify_changed();
        s_storage_changed = false;
    }
}

static void agent_on_exit(app_ctx_t *ctx) {
    (void)ctx; cleanup(); display_set_bulk_io(false); (void)ttf_font_suspend_sd(false);
}

static void on_media_lost(app_ctx_t *ctx) { (void)ctx; cleanup(); display_set_bulk_io(false); (void)ttf_font_suspend_sd(false); snprintf(s_notice, sizeof(s_notice), "TF 卡已移除，网络和图片任务已停止"); }

static app_redraw_t on_tick(app_ctx_t *ctx) {
    (void)ctx;
    if (s_beep_pending) {
        s_beep_pending = false;
        if (!app_settings_muted() && !read_pico_buzzer_score_busy()) {
            (void)read_pico_buzzer_tone(880, 70);
            vTaskDelay(pdMS_TO_TICKS(35));
            (void)read_pico_buzzer_tone(1175, 70);
        }
    }
    int64_t now = esp_timer_get_time() / 1000;
    bool was_ready = s_net.network_ready;
    portENTER_CRITICAL(&s_agent_mux);
    bool was_connected = s_connected;
    bool was_subscribed = s_subscribed;
    bool was_subscribe_failed = s_subscribe_failed;
    portEXIT_CRITICAL(&s_agent_mux);
    bool was_mqtt_started = s_mqtt_started;
    if (now - s_poll_ms >= 500) {
        s_poll_ms = now;
        if (s_net_started) {
            read_pico_transfer_service_poll();
            read_pico_transfer_get_status(&s_net);
            uint32_t version = read_pico_transfer_agent_config_version();
            if (version != s_agent_config_version) {
                mqtt_stop();
                memset(&s_config, 0, sizeof(s_config));
                s_config_loaded = read_pico_transfer_get_agent_config(&s_config) == ESP_OK;
                s_agent_config_version = version;
                snprintf(s_notice, sizeof(s_notice), "配置已更新，正在重连 MQTT/TLS…");
                return APP_REDRAW_PAGE;
            }
            if (s_net.network_ready && !s_mqtt) {
                if (!s_config_loaded) s_config_loaded = read_pico_transfer_get_agent_config(&s_config) == ESP_OK;
                if (s_config.broker_uri[0]) start_mqtt();
            }
        }
        if (was_ready != s_net.network_ready || was_mqtt_started != s_mqtt_started)
            return APP_REDRAW_PAGE;
    }
    char *payload = NULL;
    if (s_messages && xQueueReceive(s_messages, &payload, 0) == pdTRUE) { process_message(payload); return APP_REDRAW_PAGE; }
    agent_image_result_t *result = NULL;
    if (s_image_results && xQueueReceive(s_image_results, &result, 0) == pdTRUE) {
        if (result && result->err == ESP_OK) {
            free(s_image_pixels); s_image_pixels = result->pixels; result->pixels = NULL;
            s_image_width = result->width; s_image_height = result->height;
            s_beep_pending = s_message_sound;
            if (result->saved) {
                app_settings_set_saver_style(APP_SAVER_IMAGE);
                snprintf(s_notice, sizeof(s_notice), "图片已保存到 TF 卡，并设为锁屏屏保");
            } else snprintf(s_notice, sizeof(s_notice), "未插 TF 卡，仅临时预览（未设为屏保）");
        } else snprintf(s_notice, sizeof(s_notice), "图片处理失败：%s", esp_err_to_name(result ? result->err : ESP_FAIL));
        if (result) { free(result->pixels); free(result); }
        return APP_REDRAW_PAGE;
    }
    portENTER_CRITICAL(&s_agent_mux);
    bool connected = s_connected;
    bool subscribed = s_subscribed;
    bool subscribe_failed = s_subscribe_failed;
    portEXIT_CRITICAL(&s_agent_mux);
    if (s_last_connected != connected || was_connected != connected
        || was_subscribed != subscribed || was_subscribe_failed != subscribe_failed) {
        s_last_connected = connected;
        if (subscribe_failed) snprintf(s_notice, sizeof(s_notice), "主题订阅失败");
        else if (subscribed) snprintf(s_notice, sizeof(s_notice), "已订阅主题，等待 Agent 投递…");
        else snprintf(s_notice, sizeof(s_notice), connected
            ? "MQTT 已连接，正在订阅主题…" : "MQTT 已断开，等待重连…");
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t send_reply_action(int index) {
    bool connected;
    portENTER_CRITICAL(&s_agent_mux); connected = s_connected; portEXIT_CRITICAL(&s_agent_mux);
    if (index < 0 || index >= s_action_count || !s_message_id[0] || !s_mqtt || !connected) return APP_REDRAW_NONE;
    char topic[READ_PICO_AGENT_TOPIC_MAX + 8];
    snprintf(topic, sizeof(topic), "%s/reply", s_config.topic);
    cJSON *reply = cJSON_CreateObject();
    if (!reply) return APP_REDRAW_NONE;
    cJSON_AddStringToObject(reply, "id", s_message_id);
    cJSON_AddStringToObject(reply, "action", s_actions[index]);
    cJSON_AddNumberToObject(reply, "ts", (double)time(NULL));
    char *json = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    if (!json) return APP_REDRAW_NONE;
    int msg_id = esp_mqtt_client_publish(s_mqtt, topic, json, 0, 1, 0);
    cJSON_free(json);
    if (msg_id < 0) snprintf(s_notice, sizeof(s_notice), "回复发送失败");
    else {
        s_message_replied = true;
        if (s_current_history >= 0 && s_current_history < AGENT_HISTORY_COUNT)
            s_history[(uint8_t)s_current_history].replied = true;
        snprintf(s_notice, sizeof(s_notice), "已回复：%s", s_actions[index]);
    }
    return APP_REDRAW_PAGE;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    (void)ctx;
    if (s_history_list) {
        if (event->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        if (event->y < UI_CONTENT_TOP + 40) { s_history_list = false; return APP_REDRAW_PAGE; }
        int row = (event->y - (UI_CONTENT_TOP + 12)) / 46;
        uint8_t oldest = (uint8_t)((s_history_next + AGENT_HISTORY_COUNT - s_history_size) % AGENT_HISTORY_COUNT);
        if (row < 0 || row >= s_history_size) return APP_REDRAW_NONE;
        s_history_selected = (int8_t)((oldest + row) % AGENT_HISTORY_COUNT);
        agent_history_entry_t *entry = &s_history[s_history_selected];
        s_current_history = s_history_selected;
        s_message_replied = entry->replied;
        s_action_count = entry->action_count;
        memcpy(s_actions, entry->actions, sizeof(s_actions));
        snprintf(s_message_id, sizeof(s_message_id), "%s", entry->id);
        snprintf(s_title, sizeof(s_title), "%s", entry->title);
        book_layout_free(); free(s_text); s_text = NULL; s_page = 0;
        if (!entry->image && entry->body_or_path) {
            s_text = strdup(entry->body_or_path);
            if (s_text) (void)book_layout_build(s_text, strlen(s_text), body_rect(), AGENT_BODY_PX);
        }
        return APP_REDRAW_PAGE;
    }
    if (event->type == UI_GESTURE_TAP && event->y < UI_CONTENT_TOP + 32 && s_history_size) {
        s_history_list = true;
        return APP_REDRAW_PAGE;
    }
    if (event->type == UI_GESTURE_TAP && s_action_count &&
        event->y >= UI_CONTENT_BOTTOM - 84 && s_message_id[0]) {
        int width = ui_content_width() / s_action_count;
        int index = (event->x - UI_MARGIN) / width;
        if (index >= 0 && index < s_action_count) return send_reply_action(index);
    }
    if (!s_text || s_image_pixels) return APP_REDRAW_NONE;
    if (event->type == UI_GESTURE_SWIPE_L || (event->type == UI_GESTURE_TAP && event->x > epd_rotated_display_width() / 2)) {
        if (s_page + 1 < book_layout_page_count()) { ++s_page; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }
    if (event->type == UI_GESTURE_SWIPE_R || (event->type == UI_GESTURE_TAP && event->x <= epd_rotated_display_width() / 2)) {
        if (s_page) --s_page;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (!s_action_count && key == UI_KEY_1 && s_history_size) {
        s_history_list = !s_history_list;
        if (!s_history_list) s_history_selected = -1;
        return APP_REDRAW_PAGE;
    }
    if (s_action_count && s_message_id[0]) {
        int index = key == UI_KEY_1 ? 0 : key == UI_KEY_2 ? 1 : key == UI_KEY_3 ? 2 : -1;
        if (index >= 0 && index < s_action_count) return send_reply_action(index);
    }
    if (key == UI_KEY_1) { if (s_text && s_page) { --s_page; return APP_REDRAW_PAGE; } return APP_REDRAW_NONE; }
    if (key == UI_KEY_2) return APP_REDRAW_FULL;
    if (key == UI_KEY_3) { if (s_text && s_page + 1 < book_layout_page_count()) { ++s_page; return APP_REDRAW_PAGE; } return APP_REDRAW_NONE; }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key_long(app_ctx_t *ctx, int key) {
    if (key == UI_KEY_1 && s_history_size) {
        s_history_list = !s_history_list;
        if (!s_history_list) s_history_selected = -1;
        return APP_REDRAW_PAGE;
    }
    if (key != UI_KEY_2) return APP_REDRAW_NONE;
    ctx->request_menu = true;
    return APP_REDRAW_NONE;
}

const app_desc_t app_agent = {
    .title = "Agent 收件",
    .detail = "MQTT/TLS 文字与图片屏保",
    .owns_keys = true,
    .blocks_idle_sleep = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = agent_on_exit,
    .on_media_lost = on_media_lost,
    .on_tick = on_tick,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_key_long = on_key_long,
};
