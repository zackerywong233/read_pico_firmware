/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：OPDS 1.2 / RSS 2.0 / Atom 1.0 订阅与在线书库页面：支持局域网配置源、
 * RSS/Atom 正文墨水屏分页阅读与存为图书、OPDS 多级目录/分页浏览及流式下载到书架、
 * TF 卡离线缓存回退。
 * English: OPDS 1.2 / RSS 2.0 / Atom 1.0 feeds and online library page: source list,
 * paginated RSS/Atom article reading with save-to-shelf, OPDS multi-level catalog
 * browsing and streaming download, and TF-card offline cache fallback.
 *
 * 冻结：页面退出、锁屏或拔卡前必须先取消并 join 后台网络任务，再停止共享网络并释放 Wi-Fi/BLE 锁；
 * 订阅源认证密码绝不在界面或公开接口中明文显示。
 * Frozen: Always cancel and join background network tasks before stopping networking and releasing
 * Wi-Fi/BLE hold on exit, lock, or media loss; never display feed passwords in UI or public APIs.
 */
#include "app.h"
#include "ble_service.h"
#include "book_layout.h"
#include "book_store.h"
#include "display.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "feeds_store.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "html_text.h"
#include "read_pico_sd.h"
#include "read_pico_transfer.h"
#include "time_sync.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const app_desc_t app_feeds;
extern const app_desc_t app_book;
extern const app_desc_t app_transfer;
extern void app_book_queue_open(const char *path);

#define TAG "app_feeds"
#define FEEDS_SD_ROOT "/sdcard"
#define FEEDS_PER_PAGE 6
#define FEEDS_NAV_DEPTH 8
#define FEEDS_FETCH_MAX (512u * 1024u)
#define FEEDS_FETCH_TIMEOUT_MS 20000
#define FEEDS_FETCH_IO_TIMEOUT_MS 2000
#define FEEDS_DOWNLOAD_TIMEOUT_MS 120000
#define FEEDS_MAX_REDIRECTS 5
#define FEEDS_BODY_PX 44

typedef enum {
    FEEDS_VIEW_SOURCES = 0,
    FEEDS_VIEW_ENTRIES,
    FEEDS_VIEW_DETAIL,
    FEEDS_VIEW_ARTICLE,
    FEEDS_VIEW_LOADING,
} feeds_view_t;

typedef enum {
    FEEDS_JOB_FETCH_XML = 0,
    FEEDS_JOB_DOWNLOAD_BOOK,
} feeds_job_kind_t;

typedef struct {
    feeds_job_kind_t kind;
    char url[FEEDS_URL_MAX];
    char auth_hdr[320];
    char save_dir[BOOK_STORE_PATH_MAX];
    char file_title[128];
    char file_ext[8];
    size_t file_limit;
    bool open_after;
} feeds_job_req_t;

typedef struct {
    feeds_job_kind_t kind;
    esp_err_t err;
    char url[FEEDS_URL_MAX];
    char *xml_data;
    size_t xml_len;
    char saved_path[BOOK_STORE_PATH_MAX];
    bool open_after;
} feeds_job_res_t;

static feeds_view_t s_view;
static feeds_view_t s_prev_view;
static feed_source_t s_sources[FEEDS_MAX_SOURCES];
static size_t s_source_count;
static int s_active_source = -1;
static int s_source_page;
static bool s_delete_mode;

static char s_nav_urls[FEEDS_NAV_DEPTH][FEEDS_URL_MAX];
static int s_nav_depth;
static feed_parsed_t s_parsed;
static int s_entry_page;
static int s_selected_entry = -1;
static char s_last_saved_path[BOOK_STORE_PATH_MAX];

static html_text_t s_article;
static size_t s_article_page;
static char s_article_title[160];

static book_store_root_t s_root;
static read_pico_transfer_status_t s_net;
static bool s_net_started;
static bool s_net_ready;
static int64_t s_net_poll_ms;
static bool s_storage_changed;
static char s_notice[160];

static QueueHandle_t s_res_queue;
static volatile bool s_worker_running;
static volatile bool s_worker_cancel;
static TaskHandle_t s_worker_task;

static EpdRect article_body_rect(void) {
    return (EpdRect){UI_MARGIN, UI_CONTENT_TOP, ui_content_width(), UI_BAR_TOP - UI_CONTENT_TOP - 8};
}

static void draw_ellipsis(uint8_t *fb, int x, int y, int px, int max_w, const char *text) {
    if (!text || !text[0]) return;
    if (ttf_text_width_px(px, text) <= max_w) {
        ui_text(fb, x, y, px, text, EPD_DRAW_ALIGN_LEFT, false);
        return;
    }
    char buf[196];
    size_t len = strlen(text);
    if (len > sizeof(buf) - 4) len = sizeof(buf) - 4;
    while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80) --len;
    while (len > 0) {
        memcpy(buf, text, len);
        memcpy(buf + len, "\xe2\x80\xa6", 4);
        if (ttf_text_width_px(px, buf) <= max_w) break;
        do { --len; } while (len > 0 && ((unsigned char)text[len] & 0xC0) == 0x80);
    }
    if (len == 0) strcpy(buf, "\xe2\x80\xa6");
    ui_text(fb, x, y, px, buf, EPD_DRAW_ALIGN_LEFT, false);
}

static bool sd_is_mounted(void) {
    read_pico_sd_info_t info = {0};
    return read_pico_sd_get_info(&info) == ESP_OK && info.mounted;
}

static uint64_t feeds_free_bytes(void *ctx) {
    return book_store_free_bytes(ctx);
}

static esp_err_t feeds_file_changed(const char *path) {
    return book_store_forget_changed_path(path);
}

static void free_article(void) {
    book_layout_free();
    html_text_free(&s_article);
    s_article_page = 0;
    s_article_title[0] = '\0';
}

static void free_job_res(feeds_job_res_t *res) {
    if (!res) return;
    free(res->xml_data);
    free(res);
}

static void reload_sources(void) {
    memset(s_sources, 0, sizeof(s_sources));
    s_source_count = 0;
    (void)feeds_store_load(s_sources, FEEDS_MAX_SOURCES, &s_source_count);
    int pages = (int)((s_source_count + FEEDS_PER_PAGE - 1) / FEEDS_PER_PAGE);
    if (pages <= 0) pages = 1;
    if (s_source_page >= pages) s_source_page = pages - 1;
    if (s_source_page < 0) s_source_page = 0;
}

