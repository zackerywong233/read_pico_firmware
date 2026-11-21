/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 热点与已有WiFi、HTTP接收和临时文件提交；容量策略由页面注入。
 * AP/STA networking, HTTP reception and file commit; page injects capacity policy.
 * 决策变更：书籍上传和管理同时接受 TXT、EPUB、Markdown 与 CBZ。
 * Frozen: Book upload and management accept TXT, EPUB, Markdown and CBZ.
 * 冻结：传书网页可下载本书书签与摘录的 Markdown 导出；UTF-8 文件名原样保留。
 * Frozen: The transfer page can download Markdown exports of a book's bookmarks and excerpts; UTF-8 names remain unchanged.
 * 冻结：UTF-8 名字不转写；先验限额，字体提交前校验，失败清理临时文件。
 * Frozen: Preserve UTF-8 names; check limits first, validate fonts before commit and remove failed parts.
 * 冻结：有完整目标文件时，同名 rename-backup 是已提交文件的过期备份，可安全清理；目标缺失时必须先恢复备份。
 * Frozen: When a complete destination exists, its same-name rename-backup is stale and may be removed; restore the backup first when the destination is missing.
 * 冻结：热点网页或停服设备触屏可管理最多五组网络，不自动切模式；凭据存于私有NVS blob，状态不含密码。
 * Frozen: AP webpage or stopped-service device UI manages up to five networks without switching modes; private NVS credentials never enter status.
 * 决策变更：网页阅读页可通过同网 HTTP 剪藏；每次开机首次启动服务生成随机口令，停启服务期间保持不变。
 * Decision change: the web reader accepts same-LAN HTTP clips; the first service start per boot creates a token that survives stop/start.
 */
#include "read_pico_search.h"
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include "transfer_font.h"

/* ---- 请求与存储 / Requests and storage ---- */
static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_name_type(const char *encoded, char *out, size_t cap, bool font) {
    if (!cap) return false;
    size_t n = 0;
    while (*encoded) {
        unsigned char c = (unsigned char)*encoded++;
        if (c == '%') {
            if (!encoded[0] || !encoded[1]) return false;
            int a = hex_value(encoded[0]), b = hex_value(encoded[1]);
            if (a < 0 || b < 0) return false;
            c = (unsigned char)((a << 4) | b); encoded += 2;
        }
        if (n + 1 >= cap || c < 32 || c == 127 || strchr("/\\:*?\"<>|", c)) return false;
        out[n++] = (char)c;
    }
    out[n] = 0;
    if (!n || out[0] == '.' || out[0] == ' ' || out[n - 1] == ' ' || strstr(out, "..")) return false;
    const char *ext = strrchr(out, '.');
    if (!ext || (font ? strcasecmp(ext, ".ttf") : (strcasecmp(ext, ".txt") && strcasecmp(ext, ".epub") && strcasecmp(ext, ".md") && strcasecmp(ext, ".cbz")))) return false;
    // 拒绝非规范 UTF-8、代理项与越界码点。/ Reject noncanonical UTF-8, surrogates and out-of-range code points.
    for (size_t i = 0; i < n;) {
        uint32_t cp; unsigned more; unsigned char c = (unsigned char)out[i++];
        if (c < 128) continue;
        if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; more = 1; }
        else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; more = 2; }
        else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; more = 3; }
        else return false;
        unsigned width = more;
        while (more--) {
            if (i == n || ((unsigned char)out[i] & 0xc0) != 0x80) return false;
            cp = (cp << 6) | ((unsigned char)out[i++] & 63);
        }
        if ((width == 2 && cp < 0x800) || (width == 3 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff) return false;
    }
    return true;
}

static bool decode_name_limit(const char *encoded, char *out, size_t cap) { return decode_name_type(encoded, out, cap, false); }
static bool decode_name(const char *encoded, char out[121]) { return decode_name_limit(encoded, out, 121); }
static bool decode_font_name(const char *encoded, char out[121]) { return decode_name_type(encoded, out, 121, true); }

static bool raw_book_name(const char *entry, char *name, size_t cap) {
    size_t len = strlen(entry);
    if (!len || len >= cap || len > 255) return false;
    static const char digits[] = "0123456789ABCDEF";
    char encoded[766];
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)entry[i];
        encoded[i * 3] = '%'; encoded[i * 3 + 1] = digits[c >> 4]; encoded[i * 3 + 2] = digits[c & 15];
    }
    encoded[len * 3] = 0;
    return decode_name_limit(encoded, name, cap);
}

// 两个存储根都是FAT；变更使用目录真实拼写，不明别名拒绝操作，避免漏清阅读记录。
// Both storage roots use FAT; use directory spelling and reject unknown aliases to protect progress identity.
static bool ascii_name_equal(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return *a == *b;
}

static int resolve_mutation_path(const char *root, const char *name,
                                 char *path, size_t cap) {
    if (snprintf(path, cap, "%s/%s", root, name) >= (int)cap) return 400;
    // 主文件优先；只有备份的中断状态也保留原始名字。/ Prefer the main file; preserve spelling when only a backup remains.
    for (int pass = 0; pass < 2; ++pass) {
        DIR *dir = opendir(root);
        if (!dir) return 507;
        bool found = false;
        int result = 0;
        struct dirent *entry;
        while (errno = 0, (entry = readdir(dir)) != NULL) {
            const char *suffix = ".rename-backup";
            size_t len = strlen(entry->d_name), tail = pass ? strlen(suffix) : 0;
            if (len <= tail || len - tail > 255 || (pass && strcmp(entry->d_name + len - tail, suffix))) continue;
            char actual[256]; memcpy(actual, entry->d_name, len - tail); actual[len - tail] = 0;
            if (!ascii_name_equal(actual, name)) continue;
            if (found || snprintf(path, cap, "%s/%s", root, actual) >= (int)cap) { result = 400; break; }
            found = true;
        }
        if (!entry && errno) result = 507;
        if (closedir(dir)) result = 507;
        if (result || found) return result;
        if (!pass) {
            struct stat st;
            if (!stat(path, &st)) return 400;
            if (errno != ENOENT) return 507;
        }
    }
    struct stat st;
    if (!stat(path, &st)) return 400;
    if (errno != ENOENT) return 507;
    if (strlen(name) + strlen(".rename-backup") <= 255) {
        char backup[480];
        if (snprintf(backup, sizeof(backup), "%s.rename-backup", path) >= (int)sizeof(backup)) return 400;
        if (!stat(backup, &st)) return 400;
        if (errno != ENOENT) return 507;
    }
    return 0;
}

static int check_length(size_t total, size_t limit, uint64_t available) {
    if (!total) return 400;
    if (limit && total > limit) return 413;
    if (total > available) return 507;
    return 0;
}

static bool temporary_basename(const char *entry, const char *suffix, char name[121]) {
    size_t len = strlen(entry), tail = strlen(suffix);
    if (len <= tail || len - tail > 120 || strcmp(entry + len - tail, suffix)) return false;
    // 目录项已解码；重新百分号编码后复用校验，保留文件名里的字面百分号。
    // Directory entries are already decoded; encode before validation to preserve literal percent signs.
    static const char digits[] = "0123456789ABCDEF";
    char encoded[361];
    for (size_t i = 0; i < len - tail; ++i) {
        unsigned char c = (unsigned char)entry[i];
        encoded[i * 3] = '%'; encoded[i * 3 + 1] = digits[c >> 4]; encoded[i * 3 + 2] = digits[c & 15];
    }
    encoded[(len - tail) * 3] = 0;
    return decode_name(encoded, name) || decode_font_name(encoded, name);
}

static int cleanup_interrupted(const char *root, unsigned *removed, unsigned *restored) {
    *removed = *restored = 0;
    // 先恢复缺失目标的旧书；目标完整时才删过期备份，最后清未提交片段；不递归。
    // Restore old books with missing destinations, remove backups only beside complete files, then clear uncommitted parts; never recurse.
    for (int pass = 0; pass < 2; ++pass) {
        DIR *dir = opendir(root);
        if (!dir) return -1;
        int result = 0;
        struct dirent *entry;
        while (errno = 0, (entry = readdir(dir)) != NULL) {
            char name[121], path[320], target[288];
            if (!temporary_basename(entry->d_name, pass ? ".part" : ".rename-backup", name)) continue;
            if (snprintf(path, sizeof(path), "%s/%s", root, entry->d_name) >= (int)sizeof(path) ||
                snprintf(target, sizeof(target), "%s/%s", root, name) >= (int)sizeof(target)) { result = -1; break; }
            struct stat st;
            if (stat(path, &st)) { result = -1; break; }
            if (!S_ISREG(st.st_mode)) continue;
            if (!pass) {
                if (!stat(target, &st)) {
                    if (!S_ISREG(st.st_mode) || remove(path)) { result = -1; break; }
                    ++*removed;
                } else {
                    if (errno != ENOENT || rename(path, target)) { result = -1; break; }
                    ++*restored;
                }
            } else {
                if (remove(path)) { result = -1; break; }
                ++*removed;
            }
        }
        if (!entry && errno) result = -1;
        if (closedir(dir)) result = -1;
        if (result) return result;
    }
    return 0;
}

static int commit_file(const char *part, const char *path) {
    if (rename(part, path) == 0) return 0;
    // FatFs 不支持直接覆盖；保留旧书直至新文件成功就位。/ FatFs cannot replace directly; retain the old book until commit.
    if (errno != EEXIST && errno != EACCES) return -1;
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return -1;
    char backup[320];
    if (snprintf(backup, sizeof(backup), "%s.rename-backup", path) >= (int)sizeof(backup)) return -1;
    if (!stat(backup, &st)) {
        if (!S_ISREG(st.st_mode) || remove(backup)) return -1;
    } else if (errno != ENOENT) return -1;
    if (rename(path, backup)) return -1;
    if (rename(part, path)) { rename(backup, path); return -1; }
    // 新书已提交；备份删除失败只能报告警告，不能跳过进度失效回调。
    // The new book is committed; failed backup removal is a warning and must not skip progress invalidation.
    return remove(backup) ? 1 : 0;
}

typedef int (*receive_cb_t)(void *, char *, size_t);
typedef void (*progress_cb_t)(size_t);
enum { TRANSFER_COMMITTED_BACKUP_RETAINED = 299 };
static int receive_file(const char *path, const char *part, size_t total, char *buf,
                        size_t cap, receive_cb_t recv, void *ctx, progress_cb_t progress) {
    FILE *f = fopen(part, "wb");
    if (!f) return 507;
    int error = 0;
    size_t done = 0;
    while (done < total) {
        size_t n = total - done < cap ? total - done : cap;
        int got = recv(ctx, buf, n);
        if (got <= 0 || (size_t)got > n) { error = 408; break; }
        if (fwrite(buf, 1, (size_t)got, f) != (size_t)got) { error = 507; break; }
        done += (size_t)got;
        if (progress) progress(done);
    }
    if (fclose(f) != 0) error = 507;
    const char *ext = strrchr(path, '.');
    if (!error && ext && !strcasecmp(ext, ".ttf") && !valid_ttf(part)) error = 422;
    if (!error) {
        int committed = commit_file(part, path);
        if (committed < 0) error = 507;
        else if (committed > 0) return TRANSFER_COMMITTED_BACKUP_RETAINED;
    }
    if (error) remove(part);
    return error;
}

typedef int (*file_changed_cb_t)(const char *);
typedef struct {
    int status;
    bool changed, progress_cleanup_failed, storage_cleanup_failed;
} file_result_t;

static file_result_t changed_result(const char *path, file_changed_cb_t callback) {
    bool failed = callback && callback(path) != 0;
    return (file_result_t){.status = failed ? 500 : 200, .changed = true, .progress_cleanup_failed = failed};
}

static void record_file_change(unsigned *count, file_result_t result, bool retry) {
    if (result.changed || (retry && result.status == 200)) ++*count;
}

static int destination_status(const char *path, bool overwrite) {
    struct stat st;
    if (!stat(path, &st)) return !S_ISREG(st.st_mode) ? 400 : overwrite ? 0 : 409;
    return errno == ENOENT ? 0 : 507;
}

static file_result_t upload_managed(const char *path, const char *part, size_t total, size_t limit,
        uint64_t available, bool overwrite, char *buf, size_t cap, receive_cb_t recv, void *ctx,
        progress_cb_t progress, file_changed_cb_t changed) {
    int status = destination_status(path, overwrite);
    if (!status) status = check_length(total, limit, available);
    if (!status) status = receive_file(path, part, total, buf, cap, recv, ctx, progress);
    if (status && status != TRANSFER_COMMITTED_BACKUP_RETAINED) return (file_result_t){.status = status};
    file_result_t result = changed_result(path, changed);
    result.storage_cleanup_failed = status == TRANSFER_COMMITTED_BACKUP_RETAINED;
    return result;
}

