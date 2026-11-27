/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：设置应用公开辅助入口（供快捷控制中心直接拉起时钟校准）。
 * English: Public Settings helpers (allows Quick Control Menu to open Time Calibration directly).
 */
#pragma once

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const app_desc_t app_settings;

/// 请求下一次进入设置页（或当前已在设置页刷新前）直接打开手动日期与时间校准视图。
/// Request Settings to open directly into the manual date/time calibration subview.
void app_settings_open_time_edit_next(void);

#ifdef __cplusplus
}
#endif