/* ---- HTTP 后台抓取与流式下载 / Background HTTP fetch & streaming download ---- */

static bool is_redirect_status(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

static esp_err_t open_http_follow(esp_http_client_handle_t client, const char *auth_hdr,
                                  int64_t deadline, int *out_status) {
    int redirects = 0;
    int status = 0;
    esp_err_t err = ESP_OK;
    while (!s_worker_cancel && esp_timer_get_time() < deadline) {
        if (auth_hdr && auth_hdr[0]) {
            (void)esp_http_client_set_header(client, "Authorization", auth_hdr);
        }
        err = esp_http_client_open(client, 0);
        if (err != ESP_OK) break;
        int64_t hdr = esp_http_client_fetch_headers(client);
        while (hdr == -ESP_ERR_HTTP_EAGAIN && !s_worker_cancel && esp_timer_get_time() < deadline) {
            hdr = esp_http_client_fetch_headers(client);
        }
        if (hdr < 0) {
            err = (hdr == -ESP_ERR_HTTP_EAGAIN) ? ESP_ERR_TIMEOUT : ESP_FAIL;
            break;
        }
        status = esp_http_client_get_status_code(client);
        if (!is_redirect_status(status)) break;
        if (redirects >= FEEDS_MAX_REDIRECTS) {
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
    if (out_status) *out_status = status;
    return err;
}

static esp_err_t do_fetch_xml(const feeds_job_req_t *req, char **out_data, size_t *out_len) {
    *out_data = NULL;
    *out_len = 0;
    esp_http_client_config_t cfg = {
        .url = req->url,
        .timeout_ms = FEEDS_FETCH_IO_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    int64_t deadline = esp_timer_get_time() + (int64_t)FEEDS_FETCH_TIMEOUT_MS * 1000;
    int status = 0;
    esp_err_t err = open_http_follow(client, req->auth_hdr, deadline, &status);

    char *buf = NULL;
    size_t len = 0, cap = 0;
    char chunk[4096];
    while (err == ESP_OK && !s_worker_cancel && esp_timer_get_time() < deadline) {
        int n = esp_http_client_read(client, chunk, sizeof(chunk));
        if (n == 0) break;
        if (n == -ESP_ERR_HTTP_EAGAIN) continue;
        if (n < 0) { err = ESP_FAIL; break; }
        size_t add = (size_t)n;
        if (len + add > FEEDS_FETCH_MAX) add = FEEDS_FETCH_MAX - len;
        if (add == 0) break;
        if (len + add + 1 > cap) {
            size_t next_cap = cap ? cap * 2 : 8192;
            while (next_cap < len + add + 1) next_cap *= 2;
            if (next_cap > FEEDS_FETCH_MAX + 1) next_cap = FEEDS_FETCH_MAX + 1;
            char *next = heap_caps_realloc(buf, next_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!next) next = realloc(buf, next_cap);
            if (!next) { err = ESP_ERR_NO_MEM; break; }
            buf = next;
            cap = next_cap;
        }
        memcpy(buf + len, chunk, add);
        len += add;
        buf[len] = '\0';
    }
    if (s_worker_cancel) err = ESP_ERR_INVALID_STATE;
    else if (err == ESP_OK && esp_timer_get_time() >= deadline) err = ESP_ERR_TIMEOUT;

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && (status < 200 || status >= 300)) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && (!buf || len == 0)) err = ESP_ERR_INVALID_RESPONSE;
    if (err != ESP_OK) {
        free(buf);
        return err;
    }
    *out_data = buf;
    *out_len = len;
    return ESP_OK;
}

static void sanitize_filename(const char *title, char out[64]) {
    size_t w = 0;
    size_t len = title ? strlen(title) : 0;
    for (size_t i = 0; i < len && w < 44;) {
        unsigned char c = (unsigned char)title[i];
        size_t step = (c < 0x80) ? 1 : ((c < 0xE0) ? 2 : ((c < 0xF0) ? 3 : 4));
        if (i + step > len || w + step > 44) break;
        if (step == 1) {
            if (isalnum(c) || c == '-' || c == '_') out[w++] = (char)c;
            else if (c == ' ') out[w++] = '_';
        } else {
            memcpy(out + w, title + i, step);
            w += step;
        }
        i += step;
    }
    if (w == 0) {
        strcpy(out, "book");
        return;
    }
    out[w] = '\0';
}

static esp_err_t do_download_book(const feeds_job_req_t *req, char out_path[BOOK_STORE_PATH_MAX]) {
    out_path[0] = '\0';
    if (!req->save_dir[0] || !req->file_ext[0]) return ESP_ERR_INVALID_ARG;

    char base[64];
    sanitize_filename(req->file_title, base);
    char final_path[BOOK_STORE_PATH_MAX];
    bool picked = false;
    for (unsigned s = 0; s < 100; ++s) {
        int r = s ? snprintf(final_path, sizeof(final_path), "%s/%s-%u.%s",
                             req->save_dir, base, s + 1, req->file_ext)
                  : snprintf(final_path, sizeof(final_path), "%s/%s.%s",
                             req->save_dir, base, req->file_ext);
        if (r <= 0 || (size_t)r >= sizeof(final_path)) return ESP_ERR_INVALID_SIZE;
        if (access(final_path, F_OK) != 0) {
            picked = true;
            break;
        }
    }
    if (!picked) return ESP_ERR_NO_MEM;

    char tmp_path[BOOK_STORE_PATH_MAX];
    if (snprintf(tmp_path, sizeof(tmp_path), "%s/.feed_dl.tmp", req->save_dir) >= (int)sizeof(tmp_path)) {
        return ESP_ERR_INVALID_SIZE;
    }
    unlink(tmp_path);

    esp_http_client_config_t cfg = {
        .url = req->url,
        .timeout_ms = FEEDS_FETCH_IO_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;

    int64_t deadline = esp_timer_get_time() + (int64_t)FEEDS_DOWNLOAD_TIMEOUT_MS * 1000;
    int status = 0;
    esp_err_t err = open_http_follow(client, req->auth_hdr, deadline, &status);

    int fd = -1;
    size_t written = 0;
    if (err == ESP_OK && status >= 200 && status < 300) {
        fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) err = ESP_FAIL;
    } else if (err == ESP_OK) {
        err = ESP_ERR_INVALID_RESPONSE;
    }

    char chunk[4096];
    while (err == ESP_OK && !s_worker_cancel && esp_timer_get_time() < deadline) {
        int n = esp_http_client_read(client, chunk, sizeof(chunk));
        if (n == 0) break;
        if (n == -ESP_ERR_HTTP_EAGAIN) continue;
        if (n < 0) { err = ESP_FAIL; break; }
        if (req->file_limit && written + (size_t)n > req->file_limit) {
            err = ESP_ERR_INVALID_SIZE;
            break;
        }
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = write(fd, chunk + off, (size_t)n - off);
            if (w <= 0) { err = ESP_FAIL; break; }
            off += (size_t)w;
        }
        written += (size_t)n;
    }
    if (fd >= 0) close(fd);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (s_worker_cancel) err = ESP_ERR_INVALID_STATE;
    else if (err == ESP_OK && esp_timer_get_time() >= deadline) err = ESP_ERR_TIMEOUT;
    else if (err == ESP_OK && written == 0) err = ESP_ERR_INVALID_RESPONSE;

    if (err != ESP_OK) {
        unlink(tmp_path);
        return err;
    }
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return ESP_FAIL;
    }
    snprintf(out_path, BOOK_STORE_PATH_MAX, "%s", final_path);
    return ESP_OK;
}