static file_result_t delete_managed(const char *path, file_changed_cb_t changed) {
    struct stat st;
    bool exists = stat(path, &st) == 0;
    if (!exists && errno != ENOENT) return (file_result_t){.status = 507};
    if (exists && !S_ISREG(st.st_mode)) return (file_result_t){.status = 400};
    char backup[480];
    const char *name = strrchr(path, '/'); name = name ? name + 1 : path;
    // 后缀会超过文件名上限时不探测备份，避免长书名删除被ENAMETOOLONG阻止。
    // Skip impossible backup names so ENAMETOOLONG cannot prevent deleting a long book name.
    if (strlen(name) + strlen(".rename-backup") <= 255) {
        if (snprintf(backup, sizeof(backup), "%s.rename-backup", path) >= (int)sizeof(backup)) return (file_result_t){.status = 400};
        bool backed_up = stat(backup, &st) == 0;
        if (!backed_up && errno != ENOENT) return (file_result_t){.status = 507};
        if (backed_up) {
            if (!S_ISREG(st.st_mode)) return (file_result_t){.status = 400};
            if (exists) {
                if (remove(backup)) return (file_result_t){.status = 507};
            } else {
                if (rename(backup, path)) return (file_result_t){.status = 507};
                exists = true;
            }
        }
    }
    if (!exists) return (file_result_t){.status = 404};
    if (remove(path)) return (file_result_t){.status = 507};
    return changed_result(path, changed);
}

#define BOOK_LIST_PAGE_SIZE 16
typedef struct { char name[256]; uint64_t size; bool has_notes; } transfer_book_entry_t;
typedef struct {
    transfer_book_entry_t items[BOOK_LIST_PAGE_SIZE];
    size_t count, total;
} transfer_book_page_t;

static bool name_contains(const char *name, const char *query) {
    return read_pico_search_match(name, query);
}

static uint32_t notes_hash(const char *path) {
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) hash = (hash ^ *p) * UINT32_C(16777619);
    return hash;
}

static bool has_book_notes(const char *root, const char *name, uint64_t size) {
    char full[448], sidecar[512];
    if (snprintf(full, sizeof(full), "%s/%s", root, name) >= (int)sizeof(full) ||
        snprintf(sidecar, sizeof(sidecar), "%s/.pico_notes/n_%08" PRIx32 ".bin", root, notes_hash(full)) >= (int)sizeof(sidecar)) return false;
    struct stat st;
    if (stat(sidecar, &st) != 0 || st.st_size < 0) return false;
    FILE *file = fopen(sidecar, "rb");
    if (!file) return false;
    uint8_t header[12];
    bool valid = fread(header, 1, sizeof(header), file) == sizeof(header) && !memcmp(header, "RBNF", 4) &&
                 (header[4] == 1 || header[4] == 2);
    size_t path_len = valid ? (size_t)header[5] | ((size_t)header[6] << 8) : 0;
    uint32_t file_size = valid ? (uint32_t)header[8] | ((uint32_t)header[9] << 8) |
                                 ((uint32_t)header[10] << 16) | ((uint32_t)header[11] << 24) : 0;
    char owner[448];
    valid = valid && path_len >= 2 && path_len <= sizeof(owner) && st.st_size > (off_t)(12 + path_len) &&
            fread(owner, 1, path_len, file) == path_len && owner[path_len - 1] == 0 &&
            !strcmp(owner, full) && file_size == size;
    fclose(file);
    return valid;
}

static int list_books(const char *root, const char *query, size_t page, transfer_book_page_t *out) {
    memset(out, 0, sizeof(*out));
    if (page > SIZE_MAX / BOOK_LIST_PAGE_SIZE) return 400;
    DIR *dir = opendir(root);
    if (!dir) return 507;
    int status = 0;
    struct dirent *entry;
    while (errno = 0, (entry = readdir(dir)) != NULL) {
        char name[256], path[448];
        if (!raw_book_name(entry->d_name, name, sizeof(name)) || !name_contains(name, query)) continue;
        if (snprintf(path, sizeof(path), "%s/%s", root, name) >= (int)sizeof(path)) { status = 507; break; }
        struct stat st;
        if (stat(path, &st)) { status = 507; break; }
        if (!S_ISREG(st.st_mode) || st.st_size < 0) continue;
        size_t index = out->total++;
        if (index >= page * BOOK_LIST_PAGE_SIZE && out->count < BOOK_LIST_PAGE_SIZE) {
            transfer_book_entry_t *item = &out->items[out->count++];
            strcpy(item->name, name); item->size = (uint64_t)st.st_size;
            item->has_notes = has_book_notes(root, name, item->size);
        }
    }
    if (!entry && errno) status = 507;
    if (closedir(dir)) status = 507;
    return status;
}

#ifndef READ_PICO_TRANSFER_HOST_TEST
#include "read_pico_transfer.h"
#include "transfer_policy.h"
#include "transfer_credentials_store.h"
#include "agent_config_store.h"
#include "clip_text.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"
#include "ota_policy.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "mdns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static read_pico_transfer_status_t s_status;
static read_pico_transfer_cfg_t s_cfg;
static char s_root[160];
static char s_font_root[160];
static httpd_handle_t s_http;
static esp_netif_t *s_netif;
static esp_event_handler_instance_t s_events, s_ip_events;
static bool s_wifi, s_started, s_loop_owned;
static bool s_ota_loop_owned, s_ota_wifi;
static esp_netif_t *s_ota_netif;
static EventGroupHandle_t s_ota_events;
static esp_event_handler_instance_t s_ota_wifi_event, s_ota_ip_event;
#define OTA_WIFI_CONNECTED BIT0
#define OTA_WIFI_FAILED BIT1
static char *s_buffer;
static char s_clip_token[33];
static bool s_stopping, s_upload_active;
static bool s_config_busy;
static bool s_mdns;
static uint8_t s_connect_index;
static char s_webopen_url[READ_PICO_WEBOPEN_URL_MAX];

static esp_err_t start_mdns(void) {
    if (s_mdns) return ESP_OK;
    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;
    // Use the final three MAC bytes to keep multiple Read Pico devices on the
    // same LAN from competing for the same mDNS hostname. This also works with
    // the ESP-IDF 6.1 mDNS component, which does not expose the newer hostname
    // change callback API.
    uint8_t mac[6] = {0};
    err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char hostname[24] = {0};
    if (err == ESP_OK) {
        snprintf(hostname, sizeof(hostname), "readpico-%02x%02x%02x", mac[3], mac[4], mac[5]);
        err = mdns_hostname_set(hostname);
    }
    if (err == ESP_OK) err = mdns_instance_name_set("小纸 Pico");
    if (err == ESP_OK) err = mdns_service_add("Read Pico", "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) { mdns_free(); return err; }
    s_mdns = true;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.mdns_host, sizeof(s_status.mdns_host), "%s", hostname);
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI("transfer", "mDNS hostname: %s", hostname);
    return ESP_OK;
}

static void stop_mdns(void) {
    if (!s_mdns) return;
    mdns_free();
    s_mdns = false;
    portENTER_CRITICAL(&s_lock);
    s_status.mdns_host[0] = 0;
    portEXIT_CRITICAL(&s_lock);
}

static uint8_t choose_saved_network(const transfer_credentials_t *saved) {
    uint8_t best_index = saved->recent;
    int8_t best_rssi = INT8_MIN;
    bool loop_owned = false, initialized = false, started = false;
    if (esp_netif_init() != ESP_OK) return best_index;
    esp_err_t err = esp_event_loop_create_default();
    if (err == ESP_OK) loop_owned = true;
    else if (err != ESP_ERR_INVALID_STATE) return best_index;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init) != ESP_OK) goto done;
    initialized = true;
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK || esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_start() != ESP_OK) goto done;
    started = true;
    wifi_scan_config_t scan = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 100, .max = 150}};
    if (esp_wifi_scan_start(&scan, true) != ESP_OK) goto done;
    uint16_t found = 0;
    if (esp_wifi_scan_get_ap_num(&found) != ESP_OK) goto done;
    for (uint16_t i = 0; i < found; ++i) {
        wifi_ap_record_t ap;
        if (esp_wifi_scan_get_ap_record(&ap) != ESP_OK) break;
        for (uint8_t j = 0; j < saved->count; ++j) {
            if (!strcmp((const char *)ap.ssid, saved->entries[j].ssid) && ap.rssi > best_rssi) {
                best_rssi = ap.rssi;
                best_index = j;
            }
        }
    }
done:
    if (started) { esp_wifi_scan_stop(); esp_wifi_clear_ap_list(); (void)esp_wifi_stop(); }
    if (initialized) (void)esp_wifi_deinit();
    if (loop_owned) (void)esp_event_loop_delete_default();
    return best_index;
}
static transfer_connection_t s_connection;
extern const char upload_start[] asm("_binary_upload_html_start");
extern const char upload_end[] asm("_binary_upload_html_end");

static void renew_clip_token(void) {
    if (s_clip_token[0]) return;
    uint8_t raw[16];
    esp_fill_random(raw, sizeof(raw));
    for (size_t i = 0; i < sizeof(raw); ++i)
        snprintf(s_clip_token + i * 2, 3, "%02x", raw[i]);
    memset(raw, 0, sizeof(raw));
}

void read_pico_transfer_get_status(read_pico_transfer_status_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_lock); *out = s_status; portEXIT_CRITICAL(&s_lock);
}

static bool claim_config(void) {
    portENTER_CRITICAL(&s_lock);
    bool available = !s_config_busy;
    if (available) s_config_busy = true;
    portEXIT_CRITICAL(&s_lock);
    return available;
}

bool read_pico_transfer_take_webopen_url(char url[READ_PICO_WEBOPEN_URL_MAX]) {
    if (!url) return false;
    portENTER_CRITICAL(&s_lock);
    bool ready = s_webopen_url[0] != 0;
    if (ready) { memcpy(url, s_webopen_url, sizeof(s_webopen_url)); s_webopen_url[0] = 0; }
    portEXIT_CRITICAL(&s_lock);
    return ready;
}

static void release_config(void) {
    portENTER_CRITICAL(&s_lock); s_config_busy = false; portEXIT_CRITICAL(&s_lock);
}

static void publish_credentials(const transfer_credentials_t *c) {
    portENTER_CRITICAL(&s_lock);
    s_status.wifi_configured = c && c->version == 2 && c->count;
    s_status.wifi_count = s_status.wifi_configured ? c->count : 0;
    memset(s_status.wifi_ssids, 0, sizeof(s_status.wifi_ssids));
    memset(s_status.wifi_ssid, 0, sizeof(s_status.wifi_ssid));
    if (s_status.wifi_configured) {
        for (size_t i = 0; i < c->count; ++i) memcpy(s_status.wifi_ssids[i], c->entries[i].ssid, 33);
        memcpy(s_status.wifi_ssid, c->entries[c->recent].ssid, 33);
    }
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t read_pico_transfer_get_saved_wifi(char ssid[33], bool *configured) {
    if (!ssid || !configured) return ESP_ERR_INVALID_ARG;
    ssid[0] = 0; *configured = false;
    if (!claim_config()) return ESP_ERR_INVALID_STATE;
    transfer_credentials_t saved;
    esp_err_t err = load_credentials(&saved);
    if (err == ESP_OK && saved.version == 2 && saved.count) { memcpy(ssid, saved.entries[saved.recent].ssid, 33); *configured = true; }
    clear_secret(&saved, sizeof(saved)); release_config();
    return err;
}

esp_err_t read_pico_transfer_get_saved_wifi_list(char ssids[5][33], size_t *count) {
    if (!ssids || !count) return ESP_ERR_INVALID_ARG;
    memset(ssids, 0, 5 * 33); *count = 0;
    if (!claim_config()) return ESP_ERR_INVALID_STATE;
    transfer_credentials_t saved;
    esp_err_t err = load_credentials(&saved);
    if (err == ESP_OK && saved.version == 2) {
        *count = saved.count;
        for (size_t i = 0; i < saved.count; ++i) memcpy(ssids[i], saved.entries[i].ssid, 33);
    }
    clear_secret(&saved, sizeof(saved)); release_config();
    return err;
}

esp_err_t read_pico_transfer_save_wifi(const char *ssid, const char *password) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (s_wifi || s_ota_wifi || s_http || s_netif || status.state != READ_PICO_TRANSFER_STOPPED) return ESP_ERR_INVALID_STATE;
    transfer_credentials_t next;
    if (!credentials_from_text(ssid, password, &next)) return ESP_ERR_INVALID_ARG;
    if (!claim_config()) { clear_secret(&next, sizeof(next)); return ESP_ERR_INVALID_STATE; }
    transfer_credentials_t current = {0};
    esp_err_t err = load_credentials(&current);
    if (err == ESP_OK) {
        size_t index = 0;
        while (index < current.count && strcmp(current.entries[index].ssid, next.entries[0].ssid)) ++index;
        if (index == current.count && current.count == 5) err = ESP_ERR_NO_MEM;
        else {
            if (index == current.count) ++current.count;
            memcpy(&current.entries[index], &next.entries[0], sizeof(next.entries[0]));
            current.version = 2;
            if (!current.count) current.count = 1;
            current.recent = (uint8_t)index;
            err = store_credentials(&current);
        }
    }
    if (err == ESP_OK) {
        publish_credentials(&current);
        ESP_LOGI("transfer", "wifi saved configured=1");
    }
    clear_secret(&current, sizeof(current)); clear_secret(&next, sizeof(next)); release_config(); return err;
}

