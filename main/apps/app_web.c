/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：联网抓取网页并以墨水屏阅读布局显示；本页同时开放同网剪藏入口。
 * English: Fetch web pages for e-paper reading and expose the local browser-clip endpoint.
 *
 * 冻结：只提取 HTML 正文，不执行脚本或 CSS；Wi-Fi 使用已保存的 STA 配置，离页停服。
 * 剪藏只允许同网设备持本次开机口令写入 TXT 书架。
 * 决策变更：联网等待期间阻止空闲睡眠；离页或电源键锁屏会取消请求并停网。
 * Decision change: Block idle sleep while fetching or awaiting clips; exit or power-key lock cancels requests and stops networking.
 * Frozen: Extract HTML text without executing scripts or CSS; use saved STA credentials and stop networking on exit.
 * Browser clips require the current boot token and are stored as TXT books.
 */

#include "app.h"
#include "ble_service.h"
#include "time_sync.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "app_web.h"
#include "book_layout.h"
#include "book_progress.h"
#include "book_store.h"
#include "display.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "html_text.h"
#include "nvs.h"
#include "read_pico_transfer.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_wifi_qr.h"
#include "web_bookmarks.h"

extern const app_desc_t app_web;

#define TAG "web_reader"
#define WEB_URL_MAX 256
#define WEB_FETCH_MAX (512u * 1024u)
#define WEB_FETCH_TIMEOUT_MS 20000
#define WEB_FETCH_IO_TIMEOUT_MS 2000
#define WEB_FETCH_MAX_REDIRECTS 5
#define WEB_BODY_PX 48
#define WEB_KEY_BASE 100
#define WEB_KEY_COUNT 40
#define WEB_SPECIAL_BASE 150

typedef enum {
    WEB_HOME,
    WEB_EDIT_URL,
    WEB_LOADING,
    WEB_READING,
    WEB_BOOKMARKS,
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
    bool truncated;
} web_result_t;

typedef struct {
    char url[WEB_URL_MAX];
} web_fetch_req_t;

typedef struct {
    EpdRect url_field;
    EpdRect open;
    EpdRect home;
    EpdRect bookmark;
    EpdRect save;
} web_geom_t;


static web_view_t s_view;
static book_store_root_t s_root;
static read_pico_transfer_status_t s_net;
static bool s_net_started;
static bool s_start_pending;
static bool s_net_start_pending;
static bool s_net_ready;
static bool s_storage_changed;
static int64_t s_net_poll_ms;
static char s_url[WEB_URL_MAX];
static char s_draft[WEB_URL_MAX];
static char s_queued_url[WEB_URL_MAX];
static bool s_uppercase;
static bool s_symbols_more;
static char s_notice[128];
static html_text_t s_article;
static size_t s_page;
static QueueHandle_t s_result_queue;
static volatile bool s_fetch_running;
static volatile bool s_fetch_cancel;
static TaskHandle_t s_fetch_task;
static bool s_article_truncated;
// 收藏表约 6.4 KB，进页时放 PSRAM、离页释放，避免常驻内部 DRAM 挤压 Wi-Fi。
// Keep the ~6.4 KB bookmark table in PSRAM while active so internal DRAM stays free for Wi-Fi.
static web_bookmark_t *s_bookmarks;
static size_t s_bookmark_count;
static size_t s_bookmark_page;

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
    if (id < 7) {
        const int cols = 7;
        const int width = (ui_content_width() - (cols - 1) * 6) / cols;
        return (EpdRect){UI_MARGIN + id * (width + 6), 730, width, 64};
    }
    const int cols = 3;
    const int local = id - 7;
    const int width = (ui_content_width() - (cols - 1) * 12) / cols;
    return (EpdRect){UI_MARGIN + local * (width + 12), 810, width, 72};
}