static void feeds_worker_task(void *arg) {
    feeds_job_req_t *req = arg;
    feeds_job_res_t *res = heap_caps_calloc(1, sizeof(*res), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!res) res = calloc(1, sizeof(*res));
    if (res) {
        res->kind = req->kind;
        res->open_after = req->open_after;
        snprintf(res->url, sizeof(res->url), "%s", req->url);
        if (req->kind == FEEDS_JOB_FETCH_XML) {
            res->err = do_fetch_xml(req, &res->xml_data, &res->xml_len);
        } else {
            res->err = do_download_book(req, res->saved_path);
        }
        if (!s_worker_cancel && xQueueSend(s_res_queue, &res, 0) == pdTRUE) {
            res = NULL;
        }
    }
    free_job_res(res);
    memset(req->auth_hdr, 0, sizeof(req->auth_hdr));
    free(req);
    s_worker_running = false;
    s_worker_task = NULL;
    vTaskDelete(NULL);
}

static void join_worker(void) {
    s_worker_cancel = true;
    while (s_worker_running) vTaskDelay(pdMS_TO_TICKS(20));
    feeds_job_res_t *res = NULL;
    while (s_res_queue && xQueueReceive(s_res_queue, &res, 0) == pdTRUE) {
        free_job_res(res);
    }
}

static bool start_xml_fetch(const char *url) {
    if (s_worker_running || !feeds_url_valid(url)) return false;
    if (!s_net_ready) {
        // 离线回退 TF 卡缓存 / Fallback to TF card offline cache when offline.
        if (sd_is_mounted()) {
            char *cached = NULL;
            size_t cached_len = 0;
            if (feeds_cache_read(FEEDS_SD_ROOT, url, &cached, &cached_len)) {
                feed_parsed_t next = {0};
                bool ok = feeds_parse_xml(cached, cached_len, url, &next, FEEDS_MAX_ITEMS);
                free(cached);
                if (ok) {
                    feeds_parsed_free(&s_parsed);
                    s_parsed = next;
                    s_entry_page = 0;
                    s_selected_entry = -1;
                    s_view = FEEDS_VIEW_ENTRIES;
                    snprintf(s_notice, sizeof(s_notice), "离线模式：已加载本地缓存");
                    return true;
                }
            }
        }
        snprintf(s_notice, sizeof(s_notice), "未联网且无本地缓存，请先在传书页配网");
        return false;
    }

    feeds_job_req_t *req = calloc(1, sizeof(*req));
    if (!req) return false;
    req->kind = FEEDS_JOB_FETCH_XML;
    snprintf(req->url, sizeof(req->url), "%s", url);
    if (s_active_source >= 0 && (size_t)s_active_source < s_source_count) {
        const feed_source_t *src = &s_sources[s_active_source];
        (void)feeds_build_basic_auth(src->auth_user, src->auth_pass, req->auth_hdr, sizeof(req->auth_hdr));
    }
    s_worker_running = true;
    s_worker_cancel = false;
    if (xTaskCreatePinnedToCore(feeds_worker_task, "feeds_net", 12 * 1024, req, 4, &s_worker_task, 1) != pdPASS) {
        s_worker_running = false;
        memset(req->auth_hdr, 0, sizeof(req->auth_hdr));
        free(req);
        snprintf(s_notice, sizeof(s_notice), "无法启动网络任务");
        return false;
    }
    s_prev_view = s_view;
    s_view = FEEDS_VIEW_LOADING;
    snprintf(s_notice, sizeof(s_notice), "正在拉取订阅内容…");
    return true;
}

static bool start_book_download(const feed_item_t *item, bool open_after) {
    if (s_worker_running || !s_net_ready || !item || !item->acq_url[0]) return false;
    if (!s_root.path[0]) {
        snprintf(s_notice, sizeof(s_notice), "书架存储不可用");
        return false;
    }
    feeds_job_req_t *req = calloc(1, sizeof(*req));
    if (!req) return false;
    req->kind = FEEDS_JOB_DOWNLOAD_BOOK;
    req->open_after = open_after;
    req->file_limit = book_store_file_limit(&s_root);
    snprintf(req->url, sizeof(req->url), "%s", item->acq_url);
    snprintf(req->save_dir, sizeof(req->save_dir), "%s", s_root.path);
    snprintf(req->file_title, sizeof(req->file_title), "%.*s", (int)(sizeof(req->file_title) - 1), item->title);
    snprintf(req->file_ext, sizeof(req->file_ext), "%s", item->acq_ext);
    if (s_active_source >= 0 && (size_t)s_active_source < s_source_count) {
        const feed_source_t *src = &s_sources[s_active_source];
        (void)feeds_build_basic_auth(src->auth_user, src->auth_pass, req->auth_hdr, sizeof(req->auth_hdr));
    }
    s_worker_running = true;
    s_worker_cancel = false;
    if (xTaskCreatePinnedToCore(feeds_worker_task, "feeds_dl", 12 * 1024, req, 4, &s_worker_task, 1) != pdPASS) {
        s_worker_running = false;
        memset(req->auth_hdr, 0, sizeof(req->auth_hdr));
        free(req);
        snprintf(s_notice, sizeof(s_notice), "无法启动下载任务");
        return false;
    }
    s_prev_view = s_view;
    s_view = FEEDS_VIEW_LOADING;
    snprintf(s_notice, sizeof(s_notice), "正在下载图书到书架…");
    return true;
}