static void ota_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (!s_ota_events) return;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_ota_events, OTA_WIFI_CONNECTED);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) xEventGroupSetBits(s_ota_events, OTA_WIFI_FAILED);
}

esp_err_t read_pico_transfer_ota_wifi_start(uint32_t timeout_ms) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (s_wifi || s_netif || s_ota_wifi || status.state != READ_PICO_TRANSFER_STOPPED) return ESP_ERR_INVALID_STATE;
    transfer_credentials_t saved = {0};
    wifi_config_t config = {0};
    esp_err_t err = load_credentials(&saved);
    if (err != ESP_OK || saved.version != 2 || !saved.count) { clear_secret(&saved, sizeof(saved)); return ESP_ERR_NOT_FOUND; }
    uint8_t index = saved.recent < saved.count ? saved.recent : 0;
    transfer_wifi_entry_t *entry = &saved.entries[index];
    memcpy(config.sta.ssid, entry->ssid, strlen(entry->ssid));
    memcpy(config.sta.password, entry->password, strlen(entry->password));
    config.sta.threshold.authmode = entry->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    clear_secret(&saved, sizeof(saved));
    err = esp_netif_init(); if (err != ESP_OK) goto fail;
    err = esp_event_loop_create_default();
    if (err == ESP_OK) s_ota_loop_owned = true;
    else if (err != ESP_ERR_INVALID_STATE) goto fail;
    s_ota_netif = esp_netif_create_default_wifi_sta();
    if (!s_ota_netif) { err = ESP_ERR_NO_MEM; goto fail; }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init); if (err != ESP_OK) goto fail;
    s_ota_wifi = true;
    s_ota_events = xEventGroupCreate();
    if (!s_ota_events) { err = ESP_ERR_NO_MEM; goto fail; }
    err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, ota_wifi_event, NULL, &s_ota_wifi_event);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ota_wifi_event, NULL, &s_ota_ip_event);
    if (err != ESP_OK) goto fail;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM); if (err != ESP_OK) goto fail;
    err = esp_wifi_set_mode(WIFI_MODE_STA); if (err != ESP_OK) goto fail;
    err = esp_wifi_set_config(WIFI_IF_STA, &config); if (err != ESP_OK) goto fail;
    clear_secret(&config, sizeof(config));
    err = esp_wifi_start(); if (err != ESP_OK) goto fail;
    err = esp_wifi_connect(); if (err != ESP_OK) goto fail;
    EventBits_t bits = xEventGroupWaitBits(s_ota_events, OTA_WIFI_CONNECTED | OTA_WIFI_FAILED,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if (!(bits & OTA_WIFI_CONNECTED)) { err = (bits & OTA_WIFI_FAILED) ? ESP_FAIL : ESP_ERR_TIMEOUT; goto fail; }
    return ESP_OK;
fail:
    clear_secret(&config, sizeof(config)); clear_secret(&saved, sizeof(saved));
    read_pico_transfer_ota_wifi_stop();
    return err;
}

void read_pico_transfer_ota_wifi_stop(void) {
    if (s_ota_ip_event) { esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ota_ip_event); s_ota_ip_event = NULL; }
    if (s_ota_wifi_event) { esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, s_ota_wifi_event); s_ota_wifi_event = NULL; }
    if (s_ota_events) { vEventGroupDelete(s_ota_events); s_ota_events = NULL; }
    if (s_ota_wifi) { (void)esp_wifi_disconnect(); (void)esp_wifi_stop(); (void)esp_wifi_deinit(); s_ota_wifi = false; }
    if (s_ota_netif) { esp_netif_destroy_default_wifi(s_ota_netif); s_ota_netif = NULL; }
    if (s_ota_loop_owned) { (void)esp_event_loop_delete_default(); s_ota_loop_owned = false; }
}

esp_err_t read_pico_transfer_scan_wifi(read_pico_transfer_network_t out[READ_PICO_TRANSFER_SCAN_MAX], size_t *count) {
    if (!out || !count) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out) * READ_PICO_TRANSFER_SCAN_MAX); *count = 0;
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (s_wifi || s_ota_wifi || s_http || s_netif || status.state != READ_PICO_TRANSFER_STOPPED) return ESP_ERR_INVALID_STATE;
    bool loop_owned = false, initialized = false, started = false;
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return err;
    err = esp_event_loop_create_default();
    if (err == ESP_OK) loop_owned = true;
    else if (err != ESP_ERR_INVALID_STATE) return err;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init); if (err != ESP_OK) goto cleanup;
    initialized = true;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM); if (err != ESP_OK) goto cleanup;
    err = esp_wifi_set_mode(WIFI_MODE_STA); if (err != ESP_OK) goto cleanup;
    // 仅驱动扫描，不创建STA网络接口或注册自动连接回调。/ Driver-only scan: no STA netif or automatic-connect callback.
    err = esp_wifi_start(); if (err != ESP_OK) goto cleanup;
    started = true;
    wifi_scan_config_t scan = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 100, .max = 200}};
    err = esp_wifi_scan_start(&scan, true); if (err != ESP_OK) goto cleanup;
    uint16_t found = 0;
    err = esp_wifi_scan_get_ap_num(&found); if (err != ESP_OK) goto cleanup;
    for (uint16_t i = 0; i < found; ++i) {
        wifi_ap_record_t ap;
        err = esp_wifi_scan_get_ap_record(&ap); if (err != ESP_OK) goto cleanup;
        read_pico_transfer_network_t item = {.rssi = ap.rssi, .authmode = (uint8_t)ap.authmode,
            .requires_password = ap.authmode != WIFI_AUTH_OPEN && ap.authmode != WIFI_AUTH_OWE};
        memcpy(item.ssid, ap.ssid, sizeof(item.ssid) - 1);
        item.supported = ap.authmode == WIFI_AUTH_OPEN || ap.authmode == WIFI_AUTH_WPA2_PSK ||
            ap.authmode == WIFI_AUTH_WPA_WPA2_PSK || ap.authmode == WIFI_AUTH_WPA3_PSK ||
            ap.authmode == WIFI_AUTH_WPA2_WPA3_PSK;
        scan_offer(out, count, &item);
    }
cleanup:
    if (started) { esp_wifi_scan_stop(); esp_wifi_clear_ap_list(); }
    if (started) {
        esp_err_t stop_err = esp_wifi_stop();
        if (err == ESP_OK && stop_err != ESP_OK) err = stop_err;
    }
    if (initialized) {
        esp_err_t deinit_err = esp_wifi_deinit();
        if (err == ESP_OK && deinit_err != ESP_OK) err = deinit_err;
    }
    if (loop_owned) {
        esp_err_t loop_err = esp_event_loop_delete_default();
        if (err == ESP_OK && loop_err != ESP_OK) err = loop_err;
    }
    if (err != ESP_OK) { memset(out, 0, sizeof(*out) * READ_PICO_TRANSFER_SCAN_MAX); *count = 0; }
    ESP_LOGI("transfer", "wifi scan result=%s count=%u", esp_err_to_name(err), (unsigned)*count);
    return err;
}

esp_err_t read_pico_transfer_forget_wifi(void) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (status.state == READ_PICO_TRANSFER_UPLOADING ||
        (status.state != READ_PICO_TRANSFER_STOPPED && status.mode == READ_PICO_TRANSFER_MODE_STA)) return ESP_ERR_INVALID_STATE;
    if (!claim_config()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = store_credentials(NULL);
    if (err == ESP_OK) {
        publish_credentials(NULL);
        ESP_LOGI("transfer", "wifi saved configured=0");
    }
    release_config(); return err;
}

esp_err_t read_pico_transfer_forget_wifi_ssid(const char *ssid) {
    if (!ssid || !ssid[0] || strnlen(ssid, 33) > 32) return ESP_ERR_INVALID_ARG;
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (status.state == READ_PICO_TRANSFER_UPLOADING ||
        (status.state != READ_PICO_TRANSFER_STOPPED && status.mode == READ_PICO_TRANSFER_MODE_STA)) return ESP_ERR_INVALID_STATE;
    if (!claim_config()) return ESP_ERR_INVALID_STATE;
    transfer_credentials_t current = {0};
    esp_err_t err = load_credentials(&current);
    size_t index = 0;
    if (err == ESP_OK) {
        while (index < current.count && strcmp(current.entries[index].ssid, ssid)) ++index;
        if (index == current.count) err = ESP_ERR_NOT_FOUND;
        else if (current.count == 1) {
            err = store_credentials(NULL);
            if (err == ESP_OK) current.count = 0;
        }
        else {
            for (size_t i = index; i + 1 < current.count; ++i) current.entries[i] = current.entries[i + 1];
            --current.count;
            clear_secret(&current.entries[current.count], sizeof(current.entries[current.count]));
            if (current.recent == index) current.recent = 0;
            else if (current.recent > index) --current.recent;
            err = store_credentials(&current);
        }
    }
    if (err == ESP_OK) publish_credentials(current.count ? &current : NULL);
    clear_secret(&current, sizeof(current)); release_config(); return err;
}

