/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pico OS 一级应用的清单。菜单与主循环从这里取；系统工具单独收纳底层诊断页。
 *
 * Pico OS root app table. Menu and loop read from here; system tools group
 * low-level diagnostic pages separately.
 */

#pragma once

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

int app_count(void);
const app_desc_t* app_at(int index);
/// 找不到时返回 -1。/ Returns -1 when not found.
int app_index_of(const app_desc_t* app);

/// 开机默认页与设备功能自检页，供 app_main 与首页引用。/ Boot default page and device self-test page, for app_main and the home page.
const app_desc_t* app_home_page(void);
const app_desc_t* app_personal_page(void);
const app_desc_t* app_selftest_page(void);
/// 字体管理页，供阅读页快捷跳转。/ Font management page for direct navigation from the reader.
const app_desc_t* app_font_page(void);
/// 一级设置页，供桌面入口跳转。/ Root Settings page for the home launcher.
const app_desc_t* app_settings_page(void);
/// 初始使用引导页，首次开机自动进入，也可从设置页重看。
/// Onboarding guide page: entered automatically on first boot and accessible from Settings.
const app_desc_t* app_onboarding_page(void);

#ifdef __cplusplus
}
#endif