/* ---- 网络启停与文章保存 / Network lifecycle & article save ---- */

static void network_start(void) {
    char ssid[33];
    bool configured = false;
    if (read_pico_transfer_get_saved_wifi(ssid, &configured) != ESP_OK || !configured) {
        snprintf(s_notice, sizeof(s_notice), "未配置 Wi-Fi，可浏览离线缓存或去传书页配网");
        return;
    }
    if (book_store_upload_root(&s_root) != ESP_OK) {
        memset(&s_root, 0, sizeof(s_root));
    }
    read_pico_transfer_cfg_t cfg = {
        .mode = READ_PICO_TRANSFER_MODE_STA,
        .root_dir = s_root.path[0] ? s_root.path : "/sdcard/books",
        .is_flash = s_root.is_flash,
        .file_limit = s_root.path[0] ? book_store_file_limit(&s_root) : 0,
        .free_bytes_cb = feeds_free_bytes,
        .free_bytes_ctx = &s_root,
        .file_changed_cb = feeds_file_changed,
        .sta_ip_cb = app_time_sync_sta_ip,
        .service_poll_cb = app_time_sync_poll,
        .network_stop_cb = app_time_sync_stop,
    };
    if (read_pico_transfer_start(&cfg) == ESP_OK) {
        s_net_started = true;
        snprintf(s_notice, sizeof(s_notice), "正在连接已保存 Wi-Fi…");
    } else {
        snprintf(s_notice, sizeof(s_notice), "Wi-Fi 启动失败，可浏览本地缓存");
    }
}

static void network_stop(void) {
    if (!s_net_started) return;
    read_pico_transfer_stop();
    s_net_started = false;
    s_net_ready = false;
    memset(&s_net, 0, sizeof(s_net));
}

static bool open_article_entry(const feed_item_t *item) {
    if (!item) return false;
    free_article();
    const char *src = (item->content_html && item->content_len) ? item->content_html : item->summary;
    size_t src_len = src ? strlen(src) : 0;
    if (!src_len) {
        snprintf(s_notice, sizeof(s_notice), "该条目没有可阅读正文");
        return false;
    }
    if (html_to_web_blocks(src, src_len, &s_article) != ESP_OK || !s_article.len) {
        free_article();
        snprintf(s_notice, sizeof(s_notice), "文章排版失败");
        return false;
    }
    if (!book_layout_begin_blocks(s_article.utf8, s_article.len, s_article.blocks,
                                  s_article.count, article_body_rect(), FEEDS_BODY_PX)) {
        free_article();
        snprintf(s_notice, sizeof(s_notice), "文章内容为空");
        return false;
    }
    snprintf(s_article_title, sizeof(s_article_title), "%s", item->title);
    s_article_page = 0;
    s_view = FEEDS_VIEW_ARTICLE;
    s_notice[0] = '\0';
    return true;
}

static bool save_article_txt(void) {
    if (!s_article.utf8 || !s_article.len || !s_root.path[0]) return false;
    char base[64], path[BOOK_STORE_PATH_MAX];
    sanitize_filename(s_article_title, base);
    for (unsigned s = 0; s < 100; ++s) {
        int r = s ? snprintf(path, sizeof(path), "%s/%s-%u.txt", s_root.path, base, s + 1)
                  : snprintf(path, sizeof(path), "%s/%s.txt", s_root.path, base);
        if (r <= 0 || (size_t)r >= sizeof(path)) return false;
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return false;
        }
        size_t done = 0;
        while (done < s_article.len) {
            ssize_t w = write(fd, s_article.utf8 + done, s_article.len - done);
            if (w <= 0) { close(fd); unlink(path); return false; }
            done += (size_t)w;
        }
        close(fd);
        book_store_notify_changed();
        s_storage_changed = true;
        snprintf(s_notice, sizeof(s_notice), "已保存到书架：%s.txt", base);
        return true;
    }
    return false;
}

/* ---- 视图渲染 / View rendering ---- */

static void render_sources(uint8_t *fb) {
    char sub[96];
    snprintf(sub, sizeof(sub), "%s · 共 %u 个源",
             s_net_ready ? "Wi-Fi 已连接" : (s_net_started ? "连接中…" : "离线模式"),
             (unsigned)s_source_count);
    ui_draw_header(fb, "订阅书库", sub);

    if (s_notice[0]) {
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 2, UI_PX_CAPTION, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    }

    int list_y = UI_CONTENT_TOP + 40;
    if (s_source_count == 0) {
        ui_text_vc(fb, epd_rotated_display_width() / 2, list_y + 180, UI_PX_BODY,
                   "暂无订阅源", EPD_DRAW_ALIGN_CENTER, false);
        ui_text_vc(fb, epd_rotated_display_width() / 2, list_y + 236, UI_PX_CAPTION,
                   "请在「传书」网页添加 OPDS / RSS 地址", EPD_DRAW_ALIGN_CENTER, false);
    } else {
        size_t start = (size_t)s_source_page * FEEDS_PER_PAGE;
        for (int row = 0; row < FEEDS_PER_PAGE && start + (size_t)row < s_source_count; ++row) {
            const feed_source_t *src = &s_sources[start + (size_t)row];
            EpdRect r = ui_grid_rect(0, 1, row, list_y, 112);
            ui_draw_round_rect(fb, r, UI_BTN_RADIUS, UI_GRAY_BLACK);
            const char *badge = (src->kind == FEED_KIND_OPDS) ? "OPDS" : "RSS";
            char head[96];
            snprintf(head, sizeof(head), "[%s] %s", badge, src->name);
            draw_ellipsis(fb, r.x + 18, r.y + 16, UI_PX_BODY, r.width - 36, head);
            draw_ellipsis(fb, r.x + 18, r.y + 66, UI_PX_CAPTION, r.width - 36, src->url);
        }
    }

    int pages = (int)((s_source_count + FEEDS_PER_PAGE - 1) / FEEDS_PER_PAGE);
    if (pages <= 0) pages = 1;
    ui_draw_button(fb, ui_bar_rect(0, 3), "去传书配置", false);
    ui_draw_button(fb, ui_bar_rect(1, 3), s_delete_mode ? "取消删除" : "删除源", s_delete_mode);
    char pg[36];
    snprintf(pg, sizeof(pg), "%d / %d 页", s_source_page + 1, pages);
    ui_draw_button(fb, ui_bar_rect(2, 3), pg, false);
}