static web_geom_t web_geom(void) {
    const int width = ui_content_width();
    const int button_w = (width - UI_GAP) / 2;
    return (web_geom_t){
        .url_field = {UI_MARGIN, 210, width, 112},
        .open = {UI_MARGIN, 330, button_w, 80},
        .home = {UI_MARGIN + button_w + UI_GAP, 330, button_w, 80},
        .bookmark = ui_bar_rect(0, 2),
        .save = ui_bar_rect(1, 2),
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

static void load_bookmarks(void) {
    if (!s_bookmarks) { s_bookmark_count = 0; return; }
    web_bookmarks_load(s_bookmarks, &s_bookmark_count);
}

static void save_bookmarks(void) {
    if (!s_bookmarks) return;
    (void)web_bookmarks_save(s_bookmarks, s_bookmark_count);
}

static bool bookmark_index(const char *url, size_t *index) {
    if (!s_bookmarks) { if (index) *index = WEB_BOOKMARK_MAX; return false; }
    *index = web_bookmark_find(s_bookmarks, s_bookmark_count, url);
    return *index != WEB_BOOKMARK_MAX;
}

static void toggle_bookmark(void) {
    if (!s_bookmarks) return;
    size_t index;
    if (bookmark_index(s_url, &index)) {
        (void)web_bookmark_remove(s_bookmarks, &s_bookmark_count, s_url);
        snprintf(s_notice, sizeof(s_notice), "已从收藏夹移除");
    } else if (s_bookmark_count < WEB_BOOKMARK_MAX) {
        char title[WEB_BOOKMARK_TITLE_MAX];
        const char *host = s_url + (strncmp(s_url, "https://", 8) == 0 ? 8 : 7);
        size_t n = strcspn(host, "/?#:");
        if (n >= sizeof(title)) n = sizeof(title) - 1;
        memcpy(title, host, n); title[n] = 0;
        if (web_bookmark_add(s_bookmarks, &s_bookmark_count, title, s_url)) snprintf(s_notice, sizeof(s_notice), "已加入收藏夹");
    } else snprintf(s_notice, sizeof(s_notice), "收藏夹最多保存 20 条网址");
    save_bookmarks();
}

static bool save_article_to_shelf(void) {
    if (!s_article.utf8 || !s_article.len || !s_root.path[0]) return false;
    char name[96], path[384];
    size_t out = 0;
    const char *title = s_article.count ? s_article.utf8 + s_article.blocks[0].offset : "";
    size_t title_len = s_article.count ? s_article.blocks[0].len : 0;
    for (size_t i = 0; i < title_len && out < 48;) {
        unsigned char c = (unsigned char)title[i];
        size_t width = c < 0x80 ? 1 : (c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4);
        if (i + width > title_len || out + width > 48) break;
        if (width == 1) name[out++] = (isalnum(c) || c == '-' || c == '_') ? (char)c : '_';
        else { memcpy(name + out, title + i, width); out += width; }
        i += width;
    }
    if (!out) {
        const char *host = s_url + (strncmp(s_url, "https://", 8) == 0 ? 8 : 7);
        size_t n = strcspn(host, "/?#:"); if (n > 48) n = 48;
        for (size_t i = 0; i < n; ++i) { unsigned char c = (unsigned char)host[i]; name[out++] = (c < 128 && (isalnum(c) || c == '-' || c == '_')) ? (char)c : '_'; }
        if (!out) { memcpy(name, "webpage", 7); out = 7; }
    }
    name[out] = 0;
    for (unsigned suffix = 0; suffix < 100; ++suffix) {
        if (suffix) snprintf(path, sizeof(path), "%s/%s-%u.txt", s_root.path, name, suffix + 1);
        else snprintf(path, sizeof(path), "%s/%s.txt", s_root.path, name);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) { if (errno == EEXIST) continue; return false; }
        size_t done = 0;
        while (done < s_article.len) {
            ssize_t wrote = write(fd, s_article.utf8 + done, s_article.len - done);
            if (wrote <= 0) { close(fd); unlink(path); return false; }
            done += (size_t)wrote;
        }
        close(fd);
        book_store_notify_changed();
        s_storage_changed = true;
        snprintf(s_notice, sizeof(s_notice), "已存到书架：%s", name);
        return true;
    }
    return false;
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

static esp_err_t append_download(web_download_t *download, const char *data, size_t add) {
    if (!download || !data || !add) return ESP_OK;
    if (download->len > WEB_FETCH_MAX || add > WEB_FETCH_MAX - download->len) {
        download->too_large = true;
        add = WEB_FETCH_MAX - download->len;
    }
    if (!add) return ESP_OK;
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
    memcpy(download->bytes + download->len, data, add);
    download->len += add;
    download->bytes[download->len] = 0;
    return ESP_OK;
}

static bool is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static esp_err_t fetch_web_page(const char *url, html_text_t *out, bool *truncated) {
    web_download_t download = {0};
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = WEB_FETCH_IO_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    int64_t deadline = esp_timer_get_time() + (int64_t)WEB_FETCH_TIMEOUT_MS * 1000;
    esp_err_t err = ESP_OK;
    int status = 0;
    int redirects = 0;
    while (!s_fetch_cancel && esp_timer_get_time() < deadline) {
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) break;
        int64_t hdr = esp_http_client_fetch_headers(client);
        while (hdr == -ESP_ERR_HTTP_EAGAIN && !s_fetch_cancel && esp_timer_get_time() < deadline) {
            hdr = esp_http_client_fetch_headers(client);
        }
        if (hdr < 0) {
            err = hdr == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_TIMEOUT : ESP_FAIL;
            break;
        }
        status = esp_http_client_get_status_code(client);
        if (!is_redirect_status(status)) break;
        if (redirects >= WEB_FETCH_MAX_REDIRECTS) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        int flushed = 0;
        (void)esp_http_client_flush_response(client, &flushed);
        err = esp_http_client_set_redirection(client);
        esp_http_client_close(client);
        if (err != ESP_OK) break;
        ++redirects;
    }
    char chunk[4096];
    while (err == ESP_OK && !s_fetch_cancel && !download.too_large
           && esp_timer_get_time() < deadline) {
        int n = esp_http_client_read(client, chunk, sizeof(chunk));
        if (n == 0) break;
        if (n == -ESP_ERR_HTTP_EAGAIN) continue;
        if (n < 0) { err = ESP_FAIL; break; }
        err = append_download(&download, chunk, (size_t)n);
    }
    if (s_fetch_cancel) err = ESP_ERR_INVALID_STATE;
    else if (err == ESP_OK && esp_timer_get_time() >= deadline) err = ESP_ERR_TIMEOUT;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && (status < 200 || status >= 300)) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && (!download.bytes || !download.len)) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) err = html_to_web_blocks(download.bytes, download.len, out);
    if (truncated) *truncated = download.too_large;
    free(download.bytes);
    return err;
}

