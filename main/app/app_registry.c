/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pico OS 全部 24 个应用与工具注册表。九宫格桌面按此表分 3 页平铺（9 + 9 + 6）。
 *
 * Pico OS registry of all 24 apps and tools. The 3x3 grid desktop lays out
 * this table across 3 pages (9 + 9 + 6).
 *
 * 冻结：取消全屏列表菜单（ui_menu）与二级系统工具页（app_system_tools）；
 * 全部应用按使用频率平铺为 3 页九宫格，顺序以本表为准。
 * Frozen: Full-screen menu list and System Tools sub-index are removed; all 24
 * apps sit flat across 3 pages of the 3x3 grid in this table's order.
 */

#include "app_registry.h"

extern const app_desc_t app_pico_home;
extern const app_desc_t app_book;
extern const app_desc_t app_transfer;
extern const app_desc_t app_web;
extern const app_desc_t app_feeds;
extern const app_desc_t app_sketch;
extern const app_desc_t app_focus;
extern const app_desc_t app_personal;
extern const app_desc_t app_agent;
extern const app_desc_t app_settings;
extern const app_desc_t app_files;
extern const app_desc_t app_font_pick;
extern const app_desc_t app_home;
extern const app_desc_t app_selftest;
extern const app_desc_t app_sleep;
extern const app_desc_t app_sd;
extern const app_desc_t app_power;
extern const app_desc_t app_refresh;
extern const app_desc_t app_touch;
extern const app_desc_t app_reading;
extern const app_desc_t app_axis;
extern const app_desc_t app_axis_lab;
extern const app_desc_t app_pmu;
extern const app_desc_t app_key;
extern const app_desc_t app_ioe;

// 24 格九宫格桌面顺序（3 页：9 + 9 + 6）。/ 24-tile grid order (3 pages: 9 + 9 + 6).
static const app_desc_t* const s_apps[] = {
    // 第 1 页（高频日常）/ Page 1 (Daily apps)
    &app_book,
    &app_transfer,
    &app_web,
    &app_feeds,
    &app_sketch,
    &app_focus,
    &app_personal,
    &app_agent,
    &app_settings,
    // 第 2 页（管理与设备）/ Page 2 (Management & device)
    &app_files,
    &app_font_pick,
    &app_home,
    &app_selftest,
    &app_sleep,
    &app_sd,
    &app_power,
    &app_refresh,
    &app_touch,
    // 第 3 页（硬件诊断）/ Page 3 (Hardware diagnostics)
    &app_reading,
    &app_axis,
    &app_axis_lab,
    &app_pmu,
    &app_key,
    &app_ioe,
};

#define APP_COUNT ((int)(sizeof(s_apps) / sizeof(s_apps[0])))

int app_count(void) {
    return APP_COUNT;
}

const app_desc_t* app_at(int index) {
    if (index < 0 || index >= APP_COUNT) return NULL;
    return s_apps[index];
}

int app_index_of(const app_desc_t* app) {
    for (int i = 0; i < APP_COUNT; i++) {
        if (s_apps[i] == app) return i;
    }
    return -1;
}

const app_desc_t* app_home_page(void) {
    return &app_pico_home;
}

const app_desc_t* app_personal_page(void) {
    return &app_personal;
}

const app_desc_t* app_selftest_page(void) {
    return &app_selftest;
}

const app_desc_t* app_font_page(void) {
    return &app_font_pick;
}

const app_desc_t* app_settings_page(void) {
    return &app_settings;
}
