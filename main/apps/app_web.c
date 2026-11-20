/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：联网抓取网页并以墨水屏阅读布局显示；本页同时开放同网剪藏入口。
 * English: Fetch web pages for e-paper reading and expose the local browser-clip endpoint.
 *
 * 冻结：只提取 HTML 正文，不执行脚本或 CSS；Wi-Fi 使用已保存的 STA 配置，离页停服。
 * 剪藏只允许同网设备持本次开机口令写入 TXT 书架。
 * Frozen: Extract HTML text without executing scripts or CSS; use saved STA credentials and stop networking on exit.
 * Browser clips require the current boot token and are stored as TXT books.
 */

#include "app.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "book_layout.h"
#include "book_progress.h"
#include "book_store.h"
#include "display.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "html_text.h"
#include "read_pico_transfer.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_wifi_qr.h"

extern const app_desc_t app_web;

#define TAG "web_reader"
#define WEB_URL_MAX 256
#define WEB_FETCH_MAX (512u * 1024u)
#define WEB_FETCH_TIMEOUT_MS 20000
#define WEB_BODY_PX 48
#define WEB_KEY_BASE 100
#define WEB_KEY_COUNT 40
#define WEB_SPECIAL_BASE 150

typedef enum {
    WEB_HOME,
    WEB_EDIT_URL,
    WEB_LOADING,
    WEB_READING,
    WEB_ERROR,
} web_view_t;

typedef struct {
    char *bytes;
    size_t len;
    size_t cap;
    bool too_large;
} web_download_t;

typedef struct {
    esp_err_t err;
    char url[WEB_URL_MAX];
    html_text_t article;
} web_result_t;

typedef struct {
    char url[WEB_URL_MAX];
} web_fetch_req_t;

typedef struct {
    EpdRect url_field;
    EpdRect open;
    EpdRect home;
    EpdRect back;
    EpdRect prev;
    EpdRect next;
} web_geom_t;

static web_view_t s_view;
static book_store_root_t s_root;
static read_pico_transfer_status_t s_net;
static bool s_net_started;
static bool s_net_start_pending;
static bool s_net_ready;
static bool s_storage_changed;
static int64_t s_net_poll_ms;
static char s_url[WEB_URL_MAX];
static char s_draft[WEB_URL_MAX];
static bool s_uppercase;
static bool s_symbols_more;
static char s_notice[128];
static html_text_t s_article;
static size_t s_page;
static QueueHandle_t s_result_queue;
static volatile bool s_fetch_running;

static const char *const s_url_keys =
    "1234567890" "qwertyuiop" "asdfghjkl-" "zxcvbnm._'";
static const char *const s_special_labels[2][10] = {
    {":", "/", "?", "&", "=", "%", "#", "退格", "清空", "更多"},
    {"+", "@", ";", ",", "~", "_", "-", "退格", "清空", "返回"},
};

static EpdRect key_rect(int id) {
    const int width = (ui_content_width() - 54) / 10;
    return (EpdRect){
        .x = UI_MARGIN + id % 10 * (width + 6),
        .y = 390 + id / 10 * 82,
        .width = width,
        .height = 72,
    };
}

static EpdRect special_rect(int id) {
    const int cols = 10;
    const int width = (ui_content_width() - (cols - 1) * 5) / cols;
    return (EpdRect){
        .x = UI_MARGIN + id * (width + 5),
        .y = 730,
        .width = width,
        .height = 64,
    };
}

static web_geom_t web_geom(void) {
    const int width = ui_content_width();
    const int button_w = (width - UI_GAP) / 2;
    return (web_geom_t){
        .url_field = {UI_MARGIN, 210, width, 112},
        .open = {UI_MARGIN, 890, button_w, 80},
        .home = {UI_MARGIN + button_w + UI_GAP, 890, button_w, 80},
        .back = ui_bar_rect(0, 3),
        .prev = ui_bar_rect(1, 3),
        .next = ui_bar_rect(2, 3),
    };
}

static EpdRect web_body_rect(void) {
    return (EpdRect){UI_MARGIN, UI_CONTENT_TOP, ui_content_width(), UI_BAR_TOP - UI_CONTENT_TOP - 8};
}

