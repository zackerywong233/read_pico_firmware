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
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "read_pico_transfer.h"

#define RELEASE_URL "https://api.github.com/repos/zackerywong233/read_pico_firmware/releases/latest"
#define RELEASE_JSON_MAX (48U * 1024U)
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

static esp_err_t read_http(const char *url, char *out, size_t cap, int *status) {
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = 20000, .buffer_size = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "User-Agent", "ReadPico-OTA");
    esp_http_client_set_header(client, "Accept", "application/vnd.github+json");
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        *status = esp_http_client_get_status_code(client);
        size_t used = 0;
        while (used + 1 < cap) {
            int n = esp_http_client_read(client, out + used, cap - used - 1);
            if (n < 0) { err = ESP_FAIL; break; }
            if (!n) break;
            used += (size_t)n;
        }
        if (used + 1 >= cap) err = ESP_ERR_INVALID_SIZE;
        out[used] = 0;
    }
    esp_http_client_close(client); esp_http_client_cleanup(client);
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
        esp_err_t wifi_err = read_pico_transfer_ota_wifi_start(20000);
        if (wifi_err != ESP_OK) { status_set(OTA_UPDATE_ERROR, 0, "无法连接已保存的 Wi-Fi 网络"); goto done; }
        char *buffer = malloc(8192);
        if (!buffer) { read_pico_transfer_ota_wifi_stop(); status_set(OTA_UPDATE_ERROR, 0, "内存不足"); goto done; }
        esp_http_client_config_t cfg = {.url = s_asset_url, .timeout_ms = 30000, .buffer_size = 8192,
            .crt_bundle_attach = esp_crt_bundle_attach};
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        esp_ota_handle_t handle = 0; bool begun = false;
        const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
        esp_err_t err = client && partition ? esp_http_client_open(client, 0) : ESP_FAIL;
        if (err == ESP_OK) {
            (void)esp_http_client_fetch_headers(client);
            if (esp_http_client_get_status_code(client) != 200) err = ESP_FAIL;
            else { err = esp_ota_begin(partition, OTA_SIZE_UNKNOWN, &handle); begun = err == ESP_OK; }
        }
        mbedtls_sha256_context sha; mbedtls_sha256_init(&sha); mbedtls_sha256_starts_ret(&sha, 0);
        int total = client ? esp_http_client_get_content_length(client) : -1; size_t received = 0;
        while (err == ESP_OK && !s_cancel) {
            int n = esp_http_client_read(client, buffer, 8192);
            if (n < 0) { err = ESP_FAIL; break; }
            if (!n) break;
            if (esp_ota_write(handle, buffer, n) != ESP_OK || mbedtls_sha256_update_ret(&sha, (uint8_t *)buffer, n) != 0) { err = ESP_FAIL; break; }
            received += (size_t)n;
            unsigned pct = total > 0 ? (unsigned)(received * 100U / (unsigned)total) : 0;
            status_set(OTA_UPDATE_DOWNLOADING, pct > 99 ? 99 : pct, NULL);
            vTaskDelay(1);
        }
        unsigned char digest[32]; char hex[65];
        if (err == ESP_OK && s_cancel) err = ESP_ERR_INVALID_STATE;
        if (err == ESP_OK && mbedtls_sha256_finish_ret(&sha, digest) == 0) {
            for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", digest[i]);
            if (strcmp(hex, s_digest)) err = ESP_ERR_INVALID_CRC;
        } else if (err == ESP_OK) err = ESP_FAIL;
        mbedtls_sha256_free(&sha); free(buffer);
        if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
        read_pico_transfer_ota_wifi_stop();
        if (err == ESP_OK) err = esp_ota_end(handle);
        else if (begun) esp_ota_abort(handle);
        if (err == ESP_OK) err = esp_ota_set_boot_partition(partition);
        if (err == ESP_OK) { status_set(OTA_UPDATE_DONE, 100, "升级完成，正在重启"); vTaskDelay(pdMS_TO_TICKS(800)); esp_restart(); }
        else status_set(s_cancel ? OTA_UPDATE_CANCELLED : OTA_UPDATE_ERROR, 0, s_cancel ? "已取消" : "升级失败，当前固件未更改");
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
    if (xTaskCreate(ota_task, "ota_update", 8192, (void *)(uintptr_t)install, 5, NULL) != pdPASS) {
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