static void web_fetch_task(void *arg) {
    web_fetch_req_t *request = arg;
    web_result_t *result = heap_caps_calloc(1, sizeof(*result), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!result) result = calloc(1, sizeof(*result));
    if (result) {
        snprintf(result->url, sizeof(result->url), "%s", request->url);
        result->err = fetch_web_page(request->url, &result->article, &result->truncated);
        if (!s_fetch_cancel && xQueueSend(s_result_queue, &result, 0) == pdTRUE) result = NULL;
    } else {
        web_result_t *empty = NULL;
        if (!s_fetch_cancel) (void)xQueueSend(s_result_queue, &empty, 0);
    }
    free_result(result);
    free(request);
    s_fetch_running = false;
    s_fetch_task = NULL;
    vTaskDelete(NULL);
}

static bool start_fetch(void) {
    if (s_fetch_running || !s_net_ready || !valid_web_url(s_draft)) return false;
    web_fetch_req_t *request = calloc(1, sizeof(*request));
    if (!request) return false;
    snprintf(request->url, sizeof(request->url), "%s", s_draft);
    snprintf(s_url, sizeof(s_url), "%s", s_draft);
    s_fetch_running = true;
    s_fetch_cancel = false;
    if (xTaskCreatePinnedToCore(web_fetch_task, "web_fetch", 12 * 1024, request, 4, &s_fetch_task, 1) != pdPASS) {
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
    s_article_truncated = false;
    if (!result || result->err != ESP_OK) {
        s_view = WEB_ERROR;
        snprintf(s_notice, sizeof(s_notice), "%s", result && result->err == ESP_ERR_INVALID_SIZE
            ? "网页超过 512 KiB" : "网页加载或排版失败");
        free_result(result);
        return;
    }
    s_article = result->article;
    s_article_truncated = result->truncated;
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
    snprintf(s_notice, sizeof(s_notice), "%s", s_article_truncated
        ? "网页较大，已截断前 512 KiB" : "网页已加载");
    free_result(result);
}

static void prepare_qr(void) {
    ui_wifi_qr_clear();
    if (s_net.network_ready && s_net.url[0]) ui_wifi_qr_prepare_url(s_net.url);
}

static void network_start(void) {
    s_net_start_pending = false;
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
        .sta_ip_cb = app_time_sync_sta_ip,
        .service_poll_cb = app_time_sync_poll,
        .network_stop_cb = app_time_sync_stop,
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
    s_start_pending = false;
    s_net_start_pending = false;
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
    ui_draw_header(fb, "网页阅读", "输入网址 · 支持 HTTP / HTTPS");
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
    draw_button(fb, ui_row_rect(0, 2, 900, 72), "https://", false);
    draw_button(fb, ui_row_rect(1, 2, 900, 72), s_uppercase ? "abc" : "ABC", s_uppercase);
    ui_text(fb, UI_MARGIN, 990, UI_PX_CAPTION,
            s_notice[0] ? s_notice : "需先连接已有 WiFi · 网页最多 512 KiB",
            EPD_DRAW_ALIGN_LEFT, false);
    draw_button(fb, ui_bar_rect(0, 1), "打开", false);
}

static void draw_home(uint8_t *fb) {
    const web_geom_t g = web_geom();
    ui_clear_page(fb);
    ui_draw_header(fb, "网页阅读", s_net_ready ? s_net.url : s_notice);
    ui_draw_section(fb, UI_CONTENT_TOP, "网址");
    EpdRect field = {UI_MARGIN, UI_CONTENT_TOP + UI_SEC_HEAD, ui_content_width(), 86};
    ui_draw_round_rect(fb, field, UI_BTN_RADIUS, UI_GRAY_BLACK);
    ui_text_vc(fb, field.x + UI_PAD, field.y + field.height / 2, UI_PX_LABEL_SM,
               s_url[0] ? s_url : "点此输入网址", EPD_DRAW_ALIGN_LEFT, false);
    draw_button(fb, g.open, "输入网址 / 打开", false);
    draw_button(fb, g.home, "收藏夹", false);
    if (s_net_ready) {
        ui_draw_section(fb, 540, "浏览器剪藏");
        if (s_net.url[0]) {
            ui_wifi_qr_draw(fb, (EpdRect){UI_MARGIN + (ui_content_width() - 240) / 2, 650, 240, 240});
            char local_url[96];
            snprintf(local_url, sizeof(local_url), "网页：http://%s.local", s_net.mdns_host[0] ? s_net.mdns_host : "readpico");
            ui_text(fb, UI_MARGIN, 914, UI_PX_CAPTION, local_url, EPD_DRAW_ALIGN_LEFT, false);
            ui_text(fb, UI_MARGIN, 946, UI_PX_CAPTION, s_net.url, EPD_DRAW_ALIGN_LEFT, false);
        }
        if (s_view == WEB_LOADING || s_view == WEB_ERROR)
            ui_text(fb, UI_MARGIN, 970, UI_PX_CAPTION, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    } else {
        ui_text(fb, UI_MARGIN, 600, UI_PX_BODY, s_notice, EPD_DRAW_ALIGN_LEFT, false);
        if (!s_net_start_pending) draw_button(fb, ui_bar_rect(0, 1), "重新连接", false);
    }
}

static void draw_article(uint8_t *fb) {
    ui_clear_page(fb);
    char page[128];
    if (s_notice[0]) {
        snprintf(page, sizeof(page), "%s第 %u / %u 页 · %.64s",
                 s_article_truncated ? "已截断 · " : "",
                 (unsigned)s_page + 1, (unsigned)book_layout_page_count(), s_notice);
    } else {
        snprintf(page, sizeof(page), "%s第 %u / %u 页",
                 s_article_truncated ? "已截断 · " : "",
                 (unsigned)s_page + 1, (unsigned)book_layout_page_count());
    }
    ui_draw_header(fb, "网页阅读", page);
    book_layout_draw_page(fb, s_page, web_body_rect(), WEB_BODY_PX);
    size_t index = 0;
    draw_button(fb, web_geom().bookmark, bookmark_index(s_url, &index) ? "取消收藏" : "收藏", false);
    draw_button(fb, web_geom().save, "存到书架", false);
}

static void draw_bookmarks(uint8_t *fb) {
    ui_clear_page(fb);
    size_t pages = s_bookmark_count ? (s_bookmark_count + 5) / 6 : 1;
    char sub[80];
    if (pages > 1) {
        snprintf(sub, sizeof(sub), "点按网址直接加载 · 第 %u/%u 页",
                 (unsigned)s_bookmark_page + 1, (unsigned)pages);
    } else {
        snprintf(sub, sizeof(sub), "点按网址直接加载");
    }
    ui_draw_header(fb, "收藏夹", sub);
    if (!s_bookmark_count) {
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 48, UI_PX_BODY, "还没有收藏网页", EPD_DRAW_ALIGN_LEFT, false);
    }
    size_t first = s_bookmark_page * 6;
    for (size_t row = 0; s_bookmarks && row < 6 && first + row < s_bookmark_count; ++row) {
        web_bookmark_t *item = &s_bookmarks[first + row];
        EpdRect rect = ui_grid_rect(0, 1, (int)row, UI_CONTENT_TOP, 104);
        ui_draw_round_rect(fb, rect, UI_BTN_RADIUS, UI_GRAY_BLACK);
        ui_text(fb, rect.x + UI_PAD, rect.y + 8, UI_PX_BTN, item->title, EPD_DRAW_ALIGN_LEFT, false);
        ui_text(fb, rect.x + UI_PAD, rect.y + rect.height - 24, UI_PX_CAPTION, item->url, EPD_DRAW_ALIGN_LEFT, false);
    }
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    if (s_view == WEB_EDIT_URL) draw_keyboard(fb);
    else if (s_view == WEB_READING) draw_article(fb);
    else if (s_view == WEB_BOOKMARKS) draw_bookmarks(fb);
    else draw_home(fb);
}

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    // 先关 BLE 再联网。/ Stop BLE before networking.
    app_ble_wifi_hold(true);
    if (!s_result_queue) s_result_queue = xQueueCreate(1, sizeof(web_result_t *));
    if (!s_bookmarks) {
        s_bookmarks = heap_caps_calloc(WEB_BOOKMARK_MAX, sizeof(*s_bookmarks), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_bookmarks) s_bookmarks = calloc(WEB_BOOKMARK_MAX, sizeof(*s_bookmarks));
    }
    s_storage_changed = false;
    load_bookmarks();
    (void)ttf_font_suspend_sd(true);
    display_set_bulk_io(true);
    s_net_started = false;
    s_start_pending = false;
    s_net_start_pending = false;
    s_net_ready = false;
    s_net_poll_ms = 0;
    s_fetch_cancel = false;
    s_article_truncated = false;
    memset(&s_net, 0, sizeof(s_net));
    free_article();
    if (!s_result_queue) {
        s_view = WEB_ERROR;
        snprintf(s_notice, sizeof(s_notice), "内存不足");
        return;
    }
    // 首帧先绘制界面，再在下一拍 on_tick 启动 Wi-Fi，避免切页卡顿。
    // Render the page first, then start Wi-Fi on the next tick so entering the app stays instant.
    s_view = WEB_HOME;
    s_start_pending = true;
    s_net_start_pending = true;
    snprintf(s_notice, sizeof(s_notice), "正在连接已有 WiFi…");
}