static uint64_t web_free_bytes(void *ctx) {
    return book_store_free_bytes(ctx);
}

static esp_err_t web_file_changed(const char *path) {
    return book_store_forget_changed_path(path);
}

static void free_article(void) {
    book_layout_free();
    html_text_free(&s_article);
    s_page = 0;
}

static void free_result(web_result_t *result) {
    if (!result) return;
    html_text_free(&result->article);
    free(result);
}

static bool valid_web_url(const char *url) {
    if (!url || (strncmp(url, "https://", 8) && strncmp(url, "http://", 7))) return false;
    if (strlen(url) > WEB_URL_MAX - 1) return false;
    for (const unsigned char *p = (const unsigned char *)url; *p; ++p)
        if (*p <= 32 || *p == 127) return false;
    const char *host = url + (url[4] == 's' ? 8 : 7);
    const char *end = strpbrk(host, "/?#");
    return (end ? end : host + strlen(host)) > host;
}

static esp_err_t web_http_event(esp_http_client_event_t *event) {
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->data || event->data_len <= 0)
        return ESP_OK;
    web_download_t *download = event->user_data;
    size_t add = (size_t)event->data_len;
    if (!download || download->len > WEB_FETCH_MAX || add > WEB_FETCH_MAX - download->len) {
        if (download) download->too_large = true;
        return ESP_FAIL;
    }
    size_t need = download->len + add + 1;
    if (need > download->cap) {
        size_t cap = download->cap ? download->cap : 8192;
        while (cap < need) cap = cap > WEB_FETCH_MAX / 2 ? WEB_FETCH_MAX + 1 : cap * 2;
        char *next = heap_caps_realloc(download->bytes, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!next) next = realloc(download->bytes, cap);
        if (!next) return ESP_ERR_NO_MEM;
        download->bytes = next;
        download->cap = cap;
    }
    memcpy(download->bytes + download->len, event->data, add);
    download->len += add;
    download->bytes[download->len] = 0;
    return ESP_OK;
}

static esp_err_t fetch_web_page(const char *url, html_text_t *out) {
    web_download_t download = {0};
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = WEB_FETCH_TIMEOUT_MS,
        .buffer_size = 4096,
        .event_handler = web_http_event,
        .user_data = &download,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && (status < 200 || status >= 300)) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && (!download.bytes || !download.len)) err = ESP_ERR_INVALID_RESPONSE;
    if (download.too_large) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) err = html_to_blocks(download.bytes, download.len, out);
    free(download.bytes);
    return err;
}

