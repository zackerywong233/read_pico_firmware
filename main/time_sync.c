/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：STA 获取 IP 后异步启动 SNTP；服务轮询时检查校时并更新 PMU。
 * English: Start SNTP after STA gets an IP; poll sync and update the PMU clock.
 */
#include "time_sync.h"

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "read_pico_pmu.h"
#include "read_pico_pmu_protocol.h"

static const char *TAG = "time_sync";
static bool s_sntp_started;
static bool s_synced_this_boot;
static int64_t s_retry_after_us;

void app_time_sync_sta_ip(void) {
    if (s_sntp_started || s_synced_this_boot) return;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started after STA acquired IP");
    } else {
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
}

void app_time_sync_poll(void) {
    if (!s_sntp_started || s_synced_this_boot) return;
    int64_t now_us = esp_timer_get_time();
    if (now_us < s_retry_after_us) return;

    time_t now = time(NULL);
    if (now < 1704067200 || now > UINT32_MAX) return;
    uint32_t unix_sec = (uint32_t)now;
    uint8_t payload[4] = {
        (uint8_t)unix_sec,
        (uint8_t)(unix_sec >> 8),
        (uint8_t)(unix_sec >> 16),
        (uint8_t)(unix_sec >> 24),
    };
    esp_err_t err = read_pico_pmu_cmd(PMU_CMD_TIME_SYNC, payload, sizeof(payload));
    uint16_t status = read_pico_pmu_get()->last_op_status;
    bool accepted = err == ESP_OK && (status == PMU_STATUS_OK || status == PMU_STATUS_ACCEPTED);
    if (accepted) err = read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
    status = read_pico_pmu_get()->last_op_status;
    if (err == ESP_OK && (status == PMU_STATUS_OK || status == PMU_STATUS_ACCEPTED)
        && read_pico_pmu_get()->time_synced) {
        s_synced_this_boot = true;
        ESP_LOGI(TAG, "PMU clock synchronized from SNTP");
    } else {
        s_retry_after_us = now_us + 30000000;
        ESP_LOGW(TAG, "PMU clock sync failed: %s", esp_err_to_name(err));
    }
}

void app_time_sync_stop(void) {
    if (s_sntp_started) {
        esp_netif_sntp_deinit();
        s_sntp_started = false;
    }
}