static void set_error(esp_err_t err) {
    portENTER_CRITICAL(&s_lock);
    if (s_status.mode == READ_PICO_TRANSFER_MODE_STA && s_connection.failed) err = ESP_ERR_TIMEOUT;
    s_status.last_error = err;
    s_status.state = err == ESP_OK ? (s_status.network_ready ? READ_PICO_TRANSFER_READY : READ_PICO_TRANSFER_STARTING) : READ_PICO_TRANSFER_ERROR;
    portEXIT_CRITICAL(&s_lock);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    int64_t now = esp_timer_get_time() / 1000;
    bool disconnected = false;
    portENTER_CRITICAL(&s_lock);
    if (!s_stopping && base == WIFI_EVENT) {
        if (id == WIFI_EVENT_AP_STACONNECTED) s_status.sta_count++;
        if (id == WIFI_EVENT_AP_STADISCONNECTED && s_status.sta_count) s_status.sta_count--;
        if (s_status.mode == READ_PICO_TRANSFER_MODE_STA && id == WIFI_EVENT_STA_DISCONNECTED) {
            disconnected = true;
            connection_lost(&s_connection, now);
            s_status.network_ready = false; s_status.url[0] = 0;
            if (s_status.state != READ_PICO_TRANSFER_UPLOADING && !s_connection.failed)
                s_status.state = READ_PICO_TRANSFER_STARTING;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (disconnected) {
        const wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW("transfer", "sta disconnected reason=%u", event ? (unsigned)event->reason : 0);
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base;
    if (id == IP_EVENT_STA_LOST_IP) {
        stop_mdns();
        int64_t now = esp_timer_get_time() / 1000;
        portENTER_CRITICAL(&s_lock);
        bool reconnect = !s_stopping && !s_connection.failed;
        if (reconnect) {
            connection_lost(&s_connection, now);
            s_status.network_ready = false; s_status.url[0] = 0;
            if (s_status.state != READ_PICO_TRANSFER_UPLOADING) s_status.state = READ_PICO_TRANSFER_STARTING;
        }
        portEXIT_CRITICAL(&s_lock);
        if (reconnect) {
            ESP_LOGW("transfer", "sta lost IP");
            esp_wifi_disconnect();
        }
        return;
    }
    if (id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t *event = data;
    if (event->esp_netif != s_netif) return;
    char url[64]; snprintf(url, sizeof(url), "http://" IPSTR, IP2STR(&event->ip_info.ip));
    portENTER_CRITICAL(&s_lock);
    bool accepted = !s_stopping && !s_connection.failed;
    if (accepted) {
        s_connection.online = true; s_connection.pending = false;
        s_status.network_ready = true; strcpy(s_status.url, url);
        s_status.last_error = ESP_OK;
        if (s_status.state != READ_PICO_TRANSFER_UPLOADING) s_status.state = READ_PICO_TRANSFER_READY;
    }
    portEXIT_CRITICAL(&s_lock);
    if (accepted) {
        if (s_cfg.mode == READ_PICO_TRANSFER_MODE_STA && claim_config()) {
            transfer_credentials_t saved = {0};
            if (load_credentials(&saved) == ESP_OK && saved.version == 2 && s_connect_index < saved.count) {
                saved.recent = s_connect_index;
                (void)store_credentials(&saved);
                publish_credentials(&saved);
            }
            clear_secret(&saved, sizeof(saved));
            release_config();
        }
        esp_err_t mdns_err = start_mdns();
        if (mdns_err != ESP_OK) ESP_LOGW("transfer", "mDNS start failed: %s", esp_err_to_name(mdns_err));
        if (s_cfg.sta_ip_cb) s_cfg.sta_ip_cb();
        ESP_LOGI("transfer", "sta ready url=%s", url);
    }
}

void read_pico_transfer_service_poll(void) {
    if (!s_started || s_cfg.mode != READ_PICO_TRANSFER_MODE_STA) return;
    if (s_cfg.service_poll_cb) s_cfg.service_poll_cb();
    int64_t now = esp_timer_get_time() / 1000;
    portENTER_CRITICAL(&s_lock);
    int action = s_stopping ? 0 : connection_poll(&s_connection, now);
    unsigned attempts = s_connection.attempts;
    if (action < 0) {
        s_status.network_ready = false; s_status.url[0] = 0;
        if (s_status.state != READ_PICO_TRANSFER_UPLOADING) s_status.state = READ_PICO_TRANSFER_ERROR;
        s_status.last_error = ESP_ERR_TIMEOUT;
    }
    portEXIT_CRITICAL(&s_lock);
    if (action < 0) {
        ESP_LOGW("transfer", "sta connection timeout attempts=%u", attempts);
        esp_wifi_disconnect();
    }
    if (action == 1) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            connection_lost(&s_connection, now); s_status.last_error = err;
            portEXIT_CRITICAL(&s_lock);
        }
    }
}

static esp_err_t respond_error(httpd_req_t *req, int code) {
    ESP_LOGW("transfer", "request failed status=%d", code);
    const char *status = code == 422 ? "422 Unprocessable Content" : code == 413 ? "413 Content Too Large" : code == 507 ? "507 Insufficient Storage" :
                         code == 408 ? "408 Request Timeout" : code == 409 ? "409 Conflict" :
                         code == 404 ? "404 Not Found" : code == 500 ? "500 Internal Server Error" : "400 Bad Request";
    const char *body = code == 422 ? "{\"error\":\"字体校验失败，需要完整的 TrueType TTF（不支持 OTF/CFF、TTC 或 WOFF）\"}" :
                       code == 413 ? "{\"error\":\"文件超过单文件上限\"}" :
                       code == 507 ? "{\"error\":\"存储空间不足或写入失败\"}" :
                       code == 408 ? "{\"error\":\"连接中断或接收超时，请重试\"}" :
                       code == 409 ? "{\"error\":\"同名文件已存在，请明确选择替换或跳过\",\"conflict\":true}" :
                       code == 404 ? "{\"error\":\"文件不存在\"}" :
                       code == 500 ? "{\"error\":\"操作失败，请重试\"}" : "{\"error\":\"文件名或请求无效：图书支持 TXT/EPUB/MD/CBZ，字体仅 TTF\"}";
    set_error(code == 408 ? ESP_ERR_TIMEOUT : code == 507 ? ESP_FAIL : ESP_ERR_INVALID_ARG);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, body);
    // 未消费的请求体不得被解析为下一个请求。/ Never parse an unread body as the next request.
    return ESP_FAIL;
}

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, upload_start, upload_end - upload_start - 1);
}

static bool copy_json_string(cJSON *json, const char *key, char *out, size_t cap, bool required) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (!item) return !required;
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    size_t len = strlen(item->valuestring);
    if (len >= cap) return false;
    for (const unsigned char *p = (const unsigned char *)item->valuestring; *p; ++p)
        if (*p < 32 || *p == 127) return false;
    memcpy(out, item->valuestring, len + 1);
    return true;
}

static bool valid_agent_config(const read_pico_agent_config_t *config) {
    if (!config || strncmp(config->broker_uri, "mqtts://", 8) || !config->broker_uri[8] || !config->topic[0]) return false;
    for (const unsigned char *p = (const unsigned char *)config->broker_uri; *p; ++p)
        if (*p <= 32 || *p == 127) return false;
    const char *authority_end = strpbrk(config->broker_uri + 8, "/?#");
    if ((authority_end ? authority_end : config->broker_uri + strlen(config->broker_uri)) == config->broker_uri + 8) return false;
    for (const char *p = config->topic; *p; ++p)
        if (*p == '+' || *p == '#' || *p <= 32 || *p == 127) return false;
    return true;
}

static esp_err_t agent_config_path(char path[288]) {
    int n = snprintf(path, 288, "%s/agent_mqtt.json", s_root);
    return n > 0 && n < 288 ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static uint32_t s_agent_config_version;

static esp_err_t store_agent_config(const read_pico_agent_config_t *config) {
    if (config && !valid_agent_config(config)) return ESP_ERR_INVALID_ARG;
    esp_err_t err = agent_config_store_write(config);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        ++s_agent_config_version;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

static esp_err_t load_agent_nvs(read_pico_agent_config_t *out) {
    esp_err_t err = agent_config_store_read(out);
    if (err == ESP_ERR_NVS_NOT_FOUND) return err;
    if (err != ESP_OK || !valid_agent_config(out)) {
        memset(out, 0, sizeof(*out));
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    return ESP_OK;
}

static esp_err_t migrate_legacy_agent_config(read_pico_agent_config_t *out) {
    if (!s_root[0]) return ESP_ERR_NOT_FOUND;
    char path[288];
    esp_err_t err = agent_config_path(path);
    if (err != ESP_OK) return err;
    FILE *file = fopen(path, "rb");
    if (!file) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    char body[1025];
    size_t len = fread(body, 1, sizeof(body) - 1, file);
    bool too_long = fgetc(file) != EOF;
    bool read_error = ferror(file) != 0;
    if (fclose(file) != 0) read_error = true;
    if (too_long || read_error) return ESP_ERR_INVALID_SIZE;
    body[len] = '\0';
    cJSON *json = cJSON_ParseWithLength(body, len);
    memset(out, 0, sizeof(*out));
    bool valid = json
        && copy_json_string(json, "uri", out->broker_uri, sizeof(out->broker_uri), true)
        && copy_json_string(json, "username", out->username, sizeof(out->username), false)
        && copy_json_string(json, "password", out->password, sizeof(out->password), false)
        && copy_json_string(json, "topic", out->topic, sizeof(out->topic), true)
        && valid_agent_config(out);
    cJSON_Delete(json);
    memset(body, 0, sizeof(body));
    if (!valid) { memset(out, 0, sizeof(*out)); return ESP_ERR_INVALID_RESPONSE; }
    err = store_agent_config(out);
    if (err == ESP_OK) (void)unlink(path);
    return err;
}

esp_err_t read_pico_transfer_get_agent_config(read_pico_agent_config_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    esp_err_t err = load_agent_nvs(out);
    if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    return migrate_legacy_agent_config(out);
}

uint32_t read_pico_transfer_agent_config_version(void) {
    portENTER_CRITICAL(&s_lock);
    uint32_t version = s_agent_config_version;
    portEXIT_CRITICAL(&s_lock);
    return version;
}

static esp_err_t info_handler(httpd_req_t *req) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    cJSON_AddBoolToObject(json, "is_flash", s_cfg.is_flash);
    cJSON_AddNumberToObject(json, "free_bytes", (double)s_cfg.free_bytes_cb(s_cfg.free_bytes_ctx));
    cJSON_AddNumberToObject(json, "file_limit", (double)s_cfg.file_limit);
    cJSON_AddStringToObject(json, "mode", status.mode == READ_PICO_TRANSFER_MODE_AP ? "ap" : "sta");
    cJSON_AddBoolToObject(json, "network_ready", status.network_ready);
    cJSON_AddStringToObject(json, "url", status.url);
    cJSON_AddStringToObject(json, "mdns_host", status.mdns_host[0] ? status.mdns_host : "readpico");
    cJSON_AddBoolToObject(json, "wifi_configured", status.wifi_configured);
    cJSON_AddStringToObject(json, "wifi_ssid", status.wifi_ssid);
    cJSON *saved = cJSON_AddArrayToObject(json, "wifi_ssids");
    for (size_t i = 0; saved && i < status.wifi_count; ++i) cJSON_AddItemToArray(saved, cJSON_CreateString(status.wifi_ssids[i]));
    cJSON_AddStringToObject(json, "root", s_root);
    cJSON_AddBoolToObject(json, "fonts_enabled", s_font_root[0] != 0);
    cJSON_AddStringToObject(json, "clip_token", s_clip_token);
    cJSON_AddStringToObject(json, "font_root", s_font_root);
    cJSON_AddNumberToObject(json, "font_limit", TRANSFER_FONT_MAX);
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body); return err;
}

static esp_err_t wifi_response(httpd_req_t *req, const char *status, const char *body) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_status(req, status);
    httpd_resp_sendstr(req, body);
    return ESP_FAIL;
}

static bool host_matches(const char *host, const char *ip, const char *mdns_host) {
    return management_host_allowed(host, ip, mdns_host);
}

static bool management_request(httpd_req_t *req) {
    char origin[96], host[64];
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (!status.network_ready || strncmp(status.url, "http://", 7)) return false;
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
        !host_matches(host, status.url + 7, status.mdns_host)) return false;
    if (httpd_req_get_hdr_value_len(req, "Origin")) {
        if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
            !management_origin_allowed(origin, status.url, status.mdns_host)) return false;
    }
    return true;
}

static bool provisioning_request(httpd_req_t *req) {
    if (s_cfg.mode != READ_PICO_TRANSFER_MODE_AP || !management_request(req)) return false;
    char content_type[80];
    return httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK &&
        !strncasecmp(content_type, "application/json", 16) &&
        (!content_type[16] || content_type[16] == ';' || content_type[16] == ' ');
}

static bool receive_stopped(void *ctx);
static int64_t receive_now(void *ctx);
static int receive_body(void *ctx, char *buf, size_t n);
static bool decode_query(const char *src, char out[121]);

static esp_err_t webopen_handler(httpd_req_t *req) {
    if (!management_request(req)) return wifi_response(req, "403 Forbidden", "{\"error\":\"请从设备显示的地址打开管理页面\"}");
    char content_type[80];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "application/json", 16) ||
        (content_type[16] && content_type[16] != ';' && content_type[16] != ' '))
        return wifi_response(req, "400 Bad Request", "{\"error\":\"仅接受 application/json\"}");
    if (!req->content_len || req->content_len > 320)
        return wifi_response(req, "400 Bad Request", "{\"error\":\"网址请求正文长度无效\"}");
    char body[321] = {0}, url[READ_PICO_WEBOPEN_URL_MAX] = {0};
    if (!receive_credentials_body(body, req->content_len, receive_body, receive_stopped, receive_now, req)) {
        clear_secret(body, sizeof(body));
        return wifi_response(req, "408 Request Timeout", "{\"error\":\"接收网址超时\"}");
    }
    bool valid = parse_webopen(body, req->content_len, url);
    clear_secret(body, sizeof(body));
    if (!valid) return wifi_response(req, "400 Bad Request", "{\"error\":\"网址必须是合法的 http(s):// 地址\"}");
    portENTER_CRITICAL(&s_lock);
    memcpy(s_webopen_url, url, sizeof(s_webopen_url));
    portEXIT_CRITICAL(&s_lock);
    return wifi_response(req, "200 OK", "{\"ok\":true}");
}

static bool receive_stopped(void *ctx) {
    (void)ctx;
    portENTER_CRITICAL(&s_lock); bool stopping = s_stopping; portEXIT_CRITICAL(&s_lock);
    return stopping;
}

static int64_t receive_now(void *ctx) { (void)ctx; return esp_timer_get_time() / 1000; }
static int receive_body(void *ctx, char *buf, size_t n) { return httpd_req_recv(ctx, buf, n); }

