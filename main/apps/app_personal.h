/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：个人看板对外接口，供 BLE/伴侣服务更新看板文字后通知页面刷新缓存。
 *
 * English: Public hook for Personal Dashboard so companion updates can refresh the cached note immediately.
 */
#pragma once

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const app_desc_t app_personal;

/// 重新从 TF 卡加载 /sdcard/dashboard.txt 缓存。/ Reload cached /sdcard/dashboard.txt from the TF card.
void app_personal_reload_note(void);

#ifdef __cplusplus
}
#endif
