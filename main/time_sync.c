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
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "read_pico_pmu.h"

static const char *TAG = "time_sync";
static portMUX_TYPE s_sync_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_sntp_started;
static bool s_synced_this_boot;
static int64_t s_retry_after_us;

void app_time_sync_sta_ip(void) {
    portENTER_CRITICAL(&s_sync_mux);
    bool skip = s_sntp_started || s_synced_this_boot;
    if (!skip) s_sntp_started = true;
    portEXIT_CRITICAL(&s_sync_mux);
    if (skip) return;
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SNTP started after STA acquired IP");
    } else {
        portENTER_CRITICAL(&s_sync_mux);
        s_sntp_started = false;
        portEXIT_CRITICAL(&s_sync_mux);
        ESP_LOGW(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
}

void app_time_sync_poll(void) {
    portENTER_CRITICAL(&s_sync_mux);
    bool active = s_sntp_started && !s_synced_this_boot;
    portEXIT_CRITICAL(&s_sync_mux);
    if (!active) return;
    int64_t now_us = esp_timer_get_time();
    if (now_us < s_retry_after_us) return;

    time_t now = time(NULL);
    if (now < 1704067200 || now > UINT32_MAX) return;
    uint32_t unix_sec = (uint32_t)now;
    esp_err_t err = read_pico_pmu_time_sync(unix_sec);
    uint16_t status = read_pico_pmu_get()->last_op_status;
    bool accepted = err == ESP_OK && (status == PMU_STATUS_OK || status == PMU_STATUS_ACCEPTED);
    if (accepted) err = read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
    status = read_pico_pmu_get()->last_op_status;
    if (err == ESP_OK && (status == PMU_STATUS_OK || status == PMU_STATUS_ACCEPTED)
        && read_pico_pmu_get()->time_synced) {
        portENTER_CRITICAL(&s_sync_mux);
        s_synced_this_boot = true;
        portEXIT_CRITICAL(&s_sync_mux);
        ESP_LOGI(TAG, "PMU clock synchronized from SNTP");
    } else {
        s_retry_after_us = now_us + 30000000;
        ESP_LOGW(TAG, "PMU clock sync failed: %s", esp_err_to_name(err));
    }
}

void app_time_sync_stop(void) {
    portENTER_CRITICAL(&s_sync_mux);
    bool was_started = s_sntp_started;
    s_sntp_started = false;
    portEXIT_CRITICAL(&s_sync_mux);
    if (was_started) {
        esp_netif_sntp_deinit();
    }
}
