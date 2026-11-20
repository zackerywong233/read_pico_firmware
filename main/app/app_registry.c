/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pico OS 一级应用与系统工具入口。主菜单顺序在这里维护。
 *
 * Pico OS top-level apps and system tools entry. The root menu order lives here.
 *
 * 冻结：一级菜单突出固件原生应用与设备自检；硬件演示和开发诊断统一从系统工具进入，避免 Pico OS 被呈现为众多演示页之一。
 * Frozen: The root menu foregrounds native firmware apps and device self-test;
 * hardware demos and development diagnostics live under System Tools so Pico OS
 * is not presented as one item among a flat list of demos.
 */

#include "app_registry.h"

extern const app_desc_t app_home;
extern const app_desc_t app_personal;
extern const app_desc_t app_pico_home;
extern const app_desc_t app_refresh;
extern const app_desc_t app_reading;
extern const app_desc_t app_touch;
extern const app_desc_t app_axis;
extern const app_desc_t app_axis_lab;
extern const app_desc_t app_power;
extern const app_desc_t app_pmu;
extern const app_desc_t app_key;
extern const app_desc_t app_sleep;
extern const app_desc_t app_sd;
extern const app_desc_t app_font_pick;
extern const app_desc_t app_ioe;
extern const app_desc_t app_selftest;
extern const app_desc_t app_book;
extern const app_desc_t app_transfer;
extern const app_desc_t app_web;
extern const app_desc_t app_agent;
extern const app_desc_t app_system_tools;

// 一级只列日常功能、自检和系统工具入口；硬件诊断由工具页继续进入。
// Keep daily functions, self-test and the system tools entry at the root; diagnostics live one level deeper.
static const app_desc_t* const s_apps[] = {
    &app_book,
    &app_transfer,
    &app_web,
    &app_agent,
    &app_home,
    &app_personal,
    &app_selftest,
    &app_system_tools,
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
