/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：已有 Wi-Fi 联网后校准系统时间，并统一读写主机与 PMU 时钟。
 * English: Sync system time over Wi-Fi and unify host and PMU clock access.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define APP_TIME_MIN_VALID 946684800U
#define APP_TIME_MAX_VALID 4102444799U

void app_time_sync_sta_ip(void);
void app_time_sync_poll(void);
void app_time_sync_stop(void);

/// 根据最近一次刷新的 PMU 时间同步主机系统时钟。/ Sync the host system clock from the latest PMU snapshot.
void app_time_sync_from_pmu(void);
/// 主动读取 PMU 时间并同步主机系统时钟。/ Query the PMU clock and sync the host system clock.
void app_time_refresh_from_pmu(void);
/// 读取统一的 UTC 时间戳；未校准时返回 false。/ Read the unified UTC timestamp; false when unset.
bool app_time_now_utc(time_t *out_utc);
/// 读取按全局时区换算后的本地时间；未校准时返回 false。/ Read local time using the global timezone; false when unset.
bool app_time_now_local(struct tm *out_local, int *out_tz_min);
/// 设置主机时间并同步写入 PMU RTC。/ Set host time and persist it to the PMU RTC.
bool app_time_set_utc(uint32_t unix_sec, bool *pmu_synced);
/// 返回指定年月的天数（含闰年）。/ Return the number of days in the given year and month (leap-year aware).
int app_time_days_in_month(int year, int month);
/// 将本地年月日时分与时区偏移换算为 UTC Unix 秒。/ Convert local Y/M/D H:M and timezone minutes to UTC Unix seconds.
bool app_time_make_utc(int year, int month, int day, int hour, int minute, int tz_min, uint32_t *out_utc);
