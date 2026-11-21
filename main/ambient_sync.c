/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：提供定时看板的免打扰唤醒时间计算。
 * English: Computes scheduled dashboard wake times around quiet hours.
 *
 * 冻结：失败或处于免打扰时段时不改写设备画面。
 * Frozen: Do not alter the display after a failed refresh or during quiet hours.
 */
#include "ambient_sync.h"

uint32_t ambient_sync_next_delay(uint32_t seconds_since_midnight, uint32_t interval_seconds,
                                 uint8_t quiet_start_h, uint8_t quiet_end_h) {
    if (interval_seconds == 0 || quiet_start_h > 23 || quiet_end_h > 23) return 0;
    uint32_t now = seconds_since_midnight % 86400U;
    uint32_t candidate = (now + interval_seconds) % 86400U;
    uint32_t end = (uint32_t)quiet_end_h * 3600U;
    if (!ambient_sync_is_quiet(candidate, quiet_start_h, quiet_end_h)) return interval_seconds;
    uint32_t delta = candidate < end ? end - candidate : 86400U - candidate + end;
    return interval_seconds + delta;
}

bool ambient_sync_is_quiet(uint32_t seconds_since_midnight,
                           uint8_t quiet_start_h, uint8_t quiet_end_h) {
    if (quiet_start_h > 23 || quiet_end_h > 23 || quiet_start_h == quiet_end_h) return false;
    uint32_t now = seconds_since_midnight % 86400U;
    uint32_t start = (uint32_t)quiet_start_h * 3600U;
    uint32_t end = (uint32_t)quiet_end_h * 3600U;
    return start < end ? now >= start && now < end : now >= start || now < end;
}
