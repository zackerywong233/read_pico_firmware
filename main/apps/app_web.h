/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：网页阅读页对外入口，供传书页或 BLE 投递待打开的 HTTP(S) 网址。
 *
 * English: Public entry for Web Reader so Transfer or BLE can queue an HTTP(S) URL to open.
 */
#pragma once

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const app_desc_t app_web;

/// 排队一个待在网页阅读页打开的 HTTP(S) 地址（跨切页保留，联网就绪后自动抓取）。
/// Queue an HTTP(S) URL to open in Web Reader (preserved across page switch and fetched once STA is ready).
void app_web_queue_url(const char *url);

#ifdef __cplusplus
}
#endif
