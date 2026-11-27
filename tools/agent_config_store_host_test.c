/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：验证 Agent NVS 配置的独立命名空间读写契约。
 * English: Verify Agent configuration storage in its dedicated NVS namespace.
 * 冻结：仅供宿主测试。/ Frozen: Host tests only.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_err.h"

typedef int nvs_handle_t;
typedef enum { NVS_READONLY, NVS_READWRITE } nvs_open_mode_t;
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size);
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key);
esp_err_t nvs_commit(nvs_handle_t handle);
void nvs_close(nvs_handle_t handle);

#define READ_PICO_AGENT_NVS_TEST
#include "agent_config_store.h"

static uint8_t blob[sizeof(read_pico_agent_config_t)];
static size_t blob_size;
static bool present;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out) {
    assert(strcmp(name, "rp_agent") == 0);
    (void)mode;
    *out = 1;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t size) {
    assert(handle == 1 && strcmp(key, "config") == 0 && size <= sizeof(blob));
    memcpy(blob, value, size);
    blob_size = size;
    present = true;
    return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) {
    assert(handle == 1 && strcmp(key, "config") == 0);
    if (!present) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < blob_size) return ESP_ERR_INVALID_STATE;
    memcpy(out, blob, blob_size);
    *size = blob_size;
    return ESP_OK;
}
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    assert(handle == 1 && strcmp(key, "config") == 0);
    if (!present) return ESP_ERR_NVS_NOT_FOUND;
    present = false;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) { assert(handle == 1); return ESP_OK; }
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }

int main(void) {
    read_pico_agent_config_t input = {
        .broker_uri = "mqtts://broker.example:8883",
        .username = "reader",
        .password = "secret",
        .topic = "device/inbox",
    };
    read_pico_agent_config_t output = {0};
    assert(agent_config_store_read(&output) == ESP_ERR_NVS_NOT_FOUND);
    assert(agent_config_store_write(&input) == ESP_OK);
    assert(agent_config_store_read(&output) == ESP_OK);
    assert(memcmp(&input, &output, sizeof(input)) == 0);
    assert(agent_config_store_write(NULL) == ESP_OK);
    memset(&output, 0xa5, sizeof(output));
    assert(agent_config_store_read(&output) == ESP_ERR_NVS_NOT_FOUND);
    assert(output.broker_uri[0] == '\0' && output.password[0] == '\0');
    return 0;
}