static esp_err_t wifi_handler(httpd_req_t *req) {
    if (!provisioning_request(req))
        return wifi_response(req, "403 Forbidden", "{\"error\":\"请在设备热点页面配置网络\"}");
    if (req->method == HTTP_DELETE) {
        if (req->content_len) return wifi_response(req, "400 Bad Request", "{\"error\":\"遗忘请求不应包含正文\"}");
        char raw[400], value[360], ssid[121];
        esp_err_t err;
        if (httpd_req_get_url_query_len(req)) {
            if (httpd_req_get_url_query_str(req, raw, sizeof(raw)) != ESP_OK ||
                httpd_query_key_value(raw, "ssid", value, sizeof(value)) != ESP_OK || !decode_query(value, ssid))
                return wifi_response(req, "400 Bad Request", "{\"error\":\"SSID 参数无效\"}");
            err = read_pico_transfer_forget_wifi_ssid(ssid);
        } else err = read_pico_transfer_forget_wifi();
        return wifi_response(req, err == ESP_OK ? "200 OK" : "500 Internal Server Error",
            err == ESP_OK ? "{\"ok\":true}" : "{\"error\":\"无法遗忘网络，请重试\"}");
    }
    if (!req->content_len || req->content_len > 256)
        return wifi_response(req, "400 Bad Request", "{\"error\":\"网络配置正文长度无效\"}");
    char body[257] = {0};
    if (!receive_credentials_body(body, req->content_len, receive_body, receive_stopped, receive_now, req)) {
        clear_secret(body, sizeof(body));
        return wifi_response(req, "408 Request Timeout", "{\"error\":\"接收配置超时，请重试\"}");
    }
    transfer_credentials_t next;
    bool valid = parse_credentials(body, req->content_len, &next);
    clear_secret(body, sizeof(body));
    if (!valid) return wifi_response(req, "400 Bad Request", "{\"error\":\"SSID须为1至32字节；密码留空，或8至63个ASCII字符、64位十六进制密钥\"}");
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (claim_config()) {
        transfer_credentials_t current = {0};
        err = load_credentials(&current);
        if (err == ESP_OK) {
            size_t index = 0;
            while (index < current.count && strcmp(current.entries[index].ssid, next.entries[0].ssid)) ++index;
            if (index == current.count && current.count == 5) err = ESP_ERR_NO_MEM;
            else {
                if (index == current.count) ++current.count;
                current.version = 2;
                current.recent = (uint8_t)index;
                memcpy(&current.entries[index], &next.entries[0], sizeof(next.entries[0]));
                err = store_credentials(&current);
            }
        }
        if (err == ESP_OK) {
            publish_credentials(&current);
            ESP_LOGI("transfer", "wifi saved configured=1");
        }
        clear_secret(&current, sizeof(current));
        release_config();
    }
    clear_secret(&next, sizeof(next));
    return wifi_response(req, err == ESP_OK ? "200 OK" : "500 Internal Server Error",
        err == ESP_OK ? "{\"ok\":true}" : "{\"error\":\"保存网络失败，原配置未主动清除，请重试\"}");
}

static int receive_http(void *ctx, char *buf, size_t n) {
    portENTER_CRITICAL(&s_lock); bool stopping = s_stopping; portEXIT_CRITICAL(&s_lock);
    if (stopping) return -1;
    return httpd_req_recv(ctx, buf, n);
}

static void upload_progress(size_t n) {
    portENTER_CRITICAL(&s_lock); s_status.cur_bytes = n; portEXIT_CRITICAL(&s_lock);
}

static bool decode_query(const char *src, char out[121]) {
    size_t n = 0;
    while (*src) {
        unsigned char c = (unsigned char)*src++;
        if (c == '%') {
            if (!src[0] || !src[1]) return false;
            int a = hex_value(src[0]), b = hex_value(src[1]);
            if (a < 0 || b < 0) return false;
            c = (unsigned char)((a << 4) | b); src += 2;
        }
        if (!c || n == 64) return false;
        out[n++] = (char)c;
    }
    out[n] = 0; return true;
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json) {
    if (!json) return ESP_ERR_NO_MEM;
    char *body = cJSON_PrintUnformatted(json); cJSON_Delete(json);
    if (!body) return ESP_ERR_NO_MEM;
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body); cJSON_free(body); return err;
}

static esp_err_t agent_config_handler(httpd_req_t *req) {
    if (!management_request(req))
        return wifi_response(req, "403 Forbidden", "{\"error\":\"请从设备显示的网址打开配置页面\"}");
    if (req->method == HTTP_GET) {
        read_pico_agent_config_t config = {0};
        esp_err_t err = read_pico_transfer_get_agent_config(&config);
        cJSON *json = cJSON_CreateObject();
        if (!json) return ESP_ERR_NO_MEM;
        cJSON_AddBoolToObject(json, "configured", err == ESP_OK);
        if (err == ESP_OK) {
            cJSON_AddStringToObject(json, "uri", config.broker_uri);
            cJSON_AddStringToObject(json, "username", config.username);
            cJSON_AddStringToObject(json, "topic", config.topic);
        } else if (err != ESP_ERR_NOT_FOUND) {
            cJSON_Delete(json);
            memset(&config, 0, sizeof(config));
            return respond_error(req, 500);
        }
        memset(&config, 0, sizeof(config));
        return send_json(req, json);
    }

    if (req->method == HTTP_DELETE) {
        if (req->content_len) return wifi_response(req, "400 Bad Request", "{\"error\":\"清除配置请求不应包含正文\"}");
        portENTER_CRITICAL(&s_lock);
        bool admitted = admission_begin(s_stopping, &s_upload_active);
        portEXIT_CRITICAL(&s_lock);
        if (!admitted) return wifi_response(req, "503 Service Unavailable", "{\"error\":\"设备正在切换网络，请稍后重试\"}");
        esp_err_t err = store_agent_config(NULL);
        char path[288];
        if (agent_config_path(path) == ESP_OK && unlink(path) != 0 && errno != ENOENT
            && err == ESP_OK) err = ESP_FAIL;
        bool ok = err == ESP_OK;
        portENTER_CRITICAL(&s_lock); s_upload_active = false; portEXIT_CRITICAL(&s_lock);
        return wifi_response(req, ok ? "200 OK" : "507 Insufficient Storage",
            ok ? "{\"ok\":true}" : "{\"error\":\"无法清除配置\"}");
    }

    char content_type[80];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "application/json", 16) ||
        (content_type[16] && content_type[16] != ';' && content_type[16] != ' '))
        return respond_error(req, 400);
    if (!req->content_len || req->content_len > 1024) return respond_error(req, req->content_len > 1024 ? 413 : 400);
    portENTER_CRITICAL(&s_lock);
    bool admitted = admission_begin(s_stopping, &s_upload_active);
    portEXIT_CRITICAL(&s_lock);
    if (!admitted) return wifi_response(req, "503 Service Unavailable", "{\"error\":\"设备正在切换网络，请稍后重试\"}");

    char body[1025] = {0};
    size_t received = 0;
    esp_err_t err = ESP_OK;
    while (received < req->content_len) {
        int n = receive_http(req, body + received, req->content_len - received);
        if (n <= 0) { err = n == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL; break; }
        received += (size_t)n;
    }
    body[received] = 0;
    cJSON *json = err == ESP_OK ? cJSON_ParseWithLength(body, received) : NULL;
    read_pico_agent_config_t next = {0}, old = {0};
    bool clear_password = false;
    cJSON *clear_item = json ? cJSON_GetObjectItemCaseSensitive(json, "clear_password") : NULL;
    if (cJSON_IsBool(clear_item)) clear_password = cJSON_IsTrue(clear_item);
    bool valid = json && copy_json_string(json, "uri", next.broker_uri, sizeof(next.broker_uri), true)
        && copy_json_string(json, "username", next.username, sizeof(next.username), true)
        && copy_json_string(json, "password", next.password, sizeof(next.password), true)
        && copy_json_string(json, "topic", next.topic, sizeof(next.topic), true)
        && valid_agent_config(&next);
    if (valid && !next.password[0] && !clear_password &&
        read_pico_transfer_get_agent_config(&old) == ESP_OK)
        memcpy(next.password, old.password, sizeof(next.password));
    memset(&old, 0, sizeof(old));
    cJSON_Delete(json);
    memset(body, 0, sizeof(body));

    bool saved = valid && store_agent_config(&next) == ESP_OK;
    memset(&next, 0, sizeof(next));
    portENTER_CRITICAL(&s_lock); s_upload_active = false; portEXIT_CRITICAL(&s_lock);
    if (!valid) return wifi_response(req, "400 Bad Request", "{\"error\":\"仅支持有效的 mqtts:// 地址、用户名、密码和精确主题\"}");
    return wifi_response(req, saved ? "200 OK" : "507 Insufficient Storage",
        saved ? "{\"ok\":true}" : "{\"error\":\"配置保存失败，原配置已保留\"}");
}

