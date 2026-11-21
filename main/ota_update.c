/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * GitHub Release 元数据、HTTPS 下载和备用 OTA 分区写入。
 * GitHub Release metadata, HTTPS download and inactive OTA slot writes.
 * 冻结：下载必须使用证书 bundle 与 SHA-256；未校验不得选择新启动槽。
 * Frozen: Downloads require the certificate bundle and SHA-256; never select an unverified slot.
 */
#include "ota_update.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "read_pico_transfer.h"

#define TAG "ota_update"
#define RELEASE_URL "https://api.github.com/repos/zackerywong233/read_pico_firmware/releases/latest"
#define RELEASE_JSON_MAX (48U * 1024U)
#define OTA_REDIRECT_URL_MAX 2048U
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ota_update_status_t s_status;
static char s_asset_url[512], s_digest[65];
static volatile bool s_cancel;
static bool s_task_active;

static bool valid_version(const char *v) {
    if (!v || (*v != 'v' && *v != 'V')) return false;
    return ota_update_compare_versions(v, "v0.0.0") >= 0;
}

static cJSON *object_item(cJSON *obj, const char *key) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item : NULL;
}

static bool release_asset_url(const char *url) {
    static const char prefix[] = "https://github.com/zackerywong233/read_pico_firmware/releases/download/";
    return url && strncmp(url, prefix, sizeof(prefix) - 1) == 0;
}

bool ota_update_parse_release(const char *json, char version[32], char url[512], char checksum_url[512], char sha[65]) {
    if (!json || !version || !url || !checksum_url || !sha) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    cJSON *tag = object_item(root, "tag_name");
    cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    bool ok = tag && valid_version(tag->valuestring) && strlen(tag->valuestring) < 32 &&
              cJSON_IsArray(assets);
    char digest[65] = {0}, asset_url[512] = {0}, checksum_asset_url[512] = {0};
    if (ok) {
        cJSON *asset;
        cJSON_ArrayForEach(asset, assets) {
            cJSON *name = object_item(asset, "name");
            cJSON *dl = object_item(asset, "browser_download_url");
            cJSON *dg = object_item(asset, "digest");
            if (!name || !dl || !release_asset_url(dl->valuestring)) continue;
            if (!strcmp(name->valuestring, "pico-os-app.bin") && strlen(dl->valuestring) < sizeof(asset_url)) {
                if (dg) (void)ota_update_parse_digest(dg->valuestring, digest);
                strcpy(asset_url, dl->valuestring);
            } else if (!strcmp(name->valuestring, "pico-os-app.bin.sha256") && strlen(dl->valuestring) < sizeof(checksum_asset_url)) {
                strcpy(checksum_asset_url, dl->valuestring);
            }
        }
        ok = asset_url[0] && (digest[0] || checksum_asset_url[0]);
    }
    if (ok) {
        strcpy(version, tag->valuestring); strcpy(url, asset_url);
        strcpy(checksum_url, digest[0] ? "" : checksum_asset_url); strcpy(sha, digest);
    }
    cJSON_Delete(root);
    return ok;
}

static void status_set(ota_update_state_t state, unsigned progress, const char *message) {
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.progress_percent = progress;
    if (message) snprintf(s_status.message, sizeof(s_status.message), "%s", message);
    portEXIT_CRITICAL(&s_lock);
}

static bool is_http_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

typedef struct {
    char location[OTA_REDIRECT_URL_MAX];
    bool overflow;
} ota_redirect_ctx_t;

static esp_err_t capture_location_event(esp_http_client_event_t *evt) {
    if (!evt || evt->event_id != HTTP_EVENT_ON_HEADER || !evt->user_data ||
        !evt->header_key || !evt->header_value) return ESP_OK;
    if (strcasecmp(evt->header_key, "Location") != 0) return ESP_OK;
    ota_redirect_ctx_t *ctx = evt->user_data;
    size_t len = strlen(evt->header_value);
    if (len >= sizeof(ctx->location)) {
        ctx->location[0] = '\0';
        ctx->overflow = true;
    } else {
        memcpy(ctx->location, evt->header_value, len + 1);
        ctx->overflow = false;
    }
    return ESP_OK;
}

