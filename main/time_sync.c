/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：STA 获取 IP 后异步启动 SNTP；服务轮询时检查校时并统一主机与 PMU 时钟。
 * English: Start SNTP after STA gets an IP; poll sync and unify host and PMU clocks.
 */
#include "time_sync.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "read_pico_pmu.h"
#include "settings.h"

#define TIME_SYNC_RETRY_BACKOFF_US (30LL * 1000000LL)
#define TIME_SYNC_RESYNC_INTERVAL_US (30LL * 60LL * 1000000LL)

static const char *TAG = "time_sync";
static portMUX_TYPE s_sync_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_sntp_started;
static bool s_sntp_pending_pmu_sync;
static bool s_synced_this_boot;
static int64_t s_last_sync_us;
static int64_t s_retry_after_us;

static bool valid_utc(time_t value) {
    return value >= (time_t)APP_TIME_MIN_VALID && value <= (time_t)APP_TIME_MAX_VALID;
}

static bool write_pmu_time(uint32_t unix_sec) {
    if (!read_pico_pmu_ready()) return false;
    esp_err_t err = read_pico_pmu_time_sync(unix_sec);
    uint16_t status = read_pico_pmu_get()->last_op_status;
    bool accepted = err == ESP_OK
        && (status == PMU_STATUS_OK || status == PMU_STATUS_ACCEPTED);
    if (!accepted) return false;
    err = read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
    status = read_pico_pmu_get()->last_op_status;
    return err == ESP_OK
        && (status == PMU_STATUS_OK || status == PMU_STATUS_ACCEPTED)
        && read_pico_pmu_get()->time_synced;
}

// SNTP 回调运行在 lwIP 任务中，不直接访问 I2C；仅标记待主循环回写 PMU。
// Runs in the lwIP task without touching I2C; marks a pending flush for the main loop.
static void sntp_sync_cb(struct timeval *tv) {
    time_t synced_sec = (tv && valid_utc(tv->tv_sec)) ? tv->tv_sec : time(NULL);
    if (!valid_utc(synced_sec)) return;
    portENTER_CRITICAL(&s_sync_mux);
    s_sntp_pending_pmu_sync = true;
    s_retry_after_us = 0;
    portEXIT_CRITICAL(&s_sync_mux);
}

static bool flush_pending_sntp_to_pmu(bool honor_backoff) {
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_sync_mux);
    bool pending = s_sntp_pending_pmu_sync;
    bool wait_retry = honor_backoff && now_us < s_retry_after_us;
    portEXIT_CRITICAL(&s_sync_mux);
    if (!pending || wait_retry) return false;

    time_t now = time(NULL);
    if (!valid_utc(now)) return false;

    if (write_pmu_time((uint32_t)now)) {
        portENTER_CRITICAL(&s_sync_mux);
        s_sntp_pending_pmu_sync = false;
        s_synced_this_boot = true;
        s_last_sync_us = now_us;
        s_retry_after_us = 0;
        portEXIT_CRITICAL(&s_sync_mux);
        ESP_LOGI(TAG, "PMU clock synchronized from SNTP");
        return true;
    }

    portENTER_CRITICAL(&s_sync_mux);
    s_retry_after_us = now_us + TIME_SYNC_RETRY_BACKOFF_US;
    portEXIT_CRITICAL(&s_sync_mux);
    ESP_LOGW(TAG, "PMU clock sync failed");
    return false;
}

int app_time_days_in_month(int year, int month) {
    static const int k_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12) return 31;
    if (month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        return leap ? 29 : 28;
    }
    return k_days[month - 1];
}

bool app_time_make_utc(int year, int month, int day, int hour, int minute, int tz_min, uint32_t *out_utc) {
    if (!out_utc) return false;
    if (year < 2000 || year > 2099 || month < 1 || month > 12) return false;
    if (day < 1 || day > app_time_days_in_month(year, month)) return false;
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
    if (tz_min < -720 || tz_min > 840) return false;

    int64_t y = (int64_t)year - (month <= 2 ? 1 : 0);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t m = month;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    int64_t local_sec = days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60;
    int64_t utc = local_sec - (int64_t)tz_min * 60;
    if (utc < (int64_t)APP_TIME_MIN_VALID || utc > (int64_t)APP_TIME_MAX_VALID) return false;
    *out_utc = (uint32_t)utc;
    return true;
}