static esp_err_t ambient_config_handler(httpd_req_t *req) {
    if (!management_request(req))
        return wifi_response(req, "403 Forbidden", "{\"error\":\"请从设备显示的网址打开配置页面\"}");
    nvs_handle_t h;
    if (req->method == HTTP_GET) {
        uint8_t enabled = 0, quiet_start = 23, quiet_end = 7, source = 2;
        uint16_t interval = 60;
        char url[257] = {0};
        if (nvs_open("rp_ambient", NVS_READONLY, &h) == ESP_OK) {
            (void)nvs_get_u8(h, "enabled", &enabled);
            (void)nvs_get_u16(h, "interval_min", &interval);
            (void)nvs_get_u8(h, "quiet_start_h", &quiet_start);
            (void)nvs_get_u8(h, "quiet_end_h", &quiet_end);
            (void)nvs_get_u8(h, "source_type", &source);
            size_t len = sizeof(url);
            (void)nvs_get_str(h, "url", url, &len);
            nvs_close(h);
        }
        cJSON *json = cJSON_CreateObject();
        if (!json) return ESP_ERR_NO_MEM;
        cJSON_AddBoolToObject(json, "enabled", enabled != 0);
        cJSON_AddNumberToObject(json, "interval_min", interval);
        cJSON_AddNumberToObject(json, "quiet_start_h", quiet_start);
        cJSON_AddNumberToObject(json, "quiet_end_h", quiet_end);
        cJSON_AddNumberToObject(json, "source_type", source);
        cJSON_AddStringToObject(json, "url", url);
        return send_json(req, json);
    }
    char content_type[80];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "application/json", 16) ||
        (content_type[16] && content_type[16] != ';' && content_type[16] != ' ')) return respond_error(req, 400);
    if (!req->content_len || req->content_len > 4096) return respond_error(req, req->content_len > 4096 ? 413 : 400);
    char body[4097] = {0};
    size_t received = 0;
    while (received < req->content_len) {
        int n = receive_http(req, body + received, req->content_len - received);
        if (n <= 0) return respond_error(req, 400);
        received += (size_t)n;
    }
    cJSON *json = cJSON_ParseWithLength(body, received);
    cJSON *enabled_item = json ? cJSON_GetObjectItemCaseSensitive(json, "enabled") : NULL;
    cJSON *interval_item = json ? cJSON_GetObjectItemCaseSensitive(json, "interval_min") : NULL;
    cJSON *start_item = json ? cJSON_GetObjectItemCaseSensitive(json, "quiet_start_h") : NULL;
    cJSON *end_item = json ? cJSON_GetObjectItemCaseSensitive(json, "quiet_end_h") : NULL;
    cJSON *source_item = json ? cJSON_GetObjectItemCaseSensitive(json, "source_type") : NULL;
    char url[257] = {0}, auth[257] = {0};
    cJSON *clear_item = json ? cJSON_GetObjectItemCaseSensitive(json, "clear_auth") : NULL;
    bool clear_auth = cJSON_IsTrue(clear_item);
    bool valid = cJSON_IsBool(enabled_item) && cJSON_IsNumber(interval_item) &&
        cJSON_IsNumber(start_item) && cJSON_IsNumber(end_item) && cJSON_IsNumber(source_item) &&
        copy_json_string(json, "url", url, sizeof(url), true) &&
        copy_json_string(json, "auth_header", auth, sizeof(auth), true) &&
        (interval_item->valueint == 15 || interval_item->valueint == 30 || interval_item->valueint == 60 ||
         interval_item->valueint == 120 || interval_item->valueint == 360 || interval_item->valueint == 720 || interval_item->valueint == 1440) &&
        start_item->valueint >= 0 && start_item->valueint < 24 && end_item->valueint >= 0 && end_item->valueint < 24 &&
        source_item->valueint >= 0 && source_item->valueint <= 2;
    if (valid && cJSON_IsTrue(enabled_item) && source_item->valueint != 2) {
        size_t prefix = !strncmp(url, "http://", 7) ? 7 : !strncmp(url, "https://", 8) ? 8 : 0;
        const char *authority_end = prefix ? strpbrk(url + prefix, "/?#") : NULL;
        valid = prefix != 0 && (authority_end ? authority_end : url + strlen(url)) != url + prefix;
        for (const unsigned char *p = (const unsigned char *)url; valid && *p; ++p)
            if (*p <= 32 || *p == 127) valid = false;
    }
    for (const unsigned char *p = (const unsigned char *)auth; valid && *p; ++p)
        if (*p == '\r' || *p == '\n' || *p == 0x7f) valid = false;
    if (clear_auth) auth[0] = '\0';
    if (valid && auth[0] == 0 && !clear_auth && nvs_open("rp_ambient", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(auth);
        (void)nvs_get_str(h, "auth_header", auth, &len);
        nvs_close(h);
    }
    esp_err_t err = valid ? nvs_open("rp_ambient", NVS_READWRITE, &h) : ESP_ERR_INVALID_ARG;
    if (err == ESP_OK) {
        err = nvs_set_u8(h, "enabled", cJSON_IsTrue(enabled_item));
        if (err == ESP_OK) err = nvs_set_u16(h, "interval_min", (uint16_t)interval_item->valueint);
        if (err == ESP_OK) err = nvs_set_u8(h, "quiet_start_h", (uint8_t)start_item->valueint);
        if (err == ESP_OK) err = nvs_set_u8(h, "quiet_end_h", (uint8_t)end_item->valueint);
        if (err == ESP_OK) err = nvs_set_u8(h, "source_type", (uint8_t)source_item->valueint);
        if (err == ESP_OK) err = nvs_set_str(h, "url", url);
        if (err == ESP_OK) err = nvs_set_str(h, "auth_header", auth);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    cJSON_Delete(json);
    memset(auth, 0, sizeof(auth));
    memset(body, 0, sizeof(body));
    return wifi_response(req, err == ESP_OK ? "200 OK" : valid ? "507 Insufficient Storage" : "400 Bad Request",
        err == ESP_OK ? "{\"ok\":true}" : valid ? "{\"error\":\"配置保存失败\"}" : "{\"error\":\"定时看板配置无效\"}");
}

static esp_err_t file_response(httpd_req_t *req, file_result_t result, bool deletion, const char *path) {
    if (result.progress_cleanup_failed || result.storage_cleanup_failed) {
        set_error(result.progress_cleanup_failed ? ESP_FAIL : ESP_OK);
        cJSON *json = cJSON_CreateObject();
        if (!json) return ESP_ERR_NO_MEM;
        const char *name = strrchr(path, '/');
        cJSON_AddStringToObject(json, "name", name ? name + 1 : path);
        cJSON_AddBoolToObject(json, deletion ? "deleted" : "committed", true);
        cJSON_AddBoolToObject(json, "ok", !result.progress_cleanup_failed);
        if (result.progress_cleanup_failed) {
            cJSON_AddBoolToObject(json, "progress_cleanup_failed", true);
            cJSON_AddStringToObject(json, "error", deletion ? "文件已删除，但阅读进度清理失败；请重试清理进度" :
                "文件已保存，但旧阅读进度清理失败；请重试清理进度");
            httpd_resp_set_status(req, "500 Internal Server Error");
        }
        if (result.storage_cleanup_failed) {
            cJSON_AddBoolToObject(json, "storage_cleanup_failed", true);
            cJSON_AddStringToObject(json, "warning", "文件已保存，旧文件备份清理失败；备份已保留，请勿重复上传");
            ESP_LOGW("transfer", "committed with retained backup");
        }
        return send_json(req, json);
    }
    if (result.status != 200) return respond_error(req, result.status);
    set_error(ESP_OK);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t books_handler(httpd_req_t *req) {
    if (!management_request(req)) return wifi_response(req, "403 Forbidden", "{\"error\":\"请从设备显示的地址打开管理页面\"}");
    char query[1024] = {0}, encoded[766], name[256] = {0}, path[448];
    if (httpd_req_get_url_query_len(req) && httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return respond_error(req, 400);
    esp_err_t name_err = httpd_query_key_value(query, "name", encoded, sizeof(encoded));
    if (name_err != ESP_OK && name_err != ESP_ERR_NOT_FOUND) return respond_error(req, 400);
    bool named = name_err == ESP_OK;
    if (named && !decode_name_limit(encoded, name, sizeof(name))) return respond_error(req, 400);
    snprintf(path, sizeof(path), "%s/%s", s_root, name);
    if (req->method == HTTP_GET) {
        cJSON *json = cJSON_CreateObject();
        if (!json) return ESP_ERR_NO_MEM;
        cJSON_AddStringToObject(json, "root", s_root);
        cJSON_AddStringToObject(json, "root_label", s_cfg.is_flash ? "内置存储" : "TF 卡");
        if (named) {
            struct stat st;
            if (stat(path, &st)) {
                int result = errno == ENOENT ? 404 : 507; cJSON_Delete(json);
                if (result == 404) return wifi_response(req, "404 Not Found", "{\"error\":\"图书不存在\"}");
                return respond_error(req, result);
            }
            if (!S_ISREG(st.st_mode)) { cJSON_Delete(json); return respond_error(req, 400); }
            cJSON *item = cJSON_AddObjectToObject(json, "item");
            cJSON_AddStringToObject(item, "name", name); cJSON_AddNumberToObject(item, "size", (double)st.st_size);
        } else {
            char search[121] = {0}, page_text[16] = "0";
            esp_err_t search_err = httpd_query_key_value(query, "q", encoded, sizeof(encoded));
            if ((search_err != ESP_OK && search_err != ESP_ERR_NOT_FOUND) || (search_err == ESP_OK && !decode_query(encoded, search))) {
                cJSON_Delete(json); return respond_error(req, 400);
            }
            esp_err_t page_err = httpd_query_key_value(query, "page", page_text, sizeof(page_text));
            if (page_err != ESP_OK && page_err != ESP_ERR_NOT_FOUND) { cJSON_Delete(json); return respond_error(req, 400); }
            char *end; unsigned long page = strtoul(page_text, &end, 10);
            if (!page_text[0] || *end || page_text[0] == '-' || page > 1000000) { cJSON_Delete(json); return respond_error(req, 400); }
            transfer_book_page_t *entries = calloc(1, sizeof(*entries));
            if (!entries) { cJSON_Delete(json); return ESP_ERR_NO_MEM; }
            int result = list_books(s_root, search, page, entries);
            if (result) { free(entries); cJSON_Delete(json); return respond_error(req, result); }
            cJSON_AddNumberToObject(json, "page", page); cJSON_AddNumberToObject(json, "total", entries->total);
            cJSON_AddNumberToObject(json, "pages", (entries->total + BOOK_LIST_PAGE_SIZE - 1) / BOOK_LIST_PAGE_SIZE);
            cJSON *items = cJSON_AddArrayToObject(json, "items");
            for (size_t i = 0; i < entries->count; ++i) {
                cJSON *item = cJSON_CreateObject(); cJSON_AddItemToArray(items, item);
                cJSON_AddStringToObject(item, "name", entries->items[i].name); cJSON_AddNumberToObject(item, "size", (double)entries->items[i].size);
                cJSON_AddBoolToObject(item, "has_notes", entries->items[i].has_notes);
            }
            free(entries);
        }
        return send_json(req, json);
    }
    if (!named || req->content_len) return respond_error(req, 400);
    char action[32];
    bool retry = req->method == HTTP_POST && httpd_query_key_value(query, "action", action, sizeof(action)) == ESP_OK && !strcmp(action, "retry_progress");
    if (req->method != HTTP_DELETE && !retry) return respond_error(req, 400);
    int resolved = resolve_mutation_path(s_root, name, path, sizeof(path));
    if (resolved) return respond_error(req, resolved);
    portENTER_CRITICAL(&s_lock);
    bool accepted = admission_begin(s_stopping, &s_upload_active);
    portEXIT_CRITICAL(&s_lock);
    if (!accepted) return wifi_response(req, "409 Conflict", "{\"error\":\"正在上传或切换网络，请稍后重试\"}");
    file_result_t result = retry ? (file_result_t){.status = !s_cfg.file_changed_cb || s_cfg.file_changed_cb(path) == ESP_OK ? 200 : 500} :
        delete_managed(path, s_cfg.file_changed_cb);
    portENTER_CRITICAL(&s_lock);
    record_file_change(&s_status.changed_count, result, retry);
    s_upload_active = false;
    s_webopen_url[0] = 0;
    portEXIT_CRITICAL(&s_lock);
    return file_response(req, result, true, path);
}

static bool markdown_write_position(FILE *out, bool excerpt, uint16_t chapter, uint32_t offset,
                                   uint8_t percent) {
    if (fprintf(out, "## %s · 第 %u 章 · %u%%\n\n", excerpt ? "摘录" : "书签", (unsigned)chapter + 1, percent) < 0)
        return false;
    if (!excerpt) return fprintf(out, "> 位置：%u 字节\n\n", (unsigned)offset) >= 0;
    return fputs("> ", out) >= 0;
}

static bool markdown_copy_excerpt(FILE *out, FILE *input, size_t length) {
    char chunk[512];
    bool line_start = false;
    while (length) {
        size_t want = length < sizeof(chunk) ? length : sizeof(chunk);
        size_t got = fread(chunk, 1, want, input);
        if (got != want || memchr(chunk, 0, got)) return false;
        for (size_t i = 0; i < got; ++i) {
            unsigned char c = (unsigned char)chunk[i];
            if (c == '\r') continue;
            if (line_start && fputs("> ", out) < 0) return false;
            if (fputc(c, out) == EOF) return false;
            line_start = c == '\n';
        }
        length -= got;
    }
    if (fputs("\n\n", out) < 0) return false;
    return true;
}

static esp_err_t notes_download_handler(httpd_req_t *req) {
    if (!management_request(req)) return wifi_response(req, "403 Forbidden", "{\"error\":\"请从设备显示的地址打开管理页面\"}");
    char query[512], encoded[361], name[121], book_path[448], sidecar[512], export_path[512], directory[160];
    if (req->method != HTTP_GET || req->content_len || httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", encoded, sizeof(encoded)) != ESP_OK || !decode_name(encoded, name))
        return respond_error(req, 400);
    if (snprintf(book_path, sizeof(book_path), "%s/%s", s_root, name) >= (int)sizeof(book_path) ||
        snprintf(sidecar, sizeof(sidecar), "%s/.pico_notes/n_%08" PRIx32 ".bin", s_root, notes_hash(book_path)) >= (int)sizeof(sidecar))
        return respond_error(req, 400);
    struct stat st;
    if (stat(book_path, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > UINT32_MAX)
        return wifi_response(req, "404 Not Found", "{\"error\":\"图书不存在\"}");
    FILE *input = fopen(sidecar, "rb");
    if (!input) return wifi_response(req, "404 Not Found", "{\"error\":\"这本书没有笔记\"}");
    uint8_t header[12]; char owner[448];
    bool valid = fread(header, 1, sizeof(header), input) == sizeof(header) && !memcmp(header, "RBNF", 4) &&
                 (header[4] == 1 || header[4] == 2);
    size_t path_len = valid ? (size_t)header[5] | ((size_t)header[6] << 8) : 0;
    uint32_t file_size = valid ? (uint32_t)header[8] | ((uint32_t)header[9] << 8) |
                                 ((uint32_t)header[10] << 16) | ((uint32_t)header[11] << 24) : 0;
    valid = valid && path_len >= 2 && path_len <= sizeof(owner) && fread(owner, 1, path_len, input) == path_len &&
            owner[path_len - 1] == 0 && !strcmp(owner, book_path) && file_size == (uint32_t)st.st_size;
    if (!valid) { fclose(input); return respond_error(req, 404); }
    const char *parent = s_cfg.is_flash ? "/flash/notes" : "/sdcard/notes";
    if (snprintf(directory, sizeof(directory), "%s", parent) >= (int)sizeof(directory) ||
        (mkdir(directory, 0777) != 0 && errno != EEXIST)) { fclose(input); return respond_error(req, 507); }
    char safe[256]; size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)name; *p && n + 8 < sizeof(safe); ++p)
        safe[n++] = ((*p < 0x20 || strchr("/\\:*?\"<>|", *p)) ? '_' : (char)*p);
    while (n && (safe[n - 1] == ' ' || safe[n - 1] == '.')) --n;
    if (!n) { memcpy(safe, "notes", 5); n = 5; }
    safe[n] = 0;
    if (snprintf(export_path, sizeof(export_path), "%s/%s.md", parent, safe) >= (int)sizeof(export_path)) { fclose(input); return respond_error(req, 507); }
    FILE *out = fopen(export_path, "wb");
    if (!out) { fclose(input); return respond_error(req, 507); }
    bool ok = fprintf(out, "# %s\n\n", name) >= 0;
    while (ok) {
        uint8_t record[8]; size_t got = fread(record, 1, sizeof(record), input);
        if (got == 0) { if (feof(input)) break; ok = false; break; }
        if (got != sizeof(record) || record[7] > 100) { ok = false; break; }
        uint16_t chapter = (uint16_t)record[1] | (uint16_t)((uint16_t)record[2] << 8);
        uint32_t offset = (uint32_t)record[3] | ((uint32_t)record[4] << 8) | ((uint32_t)record[5] << 16) | ((uint32_t)record[6] << 24);
        if (record[0] == 1) ok = markdown_write_position(out, false, chapter, offset, record[7]);
        else if (record[0] == 2) {
            uint8_t raw_len[4] = {0};
            size_t length_bytes = header[4] == 1 ? 2 : 4;
            if (fread(raw_len, 1, length_bytes, input) != length_bytes) { ok = false; break; }
            size_t len = (size_t)raw_len[0] | ((size_t)raw_len[1] << 8);
            if (length_bytes == 4) len |= (size_t)raw_len[2] << 16 | (size_t)raw_len[3] << 24;
            if (!len || !markdown_write_position(out, true, chapter, offset, record[7])) { ok = false; break; }
            ok = markdown_copy_excerpt(out, input, len);
        } else ok = false;
    }
    if (fflush(out) != 0) ok = false;
    fclose(out); fclose(input);
    if (!ok) { unlink(export_path); return respond_error(req, 404); }
    out = fopen(export_path, "rb");
    if (!out) return respond_error(req, 507);
    httpd_resp_set_type(req, "text/markdown; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"Read-Pico-notes.md\"");
    char chunk[1024];
    while ((n = fread(chunk, 1, sizeof(chunk), out)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) { fclose(out); return ESP_FAIL; }
    }
    fclose(out);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t upload_handler(httpd_req_t *req) {
    if (!management_request(req)) return wifi_response(req, "403 Forbidden", "{\"error\":\"请从设备显示的地址打开管理页面\"}");
    char query[512], encoded[361], name[121], path[288], part[296], replace[8];
    bool font = req->user_ctx != NULL;
    if (font && !s_font_root[0]) return wifi_response(req, "409 Conflict", "{\"error\":\"字体需要 TF 卡，请挂载后重新进入传书\"}");
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", encoded, sizeof(encoded)) != ESP_OK ||
        !(font ? decode_font_name(encoded, name) : decode_name(encoded, name)))
        return respond_error(req, 400);
    int resolved = resolve_mutation_path(font ? s_font_root : s_root, name, path, sizeof(path));
    if (resolved) return respond_error(req, resolved);
    snprintf(part, sizeof(part), "%s.part", path);
    bool overwrite = httpd_query_key_value(query, "overwrite", replace, sizeof(replace)) == ESP_OK && !strcmp(replace, "1");
    portENTER_CRITICAL(&s_lock);
    if (!admission_begin(s_stopping, &s_upload_active)) {
        portEXIT_CRITICAL(&s_lock);
        return wifi_response(req, "503 Service Unavailable", "{\"error\":\"网络正在切换，请连接后重试\"}");
    }
    s_status.state = READ_PICO_TRANSFER_UPLOADING; s_status.last_error = ESP_OK;
    memcpy(s_status.cur_name, name, strlen(name) + 1);
    s_status.cur_bytes = 0; s_status.cur_total = req->content_len;
    portEXIT_CRITICAL(&s_lock);
    int64_t started = esp_timer_get_time();
    ESP_LOGI("transfer", "receive name=%s bytes=%u", name, (unsigned)req->content_len);
    file_result_t result = upload_managed(path, part, req->content_len, font ? TRANSFER_FONT_MAX : s_cfg.file_limit,
        s_cfg.free_bytes_cb(s_cfg.free_bytes_ctx), overwrite, s_buffer, 16384, receive_http, req, upload_progress,
        font ? NULL : s_cfg.file_changed_cb);
    portENTER_CRITICAL(&s_lock);
    s_upload_active = false;
    if (result.changed) { s_status.done_count++; s_status.changed_count++; }
    portEXIT_CRITICAL(&s_lock);
    if (result.changed) ESP_LOGI("transfer", "committed name=%s bytes=%u elapsed_ms=%lld", name,
             (unsigned)req->content_len, (esp_timer_get_time() - started) / 1000);
    return file_response(req, result, false, path);
}

static esp_err_t ota_upload_handler(httpd_req_t *req) {
    if (!management_request(req)) return wifi_response(req, "403 Forbidden", "{\"error\":\"请从设备显示的地址打开管理页面\"}");
    (void)read_pico_pmu_refresh();
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    bool charging = pmu && pmu->charge_state == PMU_CHARGE_CHARGING;
    if (!pmu || !ota_update_battery_allowed(pmu->soc_permille, charging))
        return wifi_response(req, "409 Conflict", "{\"error\":\"电量低于 20%，请连接充电器后升级\"}");
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || req->content_len < (int)sizeof(esp_image_header_t) ||
        req->content_len > (int)partition->size)
        return wifi_response(req, "413 Payload Too Large", "{\"error\":\"固件大小不适用于备用分区\"}");
    portENTER_CRITICAL(&s_lock);
    bool admitted = admission_begin(s_stopping, &s_upload_active);
    portEXIT_CRITICAL(&s_lock);
    if (!admitted) return wifi_response(req, "503 Service Unavailable", "{\"error\":\"设备正在处理其他上传\"}");

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(partition, (size_t)req->content_len, &handle);
    bool ota_begun = err == ESP_OK;
    uint8_t buffer[4096];
    uint8_t image_header[sizeof(esp_image_header_t)];
    size_t header_bytes = 0;
    size_t received = 0;
    bool header_checked = false;
    while (err == ESP_OK && received < (size_t)req->content_len) {
        size_t want = (size_t)req->content_len - received;
        if (want > sizeof(buffer)) want = sizeof(buffer);
        int n = httpd_req_recv(req, (char *)buffer, want);
        if (n <= 0) { err = ESP_FAIL; break; }
        size_t offset = 0;
        if (!header_checked) {
            size_t take = sizeof(image_header) - header_bytes;
            if (take > (size_t)n) take = (size_t)n;
            memcpy(image_header + header_bytes, buffer, take);
            header_bytes += take; offset += take;
            if (header_bytes == sizeof(image_header)) {
                esp_image_header_t header;
                memcpy(&header, image_header, sizeof(header));
                if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32S3) {
                    err = ESP_ERR_INVALID_ARG; break;
                }
                header_checked = true;
                err = esp_ota_write(handle, image_header, sizeof(image_header));
            }
        }
        if (err == ESP_OK && header_checked && offset < (size_t)n)
            err = esp_ota_write(handle, buffer + offset, (size_t)n - offset);
        received += (size_t)n;
    }
    if (err == ESP_OK && !header_checked) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) err = esp_ota_end(handle);
    else if (ota_begun) (void)esp_ota_abort(handle);
    if (err == ESP_OK) err = esp_ota_set_boot_partition(partition);
    portENTER_CRITICAL(&s_lock); s_upload_active = false; portEXIT_CRITICAL(&s_lock);
    if (err != ESP_OK) return wifi_response(req, "422 Unprocessable Content", "{\"error\":\"固件校验失败，当前系统未更改\"}");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"升级完成，设备即将重启\"}");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