static esp_err_t open_https_with_redirects(const char *initial_url, const char *accept_hdr,
                                           int timeout_ms, esp_http_client_handle_t *client_out,
                                           int *status_out) {
    if (!initial_url || !client_out) return ESP_ERR_INVALID_ARG;
    *client_out = NULL;
    if (status_out) *status_out = 0;

    char *url = malloc(OTA_REDIRECT_URL_MAX);
    ota_redirect_ctx_t *rctx = calloc(1, sizeof(*rctx));
    if (!url || !rctx) {
        free(url);
        free(rctx);
        return ESP_ERR_NO_MEM;
    }
    if (strlen(initial_url) >= OTA_REDIRECT_URL_MAX) {
        free(url);
        free(rctx);
        return ESP_ERR_INVALID_SIZE;
    }
    strcpy(url, initial_url);

    // 跨主机重定向时重建干净的 HTTPS 客户端，避免旧主机 TLS/缓冲状态残留。
    // Recreate a clean HTTPS client across host redirects so TLS and buffer state stay clean.
    esp_err_t result = ESP_ERR_INVALID_RESPONSE;
    for (int hop = 0; hop <= 5; ++hop) {
        rctx->location[0] = '\0';
        rctx->overflow = false;
        esp_http_client_config_t cfg = {
            .url = url,
            .timeout_ms = timeout_ms,
            .buffer_size = 16384,
            .buffer_size_tx = 4096,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .disable_auto_redirect = true,
            .event_handler = capture_location_event,
            .user_data = rctx,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) { result = ESP_ERR_NO_MEM; break; }
        esp_http_client_set_header(client, "User-Agent", "ReadPico-OTA");
        if (accept_hdr) esp_http_client_set_header(client, "Accept", accept_hdr);

        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "hop=%d open failed: %s", hop, esp_err_to_name(err));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            result = err;
            break;
        }
        int64_t hdr = esp_http_client_fetch_headers(client);
        if (hdr < 0) {
            ESP_LOGE(TAG, "hop=%d fetch_headers failed", hop);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            result = ESP_FAIL;
            break;
        }
        int status = esp_http_client_get_status_code(client);
        if (!is_http_redirect(status)) {
            if (status_out) *status_out = status;
            *client_out = client;
            result = ESP_OK;
            break;
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        if (hop == 5 || rctx->overflow || strncmp(rctx->location, "https://", 8) != 0) {
            ESP_LOGE(TAG, "hop=%d redirect rejected status=%d overflow=%d", hop, status, rctx->overflow);
            result = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        strcpy(url, rctx->location);
    }

    free(url);
    free(rctx);
    return result;
}

static esp_err_t read_http(const char *url, char *out, size_t cap, int *status) {
    esp_http_client_handle_t client = NULL;
    esp_err_t err = open_https_with_redirects(url, "application/vnd.github+json", 20000, &client, status);
    if (err == ESP_OK) {
        size_t used = 0;
        unsigned eagain = 0;
        while (used + 1 < cap) {
            int n = esp_http_client_read(client, out + used, cap - used - 1);
            if (n == -ESP_ERR_HTTP_EAGAIN) {
                if (++eagain <= 3) continue;
                err = ESP_ERR_TIMEOUT;
                break;
            }
            if (n < 0) { err = ESP_FAIL; break; }
            if (!n) break;
            eagain = 0;
            used += (size_t)n;
        }
        if (used + 1 >= cap) err = ESP_ERR_INVALID_SIZE;
        out[used] = 0;
    }
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    return err;
}