static void web_fetch_task(void *arg) {
    web_fetch_req_t *request = arg;
    web_result_t *result = heap_caps_calloc(1, sizeof(*result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!result) result = calloc(1, sizeof(*result));
    if (result) {
        snprintf(result->url, sizeof(result->url), "%s", request->url);
        result->err = fetch_web_page(request->url, &result->article);
        xQueueSend(s_result_queue, &result, portMAX_DELAY);
    } else {
        web_result_t *empty = NULL;
        xQueueSend(s_result_queue, &empty, portMAX_DELAY);
    }
    free(request);
    s_fetch_running = false;
    vTaskDelete(NULL);
}

static bool start_fetch(void) {
    if (s_fetch_running || !s_net_ready || !valid_web_url(s_draft)) return false;
    web_fetch_req_t *request = calloc(1, sizeof(*request));
    if (!request) return false;
    snprintf(request->url, sizeof(request->url), "%s", s_draft);
    snprintf(s_url, sizeof(s_url), "%s", s_draft);
    s_fetch_running = true;
    if (xTaskCreatePinnedToCore(web_fetch_task, "web_fetch", 12 * 1024, request, 4, NULL, 1) != pdPASS) {
        s_fetch_running = false;
        free(request);
        return false;
    }
    s_view = WEB_LOADING;
    snprintf(s_notice, sizeof(s_notice), "正在加载网页…");
    return true;
}

static void accept_result(web_result_t *result) {
    free_article();
    if (!result || result->err != ESP_OK) {
        s_view = WEB_ERROR;
        snprintf(s_notice, sizeof(s_notice), "%s", result && result->err == ESP_ERR_INVALID_SIZE
            ? "网页超过 512 KiB" : "网页加载或排版失败");
        free_result(result);
        return;
    }
    s_article = result->article;
    result->article = (html_text_t){0};
    if (!book_layout_begin_blocks(
            s_article.utf8, s_article.len, s_article.blocks, s_article.count,
            web_body_rect(), WEB_BODY_PX)) {
        free_result(result);
        free_article();
        s_view = WEB_ERROR;
        snprintf(s_notice, sizeof(s_notice), "网页正文为空或无法排版");
        return;
    }
    snprintf(s_url, sizeof(s_url), "%s", result->url);
    s_page = 0;
    s_view = WEB_READING;
    snprintf(s_notice, sizeof(s_notice), "网页已加载");
    free_result(result);
}

static void prepare_qr(void) {
    ui_wifi_qr_clear();
    if (s_net.network_ready && s_net.url[0]) ui_wifi_qr_prepare_url(s_net.url);
}

static void network_start(void) {
    if (!ttf_font_ready()) {
        s_view = WEB_ERROR;
        snprintf(s_notice, sizeof(s_notice), "内置字体不可用，无法显示网页阅读页面");
        return;
    }
    char ssid[33];
    bool configured = false;
    if (read_pico_transfer_get_saved_wifi(ssid, &configured) != ESP_OK || !configured) {
        s_view = WEB_ERROR;
        snprintf(s_notice, sizeof(s_notice), "请先在传书页配置并连接已有 WiFi");
        return;
    }
    esp_err_t err = book_store_upload_root(&s_root);
    if (err != ESP_OK) {
        s_view = WEB_ERROR;
        snprintf(s_notice, sizeof(s_notice), "书籍存储不可用");
        return;
    }
    read_pico_transfer_cfg_t cfg = {
        .mode = READ_PICO_TRANSFER_MODE_STA,
        .root_dir = s_root.path,
        .is_flash = s_root.is_flash,
        .file_limit = book_store_file_limit(&s_root),
        .free_bytes_cb = web_free_bytes,
        .free_bytes_ctx = &s_root,
        .file_changed_cb = web_file_changed,
    };
    err = read_pico_transfer_start(&cfg);
    if (err != ESP_OK) {
        s_view = WEB_ERROR;
        snprintf(s_notice, sizeof(s_notice), "WiFi 启动失败：%s", esp_err_to_name(err));
    } else {
        s_net_started = true;
        s_net_start_pending = true;
        s_view = WEB_HOME;
        snprintf(s_notice, sizeof(s_notice), "正在连接已有 WiFi…");
    }
    ESP_LOGI(TAG, "network start: %s", esp_err_to_name(err));
}

static void network_stop(void) {
    if (!s_net_started) return;
    read_pico_transfer_stop();
    read_pico_transfer_get_status(&s_net);
    s_storage_changed = s_storage_changed || s_net.changed_count != 0;
    s_net_started = false;
    s_net_ready = false;
    ui_wifi_qr_clear();
}

static void append_url(const char *value) {
    size_t len = strlen(s_draft), add = strlen(value);
    if (len + add >= sizeof(s_draft)) return;
    memcpy(s_draft + len, value, add + 1);
}

static void url_key(int id) {
    if (id >= WEB_KEY_BASE && id < WEB_KEY_BASE + WEB_KEY_COUNT) {
        char ch = s_url_keys[id - WEB_KEY_BASE];
        if (s_uppercase) ch = (char)toupper((unsigned char)ch);
        char key[2] = {ch, 0};
        append_url(key);
        return;
    }
    if (id >= WEB_SPECIAL_BASE && id < WEB_SPECIAL_BASE + 7) {
        static const char chars[2][7] = {
            {':', '/', '?', '&', '=', '%', '#'},
            {'+', '@', ';', ',', '~', '_', '-'},
        };
        char key[2] = {chars[s_symbols_more ? 1 : 0][id - WEB_SPECIAL_BASE], 0};
        append_url(key);
        return;
    }
    if (id == WEB_SPECIAL_BASE + 7) {
        size_t len = strlen(s_draft);
        if (len) {
            do { --len; } while (len && ((uint8_t)s_draft[len] & 0xc0) == 0x80);
            s_draft[len] = 0;
        }
    } else if (id == WEB_SPECIAL_BASE + 8) {
        s_draft[0] = 0;
    } else if (id == WEB_SPECIAL_BASE + 9) {
        s_symbols_more = !s_symbols_more;
    }
}

static void draw_button(uint8_t *fb, EpdRect rect, const char *label, bool selected) {
    ui_draw_choice_round_rect(fb, rect, UI_BTN_RADIUS, selected);
    ui_text_vc(fb, rect.x + rect.width / 2, rect.y + rect.height / 2,
               UI_PX_LABEL_SM, label, EPD_DRAW_ALIGN_CENTER, selected);
}

static void draw_keyboard(uint8_t *fb) {
    const web_geom_t g = web_geom();
    ui_clear_page(fb);
    ui_draw_header(fb, "网页阅读 Web", "输入网址 · 支持 HTTP / HTTPS");
    ui_draw_round_rect(fb, g.url_field, UI_BTN_RADIUS, UI_GRAY_BLACK);
    const char *tail = s_draft;
    while (*tail && ttf_text_width_px(UI_PX_BODY, tail) > g.url_field.width - 2 * UI_PAD) ++tail;
    ui_text_vc(fb, g.url_field.x + UI_PAD, g.url_field.y + g.url_field.height / 2,
               UI_PX_BODY, tail, EPD_DRAW_ALIGN_LEFT, false);
    for (int i = 0; i < WEB_KEY_COUNT; ++i) {
        char ch = s_url_keys[i];
        if (s_uppercase) ch = (char)toupper((unsigned char)ch);
        char label[2] = {ch, 0};
        draw_button(fb, key_rect(i), label, false);
    }
    for (int i = 0; i < 10; ++i) draw_button(fb, special_rect(i), s_special_labels[s_symbols_more ? 1 : 0][i], false);
    draw_button(fb, ui_row_rect(0, 2, 830, 72), "https://", false);
    draw_button(fb, ui_row_rect(1, 2, 830, 72), s_uppercase ? "abc" : "ABC", s_uppercase);
    ui_text(fb, UI_MARGIN, 930, UI_PX_CAPTION,
            s_notice[0] ? s_notice : "需先连接已有 WiFi · 网页最多 512 KiB · KEY3 菜单",
            EPD_DRAW_ALIGN_LEFT, false);
    draw_button(fb, ui_bar_rect(0, 2), "取消", false);
    draw_button(fb, ui_bar_rect(1, 2), "打开", false);
    ui_draw_menu_handle(fb, false);
}

static void draw_home(uint8_t *fb) {
    const web_geom_t g = web_geom();
    ui_clear_page(fb);
    ui_draw_header(fb, "网页阅读 Web", s_net_ready ? s_net.url : s_notice);
    ui_draw_section(fb, UI_CONTENT_TOP, "网址 Address");
    EpdRect field = {UI_MARGIN, UI_CONTENT_TOP + UI_SEC_HEAD, ui_content_width(), 86};
    ui_draw_round_rect(fb, field, UI_BTN_RADIUS, UI_GRAY_BLACK);
    ui_text_vc(fb, field.x + UI_PAD, field.y + field.height / 2, UI_PX_LABEL_SM,
               s_url[0] ? s_url : "点此输入网址", EPD_DRAW_ALIGN_LEFT, false);
    draw_button(fb, g.open, "输入网址 / 打开", false);
    draw_button(fb, g.home, "重新连接", false);
    if (s_net_ready) {
        ui_draw_section(fb, 540, "浏览器剪藏 Browser clip");
        if (s_net.url[0]) {
            ui_wifi_qr_draw(fb, (EpdRect){UI_MARGIN + (ui_content_width() - 240) / 2, 650, 240, 240});
            ui_text(fb, UI_MARGIN, 914, UI_PX_CAPTION,
                    "扫码后按网页说明添加书签；投递时设备需保持唤醒。",
                    EPD_DRAW_ALIGN_LEFT, false);
        }
        if (s_view == WEB_LOADING || s_view == WEB_ERROR)
            ui_text(fb, UI_MARGIN, 970, UI_PX_CAPTION, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    } else {
        ui_text(fb, UI_MARGIN, 600, UI_PX_BODY, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    }
    ui_draw_menu_handle(fb, false);
}

static void draw_article(uint8_t *fb) {
    ui_clear_page(fb);
    const char *url_tail = s_url;
    while (*url_tail && ttf_text_width_px(UI_PX_SUB, url_tail) > ui_content_width()) ++url_tail;
    ui_draw_header(fb, "网页阅读 Web", url_tail);
    book_layout_draw_page(fb, s_page, web_body_rect(), WEB_BODY_PX);
    draw_button(fb, web_geom().back, "网址", false);
    draw_button(fb, web_geom().prev, "上一页", false);
    draw_button(fb, web_geom().next, "下一页", false);
    ui_draw_menu_handle(fb, false);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    if (s_view == WEB_EDIT_URL) draw_keyboard(fb);
    else if (s_view == WEB_READING) draw_article(fb);
    else draw_home(fb);
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    if (!s_result_queue) s_result_queue = xQueueCreate(1, sizeof(web_result_t *));
    s_storage_changed = false;
    (void)ttf_font_suspend_sd(true);
    display_set_bulk_io(true);
    s_net_started = false;
    s_net_start_pending = false;
    s_net_ready = false;
    memset(&s_net, 0, sizeof(s_net));
    free_article();
    if (!s_result_queue) {
        s_view = WEB_ERROR;
        snprintf(s_notice, sizeof(s_notice), "内存不足");
        return;
    }
    network_start();
}

static void on_exit(app_ctx_t *ctx) {
    (void)ctx;
    while (s_fetch_running) vTaskDelay(pdMS_TO_TICKS(20));
    web_result_t *result = NULL;
    while (s_result_queue && xQueueReceive(s_result_queue, &result, 0) == pdTRUE) free_result(result);
    network_stop();
    free_article();
    if (s_storage_changed) book_store_notify_changed();
    if (s_result_queue) { vQueueDelete(s_result_queue); s_result_queue = NULL; }
    display_set_bulk_io(false);
    (void)ttf_font_suspend_sd(false);
}

static void on_media_lost(app_ctx_t *ctx) {
    (void)ctx;
    if (s_root.is_flash) return;
    network_stop();
    memset(&s_root, 0, sizeof(s_root));
    s_view = WEB_ERROR;
    snprintf(s_notice, sizeof(s_notice), "TF 卡已移除，网页剪藏服务已停止");
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    web_result_t *result = NULL;
    if (s_result_queue && xQueueReceive(s_result_queue, &result, 0) == pdTRUE) {
        accept_result(result);
        return APP_REDRAW_FULL;
    }
    if (!s_net_started || ctx->now_ms - s_net_poll_ms < 500) return APP_REDRAW_NONE;
    s_net_poll_ms = ctx->now_ms;
    read_pico_transfer_service_poll();
    read_pico_transfer_status_t next = {0};
    read_pico_transfer_get_status(&next);
    bool changed = next.network_ready != s_net.network_ready || strcmp(next.url, s_net.url)
        || next.state != s_net.state || next.last_error != s_net.last_error;
    s_net = next;
    if (changed) prepare_qr();
    if (s_net.network_ready) {
        s_net_ready = true;
        if (s_net_start_pending) {
            s_net_start_pending = false;
            snprintf(s_notice, sizeof(s_notice), "WiFi 已连接，可输入网址或从浏览器剪藏");
            return APP_REDRAW_PAGE;
        }
    } else if (s_net_ready) {
        s_net_ready = false;
        snprintf(s_notice, sizeof(s_notice), "WiFi 连接中断，正在重试…");
        return APP_REDRAW_PAGE;
    } else if (s_net.state == READ_PICO_TRANSFER_ERROR) {
        s_net_start_pending = false;
        snprintf(s_notice, sizeof(s_notice), "WiFi 连接失败；点重新连接再试");
        return APP_REDRAW_PAGE;
    }
    if (changed) return APP_REDRAW_PAGE;
    return APP_REDRAW_NONE;
}

static void open_address_editor(void) {
    snprintf(s_draft, sizeof(s_draft), "%s", s_url);
    if (!s_draft[0]) snprintf(s_draft, sizeof(s_draft), "https://");
    s_notice[0] = 0;
    s_view = WEB_EDIT_URL;
}

static void home_touch(uint16_t x, uint16_t y) {
    const web_geom_t g = web_geom();
    EpdRect field = {UI_MARGIN, UI_CONTENT_TOP + UI_SEC_HEAD, ui_content_width(), 86};
    if (ui_rect_hit(field, x, y) || ui_rect_hit(g.open, x, y)) {
        open_address_editor();
    } else if (ui_rect_hit(g.home, x, y)) {
        network_stop();
        network_start();
    }
}

static void keyboard_touch(uint16_t x, uint16_t y) {
    for (int i = 0; i < WEB_KEY_COUNT; ++i) {
        if (ui_rect_hit(key_rect(i), x, y)) { url_key(WEB_KEY_BASE + i); return; }
    }
    for (int i = 0; i < 10; ++i) {
        if (ui_rect_hit(special_rect(i), x, y)) { url_key(s_special_ids[i]); return; }
    }
    if (ui_rect_hit(ui_row_rect(0, 2, 830, 72), x, y)) append_url("https://");
    else if (ui_rect_hit(ui_row_rect(1, 2, 830, 72), x, y)) s_uppercase = !s_uppercase;
    else if (ui_rect_hit(ui_bar_rect(0, 2), x, y)) s_view = WEB_HOME;
    else if (ui_rect_hit(ui_bar_rect(1, 2), x, y)) {
        if (!s_net_ready) snprintf(s_notice, sizeof(s_notice), "WiFi 尚未连接");
        else if (!valid_web_url(s_draft)) snprintf(s_notice, sizeof(s_notice), "请输入有效的 HTTP 或 HTTPS 网址");
        else if (!start_fetch()) snprintf(s_notice, sizeof(s_notice), "无法开始加载网页");
    }
}

static void article_touch(uint16_t x, uint16_t y) {
    const web_geom_t g = web_geom();
    if (ui_rect_hit(g.back, x, y)) { s_view = WEB_HOME; return; }
    if (ui_rect_hit(g.prev, x, y) && s_page > 0) { --s_page; return; }
    if (ui_rect_hit(g.next, x, y)) {
        if (s_page + 1 >= book_layout_page_count() && !book_layout_complete())
            book_layout_extend(2);
        if (s_page + 1 < book_layout_page_count()) ++s_page;
        return;
    }
    if (x < epd_rotated_display_width() / 3 && s_page > 0) --s_page;
    else if (x > epd_rotated_display_width() * 2 / 3) {
        if (s_page + 1 >= book_layout_page_count() && !book_layout_complete())
            book_layout_extend(2);
        if (s_page + 1 < book_layout_page_count()) ++s_page;
    }
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    (void)ctx;
    if (event->type == UI_GESTURE_TAP) {
        if (s_view == WEB_EDIT_URL) keyboard_touch(event->x, event->y);
        else if (s_view == WEB_READING) article_touch(event->x, event->y);
        else home_touch(event->x, event->y);
        return APP_REDRAW_PAGE;
    }
    if (s_view == WEB_READING && event->type == UI_GESTURE_SWIPE_L) {
        article_touch(epd_rotated_display_width() - 1, UI_LOCK_HEIGHT / 2);
        return APP_REDRAW_PAGE;
    }
    if (s_view == WEB_READING && event->type == UI_GESTURE_SWIPE_R) {
        if (s_page > 0) --s_page;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

const app_desc_t app_web = {
    .title = "网页阅读 Web",
    .detail = "在线网页与浏览器剪藏",
    .render = render,
    .on_enter = on_enter,
    .on_exit = on_exit,
    .on_media_lost = on_media_lost,
    .on_gesture = on_gesture,
    .on_tick = on_tick,
};
