/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 读取、迁移并写入多 Wi-Fi 凭据 NVS blob。
 * Read, migrate and write the multi-network Wi-Fi NVS blob.
 * 冻结：迁移不得丢弃有效 v1 凭据；错误不得留下密码副本。
 * Frozen: Migration preserves valid v1 credentials; errors leave no password copies.
 */
#include "transfer_credentials_store.h"
#include <string.h>
#ifndef READ_PICO_TRANSFER_NVS_TEST
#include "nvs.h"
#endif

static bool legacy_valid(const transfer_credentials_v1_t *old) {
    if (old->version != 1 || !memchr(old->ssid, 0, sizeof(old->ssid)) ||
        !memchr(old->password, 0, sizeof(old->password))) return false;
    transfer_wifi_entry_t entry = {0};
    memcpy(entry.ssid, old->ssid, sizeof(entry.ssid));
    memcpy(entry.password, old->password, sizeof(entry.password));
    bool valid = wifi_entry_valid(&entry);
    clear_secret(&entry, sizeof(entry));
    return valid;
}

esp_err_t load_credentials(transfer_credentials_t *out) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    nvs_handle_t handle;
    esp_err_t err = nvs_open("rp_wifi", NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    size_t size = 0;
    err = nvs_get_blob(handle, "credentials", NULL, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) { nvs_close(handle); return ESP_OK; }
    if (err != ESP_OK) { nvs_close(handle); return err; }
    if (size == sizeof(*out)) {
        size_t actual = size;
        err = nvs_get_blob(handle, "credentials", out, &actual);
        nvs_close(handle);
        if (err != ESP_OK || actual != sizeof(*out) || !credentials_valid(out)) {
            clear_secret(out, sizeof(*out));
            return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
        }
        return ESP_OK;
    }
    transfer_credentials_v1_t old = {0};
    if (size == sizeof(old)) {
        size_t actual = size;
        err = nvs_get_blob(handle, "credentials", &old, &actual);
    }
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(old) || !legacy_valid(&old)) {
        clear_secret(&old, sizeof(old));
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    out->version = 2; out->count = 1; out->recent = 0;
    memcpy(out->entries[0].ssid, old.ssid, sizeof(old.ssid));
    memcpy(out->entries[0].password, old.password, sizeof(old.password));
    clear_secret(&old, sizeof(old));
    err = store_credentials(out);
    if (err != ESP_OK) clear_secret(out, sizeof(*out));
    return err;
}

esp_err_t store_credentials(const transfer_credentials_t *next) {
    if (next && !credentials_valid(next)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("rp_wifi", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = next ? nvs_set_blob(handle, "credentials", next, sizeof(*next)) : nvs_erase_key(handle, "credentials");
    if (!next && err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}
