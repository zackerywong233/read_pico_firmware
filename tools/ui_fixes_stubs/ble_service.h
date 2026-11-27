/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：UI 回归宿主测试的 BLE 服务桩头文件。/ English: BLE service stub header for UI host tests.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

void app_ble_set_enabled(bool enabled);
void app_ble_set_pin_required(bool required);
void app_ble_format_status(char *buf, size_t cap);
void app_ble_yield_to_ota(void);
