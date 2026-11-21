/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：在独立 NVS 命名空间保存 Agent Broker 配置。
 * English: Store Agent broker configuration in a dedicated NVS namespace.
 * 冻结：不把 Broker 密码写入可浏览存储或公开状态。
 * Frozen: Never write Broker passwords to browseable storage or public status.
 */
#pragma once

#include <string.h>

#include "read_pico_transfer.h"
#ifndef READ_PICO_AGENT_NVS_TEST
#include "nvs.h"
#endif

static esp_err_t agent_config_store_write(const read_pico_agent_config_t *config) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("rp_agent", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    if (config) err = nvs_set_blob(handle, "config", config, sizeof(*config));
    else {
        err = nvs_erase_key(handle, "config");
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t agent_config_store_read(read_pico_agent_config_t *out) {
    memset(out, 0, sizeof(*out));
    nvs_handle_t handle;
    esp_err_t err = nvs_open("rp_agent", NVS_READONLY, &handle);
    if (err != ESP_OK) return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NVS_NOT_FOUND : err;
    size_t size = sizeof(*out);
    err = nvs_get_blob(handle, "config", out, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(*out)) {
        memset(out, 0, sizeof(*out));
        return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
    return ESP_OK;
}