static void render_entries(uint8_t *fb) {
    const char *title = s_parsed.title[0] ? s_parsed.title : "订阅目录";
    char sub[96];
    int pages = (int)((s_parsed.count + FEEDS_PER_PAGE - 1) / FEEDS_PER_PAGE);
    if (pages <= 0) pages = 1;
    snprintf(sub, sizeof(sub), "%s · %u 项 · 第 %d/%d 页",
             s_parsed.doc_kind == FEED_DOC_OPDS ? "OPDS" : "RSS/Atom",
             (unsigned)s_parsed.count, s_entry_page + 1, pages);
    ui_draw_header(fb, title, sub);

    if (s_notice[0]) {
        ui_text(fb, UI_MARGIN, UI_CONTENT_TOP + 2, UI_PX_CAPTION, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    }

    int list_y = UI_CONTENT_TOP + 40;
    size_t start = (size_t)s_entry_page * FEEDS_PER_PAGE;
    for (int row = 0; row < FEEDS_PER_PAGE && start + (size_t)row < s_parsed.count; ++row) {
        const feed_item_t *it = &s_parsed.items[start + (size_t)row];
        EpdRect r = ui_grid_rect(0, 1, row, list_y, 112);
        ui_draw_round_rect(fb, r, UI_BTN_RADIUS, UI_GRAY_BLACK);
        draw_ellipsis(fb, r.x + 18, r.y + 16, UI_PX_BODY, r.width - 36, it->title);
        char meta[128];
        if (it->acq_url[0]) {
            snprintf(meta, sizeof(meta), "可下载 %s%s%s", it->acq_ext,
                     it->author[0] ? " · " : "", it->author);
        } else if (it->is_nav) {
            snprintf(meta, sizeof(meta), "子目录 · 点按进入");
        } else {
            snprintf(meta, sizeof(meta), "%.*s", (int)(sizeof(meta) - 1),
                     it->author[0] ? it->author : (it->summary[0] ? it->summary : "点按阅读"));
        }
        draw_ellipsis(fb, r.x + 18, r.y + 66, UI_PX_CAPTION, r.width - 36, meta);
    }

    ui_draw_button(fb, ui_bar_rect(0, 4), "返回", false);
    ui_draw_button(fb, ui_bar_rect(1, 4), "刷新", false);
    ui_draw_button(fb, ui_bar_rect(2, 4), "上一页", false);
    ui_draw_button(fb, ui_bar_rect(3, 4),
                   (s_entry_page + 1 >= pages && s_parsed.next_url[0]) ? "下批目录" : "下一页",
                   false);
}

static void render_detail(uint8_t *fb) {
    if (s_selected_entry < 0 || (size_t)s_selected_entry >= s_parsed.count) {
        s_view = FEEDS_VIEW_ENTRIES;
        render_entries(fb);
        return;
    }
    const feed_item_t *it = &s_parsed.items[s_selected_entry];
    ui_draw_header(fb, "书目详情", it->acq_ext[0] ? it->acq_ext : "OPDS");

    int y = UI_CONTENT_TOP + 16;
    draw_ellipsis(fb, UI_MARGIN, y, UI_PX_TITLE, ui_content_width(), it->title);
    y += 64;
    if (it->author[0]) {
        y = ui_draw_row(fb, y, "作者", it->author);
    }
    if (it->acq_ext[0]) {
        y = ui_draw_row(fb, y, "格式", it->acq_ext);
    }
    if (it->summary[0]) {
        y += 12;
        draw_ellipsis(fb, UI_MARGIN, y, UI_PX_BODY, ui_content_width(), it->summary);
        y += 56;
    }
    if (s_notice[0]) {
        ui_text(fb, UI_MARGIN, y + 12, UI_PX_CAPTION, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    }

    int by = 720;
    ui_draw_button(fb, ui_row_rect(0, 1, by, UI_BTN_H), "下载并立即阅读", true);
    ui_draw_button(fb, ui_row_rect(0, 1, by + 96, UI_BTN_H), "仅下载到书架", false);
    if (s_last_saved_path[0]) {
        ui_draw_button(fb, ui_row_rect(0, 1, by + 192, UI_BTN_H), "打开已下载图书", false);
    }
    ui_draw_button(fb, ui_bar_rect(0, 1), "返回目录", false);
}

static void render_article(uint8_t *fb) {
    size_t total = book_layout_page_count();
    char sub[64];
    snprintf(sub, sizeof(sub), "第 %u / %u 页",
             (unsigned)(s_article_page + 1), (unsigned)(total ? total : 1));
    ui_draw_header(fb, s_article_title[0] ? s_article_title : "订阅阅读", sub);
    book_layout_draw_page(fb, s_article_page, article_body_rect(), FEEDS_BODY_PX);
    if (s_notice[0]) {
        ui_text(fb, UI_MARGIN, UI_BAR_TOP - 32, UI_PX_CAPTION, s_notice, EPD_DRAW_ALIGN_LEFT, false);
    }
    ui_draw_button(fb, ui_bar_rect(0, 4), "返回", false);
    ui_draw_button(fb, ui_bar_rect(1, 4), "上一页", false);
    ui_draw_button(fb, ui_bar_rect(2, 4), "下一页", false);
    ui_draw_button(fb, ui_bar_rect(3, 4), "存为图书", false);
}

static void render_loading(uint8_t *fb) {
    ui_draw_header(fb, "订阅书库", "网络请求中");
    ui_text_vc(fb, epd_rotated_display_width() / 2, UI_CONTENT_TOP + 220, UI_PX_BODY,
               s_notice[0] ? s_notice : "正在加载…", EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_button(fb, ui_bar_rect(0, 1), "取消", false);
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    (void)ctx;
    ui_clear_page(fb);
    if (s_view == FEEDS_VIEW_LOADING) render_loading(fb);
    else if (s_view == FEEDS_VIEW_ENTRIES) render_entries(fb);
    else if (s_view == FEEDS_VIEW_DETAIL) render_detail(fb);
    else if (s_view == FEEDS_VIEW_ARTICLE) render_article(fb);
    else render_sources(fb);
}

/* ---- 交互与生命周期 / Interaction & lifecycle ---- */

static void on_enter(app_ctx_t *ctx) {
    (void)ctx;
    app_ble_wifi_hold(true);
    if (!s_res_queue) s_res_queue = xQueueCreate(1, sizeof(feeds_job_res_t *));
    (void)ttf_font_suspend_sd(true);
    display_set_bulk_io(true);
    s_view = FEEDS_VIEW_SOURCES;
    s_prev_view = FEEDS_VIEW_SOURCES;
    s_active_source = -1;
    s_source_page = 0;
    s_delete_mode = false;
    s_nav_depth = 0;
    s_entry_page = 0;
    s_selected_entry = -1;
    s_last_saved_path[0] = '\0';
    s_storage_changed = false;
    s_net_started = false;
    s_net_ready = false;
    s_net_poll_ms = 0;
    s_notice[0] = '\0';
    feeds_parsed_free(&s_parsed);
    free_article();
    reload_sources();
    network_start();
}

static void feeds_on_exit(app_ctx_t *ctx) {
    (void)ctx;
    join_worker();
    network_stop();
    feeds_parsed_free(&s_parsed);
    free_article();
    memset(s_sources, 0, sizeof(s_sources));
    app_ble_wifi_hold(false);
    if (s_storage_changed) book_store_notify_changed();
    if (s_res_queue) { vQueueDelete(s_res_queue); s_res_queue = NULL; }
    display_set_bulk_io(false);
    (void)ttf_font_suspend_sd(false);
}

static void feeds_on_media_lost(app_ctx_t *ctx) {
    (void)ctx;
    join_worker();
    if (!s_root.is_flash) {
        network_stop();
        memset(&s_root, 0, sizeof(s_root));
        s_view = FEEDS_VIEW_SOURCES;
        snprintf(s_notice, sizeof(s_notice), "TF 卡已移除，网络任务已停止");
    }
}

static app_redraw_t on_tick(app_ctx_t *ctx) {
    feeds_job_res_t *res = NULL;
    if (s_res_queue && xQueueReceive(s_res_queue, &res, 0) == pdTRUE) {
        if (res->kind == FEEDS_JOB_FETCH_XML) {
            if (res->err == ESP_OK && res->xml_data && res->xml_len) {
                feed_parsed_t next = {0};
                if (feeds_parse_xml(res->xml_data, res->xml_len, res->url, &next, FEEDS_MAX_ITEMS)) {
                    if (sd_is_mounted()) {
                        (void)feeds_cache_write(FEEDS_SD_ROOT, res->url, res->xml_data, res->xml_len);
                    }
                    feeds_parsed_free(&s_parsed);
                    s_parsed = next;
                    s_entry_page = 0;
                    s_selected_entry = -1;
                    s_view = FEEDS_VIEW_ENTRIES;
                    s_notice[0] = '\0';
                } else {
                    s_view = s_prev_view;
                    snprintf(s_notice, sizeof(s_notice), "订阅 XML 解析失败");
                }
            } else {
                s_view = s_prev_view;
                snprintf(s_notice, sizeof(s_notice), "拉取失败（%s）", esp_err_to_name(res->err));
            }
            free_job_res(res);
            return APP_REDRAW_PAGE;
        }
        // OPDS 图书下载完成 / OPDS book download completed.
        if (res->err == ESP_OK && res->saved_path[0]) {
            book_store_notify_changed();
            s_storage_changed = true;
            snprintf(s_last_saved_path, sizeof(s_last_saved_path), "%s", res->saved_path);
            if (res->open_after) {
                app_book_queue_open(res->saved_path);
                free_job_res(res);
                ctx->request_app = &app_book;
                return APP_REDRAW_NONE;
            }
            s_view = FEEDS_VIEW_DETAIL;
            snprintf(s_notice, sizeof(s_notice), "已下载到书架，可直接打开阅读");
        } else {
            s_view = FEEDS_VIEW_DETAIL;
            snprintf(s_notice, sizeof(s_notice), "下载失败（%s）", esp_err_to_name(res->err));
        }
        free_job_res(res);
        return APP_REDRAW_PAGE;
    }

    if (!s_net_started || ctx->now_ms - s_net_poll_ms < 500) return APP_REDRAW_NONE;
    s_net_poll_ms = ctx->now_ms;
    read_pico_transfer_service_poll();
    read_pico_transfer_status_t next = {0};
    read_pico_transfer_get_status(&next);
    bool changed = (next.network_ready != s_net.network_ready || next.state != s_net.state);
    s_net = next;
    if (s_net.network_ready && !s_net_ready) {
        s_net_ready = true;
        snprintf(s_notice, sizeof(s_notice), "Wi-Fi 已连接，点选订阅源开始浏览");
        return APP_REDRAW_PAGE;
    }
    if (!s_net.network_ready && s_net_ready) {
        s_net_ready = false;
        snprintf(s_notice, sizeof(s_notice), "Wi-Fi 连接中断，正在重试…");
        return APP_REDRAW_PAGE;
    }
    return changed ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
}

static bool navigate_back(void) {
    if (s_view == FEEDS_VIEW_LOADING) {
        s_worker_cancel = true;
        s_view = s_prev_view;
        snprintf(s_notice, sizeof(s_notice), "已取消请求");
        return true;
    }
    if (s_view == FEEDS_VIEW_ARTICLE) {
        free_article();
        s_view = FEEDS_VIEW_ENTRIES;
        return true;
    }
    if (s_view == FEEDS_VIEW_DETAIL) {
        s_view = FEEDS_VIEW_ENTRIES;
        s_notice[0] = '\0';
        return true;
    }
    if (s_view == FEEDS_VIEW_ENTRIES) {
        if (s_nav_depth > 1) {
            s_nav_depth--;
            (void)start_xml_fetch(s_nav_urls[s_nav_depth - 1]);
            return true;
        }
        s_nav_depth = 0;
        s_view = FEEDS_VIEW_SOURCES;
        s_notice[0] = '\0';
        return true;
    }
    return false;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (!ev) return APP_REDRAW_NONE;
    if (s_view == FEEDS_VIEW_ARTICLE) {
        size_t total = book_layout_page_count();
        if (ev->type == UI_GESTURE_SWIPE_L) {
            if (s_article_page + 1 < total) { s_article_page++; return APP_REDRAW_PAGE; }
            return APP_REDRAW_NONE;
        }
        if (ev->type == UI_GESTURE_SWIPE_R) {
            if (s_article_page > 0) { s_article_page--; return APP_REDRAW_PAGE; }
            return APP_REDRAW_NONE;
        }
        if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
        if (ui_rect_hit(ui_bar_rect(0, 4), ev->x, ev->y)) { navigate_back(); return APP_REDRAW_PAGE; }
        if (ui_rect_hit(ui_bar_rect(1, 4), ev->x, ev->y) && s_article_page > 0) { s_article_page--; return APP_REDRAW_PAGE; }
        if (ui_rect_hit(ui_bar_rect(2, 4), ev->x, ev->y) && s_article_page + 1 < total) { s_article_page++; return APP_REDRAW_PAGE; }
        if (ui_rect_hit(ui_bar_rect(3, 4), ev->x, ev->y)) {
            if (!save_article_txt()) snprintf(s_notice, sizeof(s_notice), "保存失败：书架不可用或空间不足");
            return APP_REDRAW_PAGE;
        }
        if (ev->x < epd_rotated_display_width() / 3 && s_article_page > 0) { s_article_page--; return APP_REDRAW_PAGE; }
        if (ev->x > epd_rotated_display_width() * 2 / 3 && s_article_page + 1 < total) { s_article_page++; return APP_REDRAW_PAGE; }
        return APP_REDRAW_NONE;
    }

    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;
    uint16_t x = ev->x, y = ev->y;

    if (s_view == FEEDS_VIEW_LOADING) {
        if (ui_rect_hit(ui_bar_rect(0, 1), x, y)) {
            navigate_back();
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }

    if (s_view == FEEDS_VIEW_SOURCES) {
        if (ui_rect_hit(ui_bar_rect(0, 3), x, y)) {
            ctx->request_app = &app_transfer;
            return APP_REDRAW_NONE;
        }
        if (ui_rect_hit(ui_bar_rect(1, 3), x, y)) {
            s_delete_mode = !s_delete_mode;
            snprintf(s_notice, sizeof(s_notice), "%s", s_delete_mode ? "点按任意源将其删除" : "");
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(ui_bar_rect(2, 3), x, y)) {
            int pages = (int)((s_source_count + FEEDS_PER_PAGE - 1) / FEEDS_PER_PAGE);
            if (pages > 1) { s_source_page = (s_source_page + 1) % pages; return APP_REDRAW_PAGE; }
            return APP_REDRAW_NONE;
        }
        int list_y = UI_CONTENT_TOP + 40;
        size_t start = (size_t)s_source_page * FEEDS_PER_PAGE;
        for (int row = 0; row < FEEDS_PER_PAGE && start + (size_t)row < s_source_count; ++row) {
            if (!ui_rect_hit(ui_grid_rect(0, 1, row, list_y, 112), x, y)) continue;
            size_t idx = start + (size_t)row;
            if (s_delete_mode) {
                bool found = false;
                (void)feeds_store_delete(s_sources[idx].url, &found);
                s_delete_mode = false;
                reload_sources();
                snprintf(s_notice, sizeof(s_notice), "%s", found ? "已删除订阅源" : "删除失败");
                return APP_REDRAW_PAGE;
            }
            s_active_source = (int)idx;
            s_nav_depth = 1;
            snprintf(s_nav_urls[0], sizeof(s_nav_urls[0]), "%s", s_sources[idx].url);
            (void)start_xml_fetch(s_nav_urls[0]);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }

    if (s_view == FEEDS_VIEW_ENTRIES) {
        int pages = (int)((s_parsed.count + FEEDS_PER_PAGE - 1) / FEEDS_PER_PAGE);
        if (pages <= 0) pages = 1;
        if (ui_rect_hit(ui_bar_rect(0, 4), x, y)) { navigate_back(); return APP_REDRAW_PAGE; }
        if (ui_rect_hit(ui_bar_rect(1, 4), x, y) && s_nav_depth > 0) {
            (void)start_xml_fetch(s_nav_urls[s_nav_depth - 1]);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(ui_bar_rect(2, 4), x, y) && s_entry_page > 0) {
            s_entry_page--;
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(ui_bar_rect(3, 4), x, y)) {
            if (s_entry_page + 1 < pages) { s_entry_page++; return APP_REDRAW_PAGE; }
            if (s_parsed.next_url[0]) {
                if (s_nav_depth < FEEDS_NAV_DEPTH) {
                    snprintf(s_nav_urls[s_nav_depth++], sizeof(s_nav_urls[0]), "%s", s_parsed.next_url);
                } else {
                    snprintf(s_nav_urls[FEEDS_NAV_DEPTH - 1], sizeof(s_nav_urls[0]), "%s", s_parsed.next_url);
                }
                (void)start_xml_fetch(s_nav_urls[s_nav_depth - 1]);
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        int list_y = UI_CONTENT_TOP + 40;
        size_t start = (size_t)s_entry_page * FEEDS_PER_PAGE;
        for (int row = 0; row < FEEDS_PER_PAGE && start + (size_t)row < s_parsed.count; ++row) {
            if (!ui_rect_hit(ui_grid_rect(0, 1, row, list_y, 112), x, y)) continue;
            size_t idx = start + (size_t)row;
            const feed_item_t *it = &s_parsed.items[idx];
            if (it->acq_url[0]) {
                s_selected_entry = (int)idx;
                s_last_saved_path[0] = '\0';
                s_notice[0] = '\0';
                s_view = FEEDS_VIEW_DETAIL;
                return APP_REDRAW_PAGE;
            }
            if (it->is_nav && it->link[0]) {
                if (s_nav_depth < FEEDS_NAV_DEPTH) {
                    snprintf(s_nav_urls[s_nav_depth++], sizeof(s_nav_urls[0]), "%s", it->link);
                } else {
                    snprintf(s_nav_urls[FEEDS_NAV_DEPTH - 1], sizeof(s_nav_urls[0]), "%s", it->link);
                }
                (void)start_xml_fetch(s_nav_urls[s_nav_depth - 1]);
                return APP_REDRAW_PAGE;
            }
            (void)open_article_entry(it);
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }

    if (s_view == FEEDS_VIEW_DETAIL) {
        if (ui_rect_hit(ui_bar_rect(0, 1), x, y)) { navigate_back(); return APP_REDRAW_PAGE; }
        if (s_selected_entry < 0 || (size_t)s_selected_entry >= s_parsed.count) return APP_REDRAW_NONE;
        const feed_item_t *it = &s_parsed.items[s_selected_entry];
        int by = 720;
        if (ui_rect_hit(ui_row_rect(0, 1, by, UI_BTN_H), x, y)) {
            (void)start_book_download(it, true);
            return APP_REDRAW_PAGE;
        }
        if (ui_rect_hit(ui_row_rect(0, 1, by + 96, UI_BTN_H), x, y)) {
            (void)start_book_download(it, false);
            return APP_REDRAW_PAGE;
        }
        if (s_last_saved_path[0] && ui_rect_hit(ui_row_rect(0, 1, by + 192, UI_BTN_H), x, y)) {
            app_book_queue_open(s_last_saved_path);
            ctx->request_app = &app_book;
            return APP_REDRAW_NONE;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    if (s_view == FEEDS_VIEW_ARTICLE) {
        size_t total = book_layout_page_count();
        if (key == UI_KEY_1) {
            ctx->key_handled = true;
            if (s_article_page > 0) { s_article_page--; return APP_REDRAW_PAGE; }
            navigate_back();
            return APP_REDRAW_PAGE;
        }
        if (key == UI_KEY_3) {
            ctx->key_handled = true;
            if (s_article_page + 1 < total) { s_article_page++; return APP_REDRAW_PAGE; }
            return APP_REDRAW_NONE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == FEEDS_VIEW_ENTRIES) {
        int pages = (int)((s_parsed.count + FEEDS_PER_PAGE - 1) / FEEDS_PER_PAGE);
        if (pages <= 0) pages = 1;
        if (key == UI_KEY_1) {
            ctx->key_handled = true;
            if (s_entry_page > 0) { s_entry_page--; return APP_REDRAW_PAGE; }
            navigate_back();
            return APP_REDRAW_PAGE;
        }
        if (key == UI_KEY_3) {
            ctx->key_handled = true;
            if (s_entry_page + 1 < pages) { s_entry_page++; return APP_REDRAW_PAGE; }
            if (s_parsed.next_url[0]) {
                if (s_nav_depth < FEEDS_NAV_DEPTH) {
                    snprintf(s_nav_urls[s_nav_depth++], sizeof(s_nav_urls[0]), "%s", s_parsed.next_url);
                } else {
                    snprintf(s_nav_urls[FEEDS_NAV_DEPTH - 1], sizeof(s_nav_urls[0]), "%s", s_parsed.next_url);
                }
                (void)start_xml_fetch(s_nav_urls[s_nav_depth - 1]);
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        return APP_REDRAW_NONE;
    }
    if (s_view == FEEDS_VIEW_SOURCES) {
        int pages = (int)((s_source_count + FEEDS_PER_PAGE - 1) / FEEDS_PER_PAGE);
        if (pages <= 0) pages = 1;
        if (key == UI_KEY_1 && s_source_page > 0) {
            ctx->key_handled = true;
            s_source_page--;
            return APP_REDRAW_PAGE;
        }
        if (key == UI_KEY_3 && s_source_page + 1 < pages) {
            ctx->key_handled = true;
            s_source_page++;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_1) {
        ctx->key_handled = true;
        navigate_back();
        return APP_REDRAW_PAGE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key_long(app_ctx_t *ctx, int key) {
    if (key != UI_KEY_1) return APP_REDRAW_NONE;
    if (s_view == FEEDS_VIEW_SOURCES) {
        ctx->request_return = true;
        ctx->key_handled = true;
        return APP_REDRAW_NONE;
    }
    ctx->key_handled = true;
    navigate_back();
    return APP_REDRAW_PAGE;
}

static void key_hints(app_ctx_t *ctx, ui_key_hints_t *out) {
    (void)ctx;
    if (s_view == FEEDS_VIEW_LOADING) {
        out->left = "取消";
        out->right = NULL;
    } else if (s_view == FEEDS_VIEW_DETAIL) {
        out->left = "返回";
        out->right = NULL;
    } else if (s_view == FEEDS_VIEW_ARTICLE) {
        size_t total = book_layout_page_count();
        out->left = s_article_page > 0 ? "上一页" : "返回";
        out->right = (s_article_page + 1 < total) ? "下一页" : NULL;
    } else if (s_view == FEEDS_VIEW_ENTRIES) {
        int pages = (int)((s_parsed.count + FEEDS_PER_PAGE - 1) / FEEDS_PER_PAGE);
        if (pages <= 0) pages = 1;
        out->left = s_entry_page > 0 ? "上一页" : "返回";
        out->right = (s_entry_page + 1 < pages || s_parsed.next_url[0]) ? "下一页" : NULL;
    } else {
        int pages = (int)((s_source_count + FEEDS_PER_PAGE - 1) / FEEDS_PER_PAGE);
        if (pages <= 0) pages = 1;
        out->left = s_source_page > 0 ? "上一页" : "返回";
        out->right = (s_source_page + 1 < pages) ? "下一页" : NULL;
    }
}

const app_desc_t app_feeds = {
    .title = "订阅书库",
    .detail = "OPDS / RSS 在线书库",
    .owns_keys = true,
    .blocks_idle_sleep = true,
    .render = render,
    .on_enter = on_enter,
    .on_exit = feeds_on_exit,
    .on_media_lost = feeds_on_media_lost,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_key_long = on_key_long,
    .key_hints = key_hints,
    .on_tick = on_tick,
};
