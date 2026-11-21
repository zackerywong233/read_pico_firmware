/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：定时看板刷新策略与自动唤醒执行入口。
 * English: Scheduled dashboard policy and automatic-wake entry point.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>


/// 返回避开免打扰时段的下一次刷新延迟秒数。
/// / Return seconds until the next refresh, shifted past quiet hours.
uint32_t ambient_sync_next_delay(uint32_t seconds_since_midnight, uint32_t interval_seconds,
                                 uint8_t quiet_start_h, uint8_t quiet_end_h);
/// 判断当前本地时刻是否在免打扰时段。
/// / Check whether local time is inside the quiet period.
bool ambient_sync_is_quiet(uint32_t seconds_since_midnight,
                           uint8_t quiet_start_h, uint8_t quiet_end_h);
