/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 多 Wi-Fi 凭据的版本化 NVS 存储接口。
 * Versioned NVS storage API for multiple Wi-Fi credentials.
 * 冻结：密码仅驻留于私有凭据结构，调用方必须在释放前清零。
 * Frozen: Passwords stay in private credential structures and callers must erase them before release.
 */
#pragma once
#ifndef READ_PICO_TRANSFER_NVS_TEST
#include "esp_err.h"
#endif
#include "transfer_policy.h"

esp_err_t load_credentials(transfer_credentials_t *out);
esp_err_t store_credentials(const transfer_credentials_t *next);
