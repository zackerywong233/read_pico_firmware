/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：已有 Wi-Fi 联网后校准系统时间，再同步 PMU 时钟。
 * English: Sync system time after joining Wi-Fi, then update the PMU clock.
 */
#pragma once

void app_time_sync_sta_ip(void);
void app_time_sync_poll(void);
void app_time_sync_stop(void);