static void ota_task(void *arg) {
    bool install = (bool)(uintptr_t)arg;
    if (!install) {
        status_set(OTA_UPDATE_CHECKING, 0, "正在检查 GitHub Releases");
        esp_err_t wifi_err = read_pico_transfer_ota_wifi_start(20000);
        if (wifi_err != ESP_OK) { status_set(OTA_UPDATE_ERROR, 0, "无法连接已保存的 Wi-Fi 网络"); goto done; }
        char *json = malloc(RELEASE_JSON_MAX + 1);
        if (!json) { read_pico_transfer_ota_wifi_stop(); status_set(OTA_UPDATE_ERROR, 0, "内存不足"); goto done; }
        int code = 0; char version[32], url[512], checksum_url[512], sha[65];
        esp_err_t err = read_http(RELEASE_URL, json, RELEASE_JSON_MAX, &code);
        if (err != ESP_OK || code != 200 || !ota_update_parse_release(json, version, url, checksum_url, sha)) {
            read_pico_transfer_ota_wifi_stop();
            free(json); status_set(OTA_UPDATE_ERROR, 0, "检查失败，请确认 Wi-Fi 与网络"); goto done;
        }
        free(json);
        if (!sha[0]) {
            char checksum[256]; int checksum_code = 0;
            err = read_http(checksum_url, checksum, sizeof(checksum), &checksum_code);
            if (err != ESP_OK || checksum_code != 200 || !ota_update_parse_checksum_file(checksum, sha)) {
                read_pico_transfer_ota_wifi_stop();
                status_set(OTA_UPDATE_ERROR, 0, "Release SHA-256 校验文件无效"); goto done;
            }
        }
        read_pico_transfer_ota_wifi_stop();
        const esp_app_desc_t *desc = esp_app_get_description();
        portENTER_CRITICAL(&s_lock);
        snprintf(s_status.current_version, sizeof(s_status.current_version), "%s", desc->version);
        snprintf(s_status.latest_version, sizeof(s_status.latest_version), "%s", version);
        strcpy(s_asset_url, url); strcpy(s_digest, sha);
        bool newer = ota_update_compare_versions(version, desc->version) > 0;
        portEXIT_CRITICAL(&s_lock);
        status_set(newer ? OTA_UPDATE_AVAILABLE : OTA_UPDATE_CURRENT, 0,
                   newer ? "发现新版本" : "当前已是最新版本");
    } else {
        status_set(OTA_UPDATE_DOWNLOADING, 0, "正在下载并写入");
        const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
        if (!partition) {
            ESP_LOGE(TAG, "no inactive OTA partition available");
            status_set(OTA_UPDATE_ERROR, 0, "无可用更新分区，请先刷机");
            goto done;
        }
        esp_err_t wifi_err = read_pico_transfer_ota_wifi_start(20000);
        if (wifi_err != ESP_OK) { status_set(OTA_UPDATE_ERROR, 0, "无法连接已保存的 Wi-Fi 网络"); goto done; }
        char *buffer = malloc(8192);
        if (!buffer) { read_pico_transfer_ota_wifi_stop(); status_set(OTA_UPDATE_ERROR, 0, "内存不足"); goto done; }

        esp_http_client_handle_t client = NULL;
        esp_ota_handle_t handle = 0;
        bool begun = false;
        int dl_status = 0;
        const char *fail_msg = "下载连接失败，请重试";
        esp_err_t err = open_https_with_redirects(s_asset_url, "application/octet-stream", 30000, &client, &dl_status);
        int64_t total = err == ESP_OK && client ? esp_http_client_get_content_length(client) : -1;
        if (err == ESP_OK && dl_status != 200) {
            ESP_LOGE(TAG, "download http status=%d", dl_status);
            err = ESP_FAIL;
        }
        if (err == ESP_OK && total > 0 && (uint64_t)total > partition->size) {
            ESP_LOGE(TAG, "firmware too large: %lld > %u", (long long)total, (unsigned)partition->size);
            fail_msg = "固件大小超出分区上限";
            err = ESP_ERR_INVALID_SIZE;
        }
        if (err == ESP_OK) {
            // 顺序写入按扇区按需擦除，避免在已建连的 HTTPS 上阻塞整块擦除导致断流。
            // Sequential writes erase on demand per sector instead of stalling an open HTTPS stream.
            err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
            begun = err == ESP_OK;
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
                fail_msg = "写入备用分区失败";
            }
        }
        psa_hash_operation_t sha = PSA_HASH_OPERATION_INIT;
        bool sha_active = false;
        if (err == ESP_OK) {
            if (psa_crypto_init() != PSA_SUCCESS ||
                psa_hash_setup(&sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
                fail_msg = "固件校验失败，当前固件未更改";
                err = ESP_FAIL;
            } else {
                sha_active = true;
            }
        }
        size_t received = 0;
        unsigned eagain = 0;
        while (err == ESP_OK && !s_cancel) {
            int n = esp_http_client_read(client, buffer, 8192);
            if (n == -ESP_ERR_HTTP_EAGAIN) {
                if (++eagain <= 3) continue;
                ESP_LOGE(TAG, "download timeout at %u/%lld", (unsigned)received, (long long)total);
                fail_msg = "下载中断，当前固件未更改";
                err = ESP_ERR_TIMEOUT;
                break;
            }
            if (n < 0) {
                ESP_LOGE(TAG, "download read err=%d at %u/%lld", n, (unsigned)received, (long long)total);
                fail_msg = "下载中断，当前固件未更改";
                err = ESP_FAIL;
                break;
            }
            if (!n) {
                if (!received || !esp_http_client_is_complete_data_received(client) ||
                    (total > 0 && (int64_t)received != total)) {
                    ESP_LOGE(TAG, "download truncated: received=%u total=%lld complete=%d",
                             (unsigned)received, (long long)total,
                             esp_http_client_is_complete_data_received(client));
                    fail_msg = "下载中断，当前固件未更改";
                    err = ESP_ERR_INVALID_SIZE;
                }
                break;
            }
            eagain = 0;
            if (received + (size_t)n > partition->size) {
                fail_msg = "固件大小超出分区上限";
                err = ESP_ERR_INVALID_SIZE;
                break;
            }
            if (esp_ota_write(handle, buffer, (size_t)n) != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed at %u", (unsigned)received);
                fail_msg = "写入备用分区失败";
                err = ESP_FAIL;
                break;
            }
            if (psa_hash_update(&sha, (const uint8_t *)buffer, (size_t)n) != PSA_SUCCESS) {
                fail_msg = "固件校验失败，当前固件未更改";
                err = ESP_FAIL;
                break;
            }
            received += (size_t)n;
            unsigned pct = total > 0 ? (unsigned)((uint64_t)received * 100ULL / (uint64_t)total) : 0;
            status_set(OTA_UPDATE_DOWNLOADING, pct > 99 ? 99 : pct, NULL);
            vTaskDelay(1);
        }
        unsigned char digest[32]; char hex[65]; size_t digest_len = 0;
        if (err == ESP_OK && s_cancel) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK && sha_active &&
            psa_hash_finish(&sha, digest, sizeof(digest), &digest_len) == PSA_SUCCESS &&
            digest_len == sizeof(digest)) {
            sha_active = false;
            for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
            if (strcmp(hex, s_digest)) {
                ESP_LOGE(TAG, "sha256 mismatch got=%s want=%s", hex, s_digest);
                fail_msg = "固件校验失败，当前固件未更改";
                err = ESP_ERR_INVALID_CRC;
            }
        } else if (err == ESP_OK) {
            fail_msg = "固件校验失败，当前固件未更改";
            err = ESP_FAIL;
        }
        if (sha_active) psa_hash_abort(&sha);
        free(buffer);
        if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
        read_pico_transfer_ota_wifi_stop();
        if (err == ESP_OK) {
            err = esp_ota_end(handle);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
                fail_msg = "固件验证失败，当前固件未更改";
            }
        } else if (begun) {
            esp_ota_abort(handle);
        }
        if (err == ESP_OK) {
            err = esp_ota_set_boot_partition(partition);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
                fail_msg = "固件验证失败，当前固件未更改";
            }
        }
        if (err == ESP_OK) { status_set(OTA_UPDATE_DONE, 100, "升级完成，正在重启"); vTaskDelay(pdMS_TO_TICKS(800)); esp_restart(); }
        else status_set(s_cancel ? OTA_UPDATE_CANCELLED : OTA_UPDATE_ERROR, 0, s_cancel ? "已取消" : fail_msg);
    }
done:
    portENTER_CRITICAL(&s_lock); s_task_active = false; portEXIT_CRITICAL(&s_lock);
    vTaskDelete(NULL);
}

static bool start(bool install) {
    portENTER_CRITICAL(&s_lock);
    if (s_task_active || (install && s_status.state != OTA_UPDATE_AVAILABLE)) { portEXIT_CRITICAL(&s_lock); return false; }
    s_task_active = true; s_cancel = false;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreate(ota_task, "ota_update", 12288, (void *)(uintptr_t)install, 5, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_lock); s_task_active = false; portEXIT_CRITICAL(&s_lock);
        status_set(OTA_UPDATE_ERROR, 0, "无法启动升级任务"); return false;
    }
    return true;
}

bool ota_update_check_start(void) { return start(false); }
bool ota_update_install_start(void) { return start(true); }
void ota_update_cancel(void) { s_cancel = true; }
void ota_update_get_status(ota_update_status_t *out) {
    if (!out) return;
    portENTER_CRITICAL(&s_lock); *out = s_status; portEXIT_CRITICAL(&s_lock);
}