#define WEBCLIP_TEXT_MAX (64u * 1024u)
#define WEBCLIP_BODY_MAX (640u * 1024u)

static bool form_value(const char *form, const char *key, char *out, size_t cap) {
    if (!form || !key || !out || cap == 0) return false;
    size_t key_len = strlen(key);
    const char *field = form;
    while (*field) {
        const char *end = strchr(field, '&');
        if (!end) end = field + strlen(field);
        const char *equals = memchr(field, '=', (size_t)(end - field));
        if (equals && (size_t)(equals - field) == key_len && !memcmp(field, key, key_len)) {
            const char *p = equals + 1;
            size_t n = 0;
            while (p < end) {
                unsigned char c = (unsigned char)*p++;
                if (c == '+') c = ' ';
                else if (c == '%') {
                    if (end - p < 2) return false;
                    int hi = hex_value((unsigned char)p[0]);
                    int lo = hex_value((unsigned char)p[1]);
                    if (hi < 0 || lo < 0) return false;
                    c = (unsigned char)((hi << 4) | lo);
                    p += 2;
                    if (c == 0) return false;
                }
                if (n + 1 >= cap) return false;
                out[n++] = (char)c;
            }
            out[n] = 0;
            return true;
        }
        field = *end ? end + 1 : end;
    }
    return false;
}

static bool clip_host_request(httpd_req_t *req) {
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    if (!status.network_ready || strncmp(status.url, "http://", 7)) return false;
    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK ||
        !host_matches(host, status.url + 7, status.mdns_host)) return false;
    if (httpd_req_get_hdr_value_len(req, "Origin")) {
        char origin[96];
        if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK ||
            !management_origin_allowed(origin, status.url, status.mdns_host)) return false;
    }
    return true;
}

static esp_err_t clip_reject(httpd_req_t *req, const char *status, const char *body) {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, body);
    return ESP_FAIL;
}

static void *transfer_alloc(size_t size) {
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return ptr ? ptr : malloc(size);
}

static esp_err_t webclip_handler(httpd_req_t *req) {
    if (!clip_host_request(req)) return clip_reject(req, "403 Forbidden", "{\"error\":\"请从设备显示的网址投递\"}");
    char content_type[80];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
        strncasecmp(content_type, "application/x-www-form-urlencoded", 33) ||
        (content_type[33] && content_type[33] != ';' && content_type[33] != ' '))
        return respond_error(req, 400);
    if (req->content_len == 0 || req->content_len > WEBCLIP_BODY_MAX)
        return respond_error(req, req->content_len > WEBCLIP_BODY_MAX ? 413 : 400);

    portENTER_CRITICAL(&s_lock);
    bool admitted = admission_begin(s_stopping, &s_upload_active);
    portEXIT_CRITICAL(&s_lock);
    if (!admitted) return clip_reject(req, "503 Service Unavailable", "{\"error\":\"设备正在切换网络，请稍后重试\"}");

    char *body = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) body = malloc(req->content_len + 1);
    esp_err_t err = ESP_OK;
    if (!body) err = ESP_ERR_NO_MEM;
    size_t received = 0;
    while (err == ESP_OK && received < req->content_len) {
        int n = receive_http(req, body + received, req->content_len - received);
        if (n <= 0) err = n == HTTPD_SOCK_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
        else received += (size_t)n;
    }
    if (err != ESP_OK) {
        free(body);
        portENTER_CRITICAL(&s_lock); s_upload_active = false; portEXIT_CRITICAL(&s_lock);
        return respond_error(req, err == ESP_ERR_TIMEOUT ? 408 : err == ESP_ERR_NO_MEM ? 500 : 400);
    }
    body[received] = 0;

    char token[33] = {0};
    char *title = transfer_alloc(512);
    char *source = transfer_alloc(2049);
    char *text = heap_caps_malloc(WEBCLIP_TEXT_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!text) text = malloc(WEBCLIP_TEXT_MAX + 1);
    bool valid = title && source && text;
    if (valid) { title[0] = '\0'; source[0] = '\0'; }
    valid = valid
        && form_value(body, "token", token, sizeof(token))
        && form_value(body, "title", title, sizeof(title))
        && form_value(body, "url", source, sizeof(source))
        && form_value(body, "text", text, WEBCLIP_TEXT_MAX + 1)
        && strlen(text) <= WEBCLIP_TEXT_MAX
        && !strcmp(token, s_clip_token)
        && (!strncmp(source, "http://", 7) || !strncmp(source, "https://", 8))
        && clip_safe_text(title, true) && clip_safe_text(source, true)
        && clip_safe_text(text, false);
    free(body);
    if (!valid) {
        free(text); free(title); free(source);
        portENTER_CRITICAL(&s_lock); s_upload_active = false; portEXIT_CRITICAL(&s_lock);
        return wifi_response(req, "400 Bad Request", "{\"error\":\"剪藏内容无效、过长或设备口令已失效\"}");
    }

    portENTER_CRITICAL(&s_lock);
    s_status.state = READ_PICO_TRANSFER_UPLOADING;
    s_status.last_error = ESP_OK;
    snprintf(s_status.cur_name, sizeof(s_status.cur_name), "网页剪藏");
    s_status.cur_bytes = 0;
    s_status.cur_total = strlen(text);
    portEXIT_CRITICAL(&s_lock);

    char path[288], part[296];
    char *header = transfer_alloc(2600);
    uint64_t nonce = ((uint64_t)esp_random() << 32) | esp_random();
    int path_len = snprintf(path, sizeof(path), "%s/webclip_%016llx.txt", s_root, (unsigned long long)nonce);
    int part_len = path_len > 0 && path_len < (int)sizeof(path)
        ? snprintf(part, sizeof(part), "%s.part", path) : -1;
    size_t text_len = strlen(text);
    size_t total_len = strlen(title) + strlen(source) + text_len + 32;
    uint64_t free_bytes = s_cfg.free_bytes_cb(s_cfg.free_bytes_ctx);
    bool committed = false;
    if (header && part_len > 0 && part_len < (int)sizeof(part)
        && (!s_cfg.file_limit || total_len <= s_cfg.file_limit) && total_len <= free_bytes) {
        int fd = open(part, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) {
            int header_len = snprintf(header, 2600, "%s\n来源：%s\n\n", title, source);
            size_t wrote = 0;
            bool ok = header_len > 0 && header_len < 2600;
            while (ok && wrote < (size_t)header_len) {
                ssize_t n = write(fd, header + wrote, (size_t)header_len - wrote);
                if (n <= 0) ok = false;
                else wrote += (size_t)n;
            }
            wrote = 0;
            while (ok && wrote < text_len) {
                ssize_t n = write(fd, text + wrote, text_len - wrote);
                if (n <= 0) ok = false;
                else wrote += (size_t)n;
            }
            if (ok && fsync(fd) != 0) ok = false;
            if (close(fd) != 0) ok = false;
            if (ok && rename(part, path) == 0) committed = true;
            if (!committed) unlink(part);
        }
    }
    free(text); free(title); free(source); free(header);
    if (committed && s_cfg.file_changed_cb) (void)s_cfg.file_changed_cb(path);
    portENTER_CRITICAL(&s_lock);
    s_upload_active = false;
    if (committed) {
        s_status.done_count++; s_status.changed_count++;
        s_status.last_error = ESP_OK;
        s_status.state = s_status.network_ready ? READ_PICO_TRANSFER_READY : READ_PICO_TRANSFER_STARTING;
    } else {
        s_status.last_error = ESP_FAIL;
        s_status.state = READ_PICO_TRANSFER_ERROR;
    }
    portEXIT_CRITICAL(&s_lock);
    if (!committed) return wifi_response(req, "507 Insufficient Storage", "{\"error\":\"剪藏保存失败或存储空间不足\"}");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "<!doctype html><meta charset=utf-8><title>已投递</title><h2>网页已投递到小纸 Pico</h2><p>返回设备打开图书即可阅读。</p>");
}