void app_time_sync_from_pmu(void) {
    // 若 SNTP 已完成但尚未落盘到 PMU，优先回写 PMU，避免旧 PMU 时间反向覆盖主机。
    // Flush any pending SNTP update to PMU first so stale PMU time never overwrites the host.
    if (flush_pending_sntp_to_pmu(false)) return;

    time_t sys_now = time(NULL);
    if (valid_utc(sys_now)) {
        // 主机 40 MHz 晶振精度远高于 CW32 片内 LSI（±1–2%）；主机时钟已有效时不再反向拉偏。
        // ESP32's 40 MHz crystal is far more accurate than CW32's ±1-2% LSI; keep valid host time.
        return;
    }
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    if (!pmu || !pmu->time_ok || !valid_utc((time_t)pmu->unix_sec)) return;
    struct timeval tv = { .tv_sec = (time_t)pmu->unix_sec, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

void app_time_refresh_from_pmu(void) {
    if (read_pico_pmu_ready()) {
        (void)read_pico_pmu_cmd(PMU_CMD_TIME_GET, NULL, 0);
    }
    app_time_sync_from_pmu();
}

bool app_time_now_utc(time_t *out_utc) {
    time_t sys_now = time(NULL);
    if (valid_utc(sys_now)) {
        if (out_utc) *out_utc = sys_now;
        return true;
    }
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    if (pmu && pmu->time_ok && valid_utc((time_t)pmu->unix_sec)) {
        struct timeval tv = { .tv_sec = (time_t)pmu->unix_sec, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        if (out_utc) *out_utc = (time_t)pmu->unix_sec;
        return true;
    }
    return false;
}

bool app_time_now_local(struct tm *out_local, int *out_tz_min) {
    time_t utc = 0;
    if (!app_time_now_utc(&utc)) return false;
    int tz_min = app_settings_timezone_min();
    time_t local_sec = utc + (time_t)tz_min * 60;
    if (out_tz_min) *out_tz_min = tz_min;
    if (out_local && !gmtime_r(&local_sec, out_local)) return false;
    return true;
}

bool app_time_set_utc(uint32_t unix_sec, bool *pmu_synced) {
    if (pmu_synced) *pmu_synced = false;
    if (!valid_utc((time_t)unix_sec)) return false;
    struct timeval tv = { .tv_sec = (time_t)unix_sec, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    // 显式设时覆盖任何挂起的旧 SNTP 回写请求。/ Explicit time set supersedes any stale pending SNTP flush.
    portENTER_CRITICAL(&s_sync_mux);
    s_sntp_pending_pmu_sync = false;
    s_retry_after_us = 0;
    portEXIT_CRITICAL(&s_sync_mux);

    bool synced = write_pmu_time(unix_sec);
    if (pmu_synced) *pmu_synced = synced;
    return true;
}

bool app_time_flush_to_pmu(void) {
    time_t now = time(NULL);
    if (!valid_utc(now)) return false;
    if (!write_pmu_time((uint32_t)now)) return false;
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_sync_mux);
    if (s_sntp_pending_pmu_sync) {
        s_sntp_pending_pmu_sync = false;
        s_synced_this_boot = true;
        s_last_sync_us = now_us;
        s_retry_after_us = 0;
    }
    portEXIT_CRITICAL(&s_sync_mux);
    return true;
}

void app_time_sync_sta_ip(void) {
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_sync_mux);
    bool recent = s_synced_this_boot && (now_us - s_last_sync_us < TIME_SYNC_RESYNC_INTERVAL_US);
    bool skip = s_sntp_started || recent;
    if (!skip) s_sntp_started = true;
    portEXIT_CRITICAL(&s_sync_mux);
    if (skip) return;

#if defined(CONFIG_LWIP_SNTP_MAX_SERVERS) && (CONFIG_LWIP_SNTP_MAX_SERVERS >= 3)
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "ntp.tencent.com", "pool.ntp.org"));
#else
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
#endif
    config.sync_cb = sntp_sync_cb;
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
    (void)flush_pending_sntp_to_pmu(true);
}

void app_time_sync_stop(void) {
    portENTER_CRITICAL(&s_sync_mux);
    bool was_started = s_sntp_started;
    s_sntp_started = false;
    portEXIT_CRITICAL(&s_sync_mux);
    if (was_started) {
        esp_netif_sntp_deinit();
    }
    // 退网前若有已收到的 SNTP 校时结果，立即回写 PMU。
    // Flush any received SNTP calibration to PMU immediately before leaving the network page.
    (void)flush_pending_sntp_to_pmu(false);
}