static void web_on_exit(app_ctx_t *ctx) {
    (void)ctx;
    s_fetch_cancel = true;
    while (s_fetch_running) vTaskDelay(pdMS_TO_TICKS(20));
    web_result_t *result = NULL;
    while (s_result_queue && xQueueReceive(s_result_queue, &result, 0) == pdTRUE) free_result(result);
    network_stop();
    free_article();
    free(s_bookmarks);
    s_bookmarks = NULL;
    s_bookmark_count = 0;
    app_ble_wifi_hold(false);
    if (s_storage_changed) book_store_notify_changed();
    if (s_result_queue) { vQueueDelete(s_result_queue); s_result_queue = NULL; }
    display_set_bulk_io(false);
    (void)ttf_font_suspend_sd(false);
}

static void on_media_lost(app_ctx_t *ctx) {
    (void)ctx;
    if (s_root.is_flash) return;
    s_fetch_cancel = true;
    while (s_fetch_running) vTaskDelay(pdMS_TO_TICKS(20));
    network_stop();
    memset(&s_root, 0, sizeof(s_root));
    s_view = WEB_ERROR;
    snprintf(s_notice, sizeof(s_notice), "TF 卡已移除，网页剪藏服务已停止");
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    if (s_start_pending && !ctx->consumed) {
        s_start_pending = false;
        network_start();
        return APP_REDRAW_PAGE;
    }
    web_result_t *result = NULL;
    if (s_result_queue && xQueueReceive(s_result_queue, &result, 0) == pdTRUE) {
        accept_result(result);
        return APP_REDRAW_FULL;
    }
    if (s_net_ready && s_view != WEB_LOADING && !s_fetch_running) {
        char pushed[READ_PICO_WEBOPEN_URL_MAX];
        if (s_queued_url[0]) {
            snprintf(s_draft, sizeof(s_draft), "%s", s_queued_url);
            s_queued_url[0] = '\0';
            if (valid_web_url(s_draft) && start_fetch()) return APP_REDRAW_PAGE;
        } else if (read_pico_transfer_take_webopen_url(pushed)) {
            snprintf(s_draft, sizeof(s_draft), "%s", pushed);
            if (valid_web_url(s_draft) && start_fetch()) return APP_REDRAW_PAGE;
        }
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

static bool home_touch(uint16_t x, uint16_t y) {
    if (s_view == WEB_LOADING) return false;
    const web_geom_t g = web_geom();
    EpdRect field = {UI_MARGIN, UI_CONTENT_TOP + UI_SEC_HEAD, ui_content_width(), 86};
    if (ui_rect_hit(field, x, y) || ui_rect_hit(g.open, x, y)) {
        open_address_editor();
        return true;
    } else if (ui_rect_hit(g.home, x, y)) {
        s_bookmark_page = 0;
        s_view = WEB_BOOKMARKS;
        return true;
    } else if (!s_net_ready && !s_net_start_pending && ui_rect_hit(ui_bar_rect(0, 1), x, y)) {
        network_stop();
        network_start();
        return true;
    }
    return false;
}

static bool keyboard_touch(uint16_t x, uint16_t y) {
    char old_draft[sizeof(s_draft)];
    snprintf(old_draft, sizeof(old_draft), "%s", s_draft);
    bool old_uppercase = s_uppercase;
    bool old_symbols = s_symbols_more;
    web_view_t old_view = s_view;
    char old_notice[sizeof(s_notice)];
    snprintf(old_notice, sizeof(old_notice), "%s", s_notice);
    for (int i = 0; i < WEB_KEY_COUNT; ++i) {
        if (ui_rect_hit(key_rect(i), x, y)) { url_key(WEB_KEY_BASE + i); goto done; }
    }
    for (int i = 0; i < 10; ++i) {
        if (ui_rect_hit(special_rect(i), x, y)) { url_key(WEB_SPECIAL_BASE + i); goto done; }
    }
    if (ui_rect_hit(ui_row_rect(0, 2, 900, 72), x, y)) append_url("https://");
    else if (ui_rect_hit(ui_row_rect(1, 2, 900, 72), x, y)) s_uppercase = !s_uppercase;
    else if (ui_rect_hit(ui_bar_rect(0, 1), x, y)) {
        if (!s_net_ready) snprintf(s_notice, sizeof(s_notice), "WiFi 尚未连接");
        else if (!valid_web_url(s_draft)) snprintf(s_notice, sizeof(s_notice), "请输入有效的 HTTP 或 HTTPS 网址");
        else if (!start_fetch()) snprintf(s_notice, sizeof(s_notice), "无法开始加载网页");
    }
done:
    return old_view != s_view || old_uppercase != s_uppercase || old_symbols != s_symbols_more
        || strcmp(old_draft, s_draft) != 0 || strcmp(old_notice, s_notice) != 0;
}

static bool article_turn(int dir) {
    if (dir < 0) {
        if (s_page == 0) return false;
        --s_page;
        s_notice[0] = '\0';
        return true;
    }
    if (s_page + 1 >= book_layout_page_count() && !book_layout_complete()
        && !book_layout_extend(2)) {
        s_view = WEB_ERROR;
        snprintf(s_notice, sizeof(s_notice), "网页正文排版失败，请重新加载");
        return true;
    }
    if (s_page + 1 < book_layout_page_count()) {
        ++s_page;
        s_notice[0] = '\0';
        return true;
    }
    return false;
}

static bool article_touch(uint16_t x, uint16_t y) {
    const web_geom_t g = web_geom();
    if (ui_rect_hit(g.bookmark, x, y)) { toggle_bookmark(); return true; }
    if (ui_rect_hit(g.save, x, y)) {
        if (!save_article_to_shelf()) snprintf(s_notice, sizeof(s_notice), "保存失败：书架空间不足或文件无法创建");
        return true;
    }
    if (x < epd_rotated_display_width() / 3) return article_turn(-1);
    if (x > epd_rotated_display_width() * 2 / 3) return article_turn(1);
    return false;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *event) {
    (void)ctx;
    if (event->type == UI_GESTURE_TAP) {
        web_view_t before_view = s_view;
        bool changed = false;
        if (s_view == WEB_EDIT_URL) changed = keyboard_touch(event->x, event->y);
        else if (s_view == WEB_READING) changed = article_touch(event->x, event->y);
        else if (s_view == WEB_BOOKMARKS) {
            size_t first = s_bookmark_page * 6;
            for (size_t row = 0; s_bookmarks && row < 6 && first + row < s_bookmark_count; ++row) {
                EpdRect rect = ui_grid_rect(0, 1, (int)row, UI_CONTENT_TOP, 104);
                if (!ui_rect_hit(rect, event->x, event->y)) continue;
                snprintf(s_draft, sizeof(s_draft), "%.*s",
                         (int)(sizeof(s_draft) - 1), s_bookmarks[first + row].url);
                if (!start_fetch()) snprintf(s_notice, sizeof(s_notice), "无法开始加载网页");
                changed = true; break;
            }
        }
        else changed = home_touch(event->x, event->y);
        if (!changed) return APP_REDRAW_NONE;
        if (before_view == WEB_EDIT_URL && s_view == WEB_EDIT_URL
            && event->x >= UI_MARGIN && event->y >= 200 && event->y < 1040) {
            render(ctx, ctx->fb);
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_PAGE;
    }
    if (s_view == WEB_READING && event->type == UI_GESTURE_SWIPE_L) {
        return article_turn(1) ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    if (s_view == WEB_READING && event->type == UI_GESTURE_SWIPE_R) {
        return article_turn(-1) ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }
    if (s_view == WEB_BOOKMARKS && event->type == UI_GESTURE_SWIPE_L &&
        (s_bookmark_page + 1) * 6 < s_bookmark_count) {
        ++s_bookmark_page;
        return APP_REDRAW_PAGE;
    }
    if (s_view == WEB_BOOKMARKS && event->type == UI_GESTURE_SWIPE_R && s_bookmark_page > 0) {
        --s_bookmark_page;
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (s_view == WEB_READING) {
        if (key == UI_KEY_1) {
            ctx->key_handled = true;
            if (s_page > 0) return article_turn(-1) ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
            s_view = WEB_HOME;
            return APP_REDRAW_PAGE;
        }
        if (key == UI_KEY_3) {
            ctx->key_handled = true;
            return article_turn(1) ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == WEB_BOOKMARKS) {
        if (key == UI_KEY_1) {
            ctx->key_handled = true;
            if (s_bookmark_page > 0) { --s_bookmark_page; return APP_REDRAW_PAGE; }
            s_view = WEB_HOME;
            return APP_REDRAW_PAGE;
        }
        if (key == UI_KEY_3 && (s_bookmark_page + 1) * 6 < s_bookmark_count) {
            ctx->key_handled = true;
            ++s_bookmark_page;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    if (s_view == WEB_EDIT_URL) {
        s_view = WEB_HOME;
        return APP_REDRAW_PAGE;
    }
    if (s_view == WEB_LOADING) {
        s_fetch_cancel = true;
        s_view = WEB_HOME;
        snprintf(s_notice, sizeof(s_notice), "已取消加载");
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key_long(app_ctx_t *ctx, int key) {
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    if (s_view == WEB_READING || s_view == WEB_BOOKMARKS || s_view == WEB_EDIT_URL) {
        ctx->key_handled = true;
        s_view = WEB_HOME;
        return APP_REDRAW_PAGE;
    }
    if (s_view == WEB_LOADING) {
        ctx->key_handled = true;
        s_fetch_cancel = true;
        s_view = WEB_HOME;
        snprintf(s_notice, sizeof(s_notice), "已取消加载");
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static void key_hints(app_ctx_t *ctx, ui_key_hints_t *out) {
    (void)ctx;
    if (s_view == WEB_READING) {
        out->left = s_page > 0 ? "上页 · 长按返回" : "返回";
        out->right = (s_page + 1 < book_layout_page_count() || !book_layout_complete()) ? "下一页" : NULL;
    } else if (s_view == WEB_BOOKMARKS) {
        out->left = s_bookmark_page > 0 ? "上页 · 长按返回" : "返回";
        out->right = (s_bookmark_page + 1) * 6 < s_bookmark_count ? "下一页" : NULL;
    } else if (s_view == WEB_EDIT_URL || s_view == WEB_LOADING) {
        out->left = "取消";
        out->right = NULL;
    } else {
        out->left = "返回";
        out->right = NULL;
    }
}

static EpdRect area_hint(app_ctx_t *ctx) {
    (void)ctx;
    return (EpdRect){UI_MARGIN, 200, ui_content_width(), 840};
}

void app_web_queue_url(const char *url) {
    if (!url || !url[0]) {
        s_queued_url[0] = '\0';
        return;
    }
    snprintf(s_queued_url, sizeof(s_queued_url), "%.*s",
             (int)(sizeof(s_queued_url) - 1), url);
}

const app_desc_t app_web = {
    .title = "网页阅读",
    .detail = "在线网页与浏览器剪藏",
    .owns_keys = true,
    .blocks_idle_sleep = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = web_on_exit,
    .on_media_lost = on_media_lost,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_key_long = on_key_long,
    .key_hints = key_hints,
    .on_tick = on_tick,
    .area_hint = area_hint,
};