static esp_err_t font_info_handler(httpd_req_t *req) {
    if (!management_request(req)) return wifi_response(req, "403 Forbidden", "{\"error\":\"请从设备显示的地址打开管理页面\"}");
    if (!s_font_root[0]) return wifi_response(req, "409 Conflict", "{\"error\":\"字体需要 TF 卡，请挂载后重新进入传书\"}");
    char query[512], encoded[361], name[121], path[288];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", encoded, sizeof(encoded)) != ESP_OK || !decode_font_name(encoded, name))
        return respond_error(req, 400);
    int resolved = resolve_mutation_path(s_font_root, name, path, sizeof(path));
    if (resolved) return respond_error(req, resolved);
    struct stat st;
    if (stat(path, &st)) return respond_error(req, errno == ENOENT ? 404 : 507);
    if (!S_ISREG(st.st_mode) || st.st_size < 0) return respond_error(req, 400);
    cJSON *json = cJSON_CreateObject();
    if (!json) return ESP_ERR_NO_MEM;
    cJSON_AddStringToObject(json, "name", strrchr(path, '/') + 1);
    cJSON_AddNumberToObject(json, "size", (double)st.st_size);
    return send_json(req, json);
}

/* ---- 生命周期 / Lifecycle ---- */
bool read_pico_transfer_try_stop_if_idle(void) {
    portENTER_CRITICAL(&s_lock);
    bool accepted = admission_stop(&s_stopping, s_upload_active);
    portEXIT_CRITICAL(&s_lock);
    if (accepted) read_pico_transfer_stop();
    return accepted;
}

void read_pico_transfer_stop(void) {
    portENTER_CRITICAL(&s_lock); s_stopping = true; portEXIT_CRITICAL(&s_lock);
    if (s_cfg.mode == READ_PICO_TRANSFER_MODE_STA && s_cfg.network_stop_cb)
        s_cfg.network_stop_cb();
    stop_mdns();
    if (s_http) { httpd_stop(s_http); s_http = NULL; }
    if (s_started) { esp_wifi_stop(); s_started = false; }
    if (s_events) { esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_events); s_events = NULL; }
    if (s_ip_events) { esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_ip_events); s_ip_events = NULL; }
    if (s_wifi) { esp_wifi_deinit(); s_wifi = false; }
    if (s_netif) { esp_netif_destroy_default_wifi(s_netif); s_netif = NULL; }
    if (s_loop_owned) { esp_event_loop_delete_default(); s_loop_owned = false; }
    free(s_buffer); s_buffer = NULL;
    portENTER_CRITICAL(&s_lock);
    s_status.state = READ_PICO_TRANSFER_STOPPED; s_status.sta_count = 0;
    s_status.network_ready = false; s_status.url[0] = 0;
    s_upload_active = false;
    memset(&s_connection, 0, sizeof(s_connection));
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t read_pico_transfer_start(const read_pico_transfer_cfg_t *cfg) {
    if (!cfg || !cfg->root_dir || !cfg->free_bytes_cb || strlen(cfg->root_dir) >= sizeof(s_root) ||
        (cfg->font_dir && (cfg->is_flash || !cfg->font_dir[0] || strlen(cfg->font_dir) >= sizeof(s_font_root))) ||
        (cfg->mode != READ_PICO_TRANSFER_MODE_AP && cfg->mode != READ_PICO_TRANSFER_MODE_STA)) return ESP_ERR_INVALID_ARG;
    if (s_wifi || s_netif || s_http || s_ota_wifi) return ESP_ERR_INVALID_STATE;
    s_cfg = *cfg; strcpy(s_root, cfg->root_dir); s_cfg.root_dir = s_root;
    snprintf(s_font_root, sizeof(s_font_root), "%s", cfg->font_dir ? cfg->font_dir : "");
    s_cfg.font_dir = s_font_root[0] ? s_font_root : NULL;
    portENTER_CRITICAL(&s_lock);
    memset(&s_status, 0, sizeof(s_status)); s_status.state = READ_PICO_TRANSFER_STARTING;
    s_stopping = s_upload_active = false;
    s_status.mode = cfg->mode;
    portEXIT_CRITICAL(&s_lock);
    renew_clip_token();
    transfer_credentials_t saved = {0};
    wifi_config_t wifi = {0};
    esp_err_t err = load_credentials(&saved);
    if (err != ESP_OK && cfg->mode == READ_PICO_TRANSFER_MODE_STA) goto fail;
    publish_credentials(&saved);
    if (cfg->mode == READ_PICO_TRANSFER_MODE_STA && (saved.version != 2 || !saved.count)) { err = ESP_ERR_NOT_FOUND; goto fail; }
    if (cfg->mode == READ_PICO_TRANSFER_MODE_STA) {
        s_connect_index = choose_saved_network(&saved);
        saved.recent = s_connect_index;
    }
    struct stat st;
    if (stat(cfg->root_dir, &st) || !S_ISDIR(st.st_mode)) { err = ESP_ERR_NOT_FOUND; goto fail; }
    unsigned removed = 0, restored = 0;
    if (cleanup_interrupted(cfg->root_dir, &removed, &restored)) {
        ESP_LOGW("transfer", "cleanup failed removed=%u restored=%u", removed, restored);
        err = ESP_FAIL; goto fail;
    }
    if (s_font_root[0]) {
        if ((mkdir(s_font_root, 0755) && errno != EEXIST) || stat(s_font_root, &st) || !S_ISDIR(st.st_mode) ||
            cleanup_interrupted(s_font_root, &removed, &restored)) { err = ESP_FAIL; goto fail; }
    }
    if (removed || restored) ESP_LOGI("transfer", "cleanup removed=%u restored=%u", removed, restored);
    err = esp_netif_init();
    if (err != ESP_OK) goto fail;
    err = esp_event_loop_create_default();
    if (err == ESP_OK) s_loop_owned = true;
    else if (err != ESP_ERR_INVALID_STATE) goto fail;
    s_netif = cfg->mode == READ_PICO_TRANSFER_MODE_AP ? esp_netif_create_default_wifi_ap() : esp_netif_create_default_wifi_sta();
    if (!s_netif) { err = ESP_ERR_NO_MEM; goto fail; }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init); if (err != ESP_OK) goto fail;
    s_wifi = true;
    if (cfg->mode == READ_PICO_TRANSFER_MODE_AP) {
        uint8_t mac[6];
        err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP); if (err != ESP_OK) goto fail;
        snprintf((char *)wifi.ap.ssid, sizeof(wifi.ap.ssid), "ReadPico-%02X%02X", mac[4], mac[5]);
        strcpy((char *)wifi.ap.password, READ_PICO_TRANSFER_PASSWORD);
        wifi.ap.ssid_len = strlen((char *)wifi.ap.ssid); wifi.ap.channel = 1;
        wifi.ap.max_connection = 2; wifi.ap.authmode = WIFI_AUTH_WPA2_PSK;
        portENTER_CRITICAL(&s_lock); strcpy(s_status.ssid, (char *)wifi.ap.ssid); portEXIT_CRITICAL(&s_lock);
    } else {
        transfer_wifi_entry_t *recent = &saved.entries[s_connect_index];
        memcpy(wifi.sta.ssid, recent->ssid, strlen(recent->ssid));
        memcpy(wifi.sta.password, recent->password, strlen(recent->password));
        wifi.sta.threshold.authmode = recent->password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        wifi.sta.pmf_cfg.capable = true;
        wifi.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, &s_events);
    if (err != ESP_OK) goto fail;
    if (cfg->mode == READ_PICO_TRANSFER_MODE_STA) {
        err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event, NULL, &s_ip_events);
        if (err != ESP_OK) goto fail;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM); if (err != ESP_OK) goto fail;
    err = esp_wifi_set_mode(cfg->mode == READ_PICO_TRANSFER_MODE_AP ? WIFI_MODE_AP : WIFI_MODE_STA); if (err != ESP_OK) goto fail;
    err = esp_wifi_set_config(cfg->mode == READ_PICO_TRANSFER_MODE_AP ? WIFI_IF_AP : WIFI_IF_STA, &wifi); if (err != ESP_OK) goto fail;
    clear_secret(&wifi, sizeof(wifi)); clear_secret(&saved, sizeof(saved));
    s_buffer = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buffer) { err = ESP_ERR_NO_MEM; goto fail; }
    err = esp_wifi_start(); if (err != ESP_OK) goto fail;
    s_started = true;
    httpd_config_t http = HTTPD_DEFAULT_CONFIG();
    http.stack_size = 16384; http.max_uri_handlers = 20; http.recv_wait_timeout = 5;
    http.lru_purge_enable = true;
    err = httpd_start(&s_http, &http); if (err != ESP_OK) goto fail;
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = index_handler },
        { .uri = "/info", .method = HTTP_GET, .handler = info_handler },
        { .uri = "/agent", .method = HTTP_GET, .handler = agent_config_handler },
        { .uri = "/agent", .method = HTTP_POST, .handler = agent_config_handler },
        { .uri = "/agent", .method = HTTP_DELETE, .handler = agent_config_handler },
        { .uri = "/ambient", .method = HTTP_GET, .handler = ambient_config_handler },
        { .uri = "/ambient", .method = HTTP_POST, .handler = ambient_config_handler },
        { .uri = "/webclip", .method = HTTP_POST, .handler = webclip_handler },
        { .uri = "/webopen", .method = HTTP_POST, .handler = webopen_handler },
        { .uri = "/upload", .method = HTTP_PUT, .handler = upload_handler },
        { .uri = "/ota", .method = HTTP_PUT, .handler = ota_upload_handler },
        { .uri = "/fonts", .method = HTTP_PUT, .handler = upload_handler, .user_ctx = (void *)1 },
        { .uri = "/fonts", .method = HTTP_GET, .handler = font_info_handler },
        { .uri = "/books", .method = HTTP_GET, .handler = books_handler },
        { .uri = "/books", .method = HTTP_DELETE, .handler = books_handler },
        { .uri = "/books", .method = HTTP_POST, .handler = books_handler },
        { .uri = "/notes", .method = HTTP_GET, .handler = notes_download_handler },
        { .uri = "/wifi", .method = HTTP_POST, .handler = wifi_handler },
        { .uri = "/wifi", .method = HTTP_DELETE, .handler = wifi_handler },
    };
    for (size_t i = 0; i < sizeof(routes)/sizeof(*routes); ++i) {
        err = httpd_register_uri_handler(s_http, &routes[i]); if (err != ESP_OK) goto fail;
    }
    if (cfg->mode == READ_PICO_TRANSFER_MODE_AP) {
        portENTER_CRITICAL(&s_lock);
        s_status.network_ready = true; strcpy(s_status.url, READ_PICO_TRANSFER_URL);
        portEXIT_CRITICAL(&s_lock);
        set_error(ESP_OK);
        ESP_LOGI("transfer", "ap ready url=%s", READ_PICO_TRANSFER_URL);
    } else {
        int64_t now = esp_timer_get_time() / 1000;
        portENTER_CRITICAL(&s_lock); connection_begin(&s_connection, now); portEXIT_CRITICAL(&s_lock);
        read_pico_transfer_service_poll();
    }
    return ESP_OK;
fail:
    clear_secret(&wifi, sizeof(wifi)); clear_secret(&saved, sizeof(saved));
    read_pico_transfer_stop(); set_error(err); return err;
}
#endif
