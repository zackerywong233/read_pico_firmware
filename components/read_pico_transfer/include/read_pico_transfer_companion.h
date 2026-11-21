/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：配套客户端（如 iOS App）所需的设备设置快照与存储信息数据结构。
 *
 * English: Device settings snapshot and storage info structures for companion clients (such as the iOS app).
 *
 * 冻结：不得包含面板 VCOM 或电源芯片写配置字段。
 * Frozen: Must never expose panel VCOM or PMIC power-rail write fields.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define READ_PICO_TRANSFER_API_VERSION 2U

typedef struct {
    uint8_t sleep_mode;             ///< 睡眠模式 0..2 / Sleep mode 0..2
    uint16_t auto_sleep_sec;        ///< 自动锁屏秒数 / Auto-lock seconds
    bool pickup_wake;               ///< 浅睡拿起唤醒 / Light-sleep pickup wake
    uint8_t book_px;                ///< 阅读默认字号 36..72 步长 4 / Default reading size 36..72 step 4
    uint8_t book_refresh;           ///< 图书翻页刷新档位 0..2 / Book turn refresh profile 0..2
    uint8_t book_line_height;       ///< 行距档位 0..2 / Line-height preset 0..2
    uint8_t book_margin;            ///< 左右边距档位 0..2 / Side-margin preset 0..2
    uint16_t book_weight;           ///< 字重 0 或 300..800 / Font weight 0 or 300..800
    uint8_t book_rotation;          ///< 阅读方向 0..2 / Reader orientation 0..2
    uint8_t book_gesture;           ///< 翻页手势 0..2 / Page-turn gesture 0..2
    uint8_t saver_style;            ///< 锁屏样式 0..2 / Lock-screen style 0..2
    uint8_t dashboard_items;        ///< 看板开关位掩码 0..15 / Dashboard item bitmask 0..15
    int16_t timezone_min;           ///< 全局时区偏移分钟 -720..840 / Timezone offset minutes -720..840
    bool charge_led;                ///< 充电指示灯开关 / Charge indicator LED switch
    uint8_t low_soc_notify_percent; ///< 低电提醒 0 或 5..30 步长 5 / Low-battery alert 0 or 5..30 step 5
    bool muted;                     ///< 全局蜂鸣器静音 / Global buzzer mute
    bool transfer_pin_required;     ///< 局域网传输强制 6 位配对码 / Require 6-digit PIN for LAN transfer writes
    char font_path[160];            ///< 只读当前字体路径，空为内置字体 / Read-only font path; empty means built-in
} read_pico_device_settings_t;

typedef struct {
    bool flash_available;      ///< 内置存储可用 / Internal flash storage available
    uint64_t flash_free_bytes; ///< 内置存储剩余字节 / Internal flash free bytes
    bool sd_available;         ///< TF 卡存储可用 / TF card storage available
    uint64_t sd_free_bytes;    ///< TF 卡剩余字节 / TF card free bytes
} read_pico_storage_info_t;

typedef struct {
    bool time_valid;      ///< 时钟是否已校准 / Whether device clock is set
    uint32_t unix_sec;    ///< 当前 UTC Unix 秒 / Current UTC Unix seconds
    int16_t timezone_min; ///< 当前时区偏移分钟 -720..840 / Current timezone offset minutes
} read_pico_time_info_t;

typedef struct {
    uint32_t total_reading_sec; ///< 累计阅读秒数 / Cumulative reading seconds
    bool has_last_book;         ///< 是否有最近在读记录 / Whether recent book exists
    char last_book_title[128];  ///< 最近阅读书名 / Recent book title
    uint8_t last_book_percent;  ///< 最近阅读进度 0..100 / Recent book progress 0..100
    bool has_excerpt;           ///< 是否有最近摘录 / Whether recent excerpt exists
    char excerpt_title[128];    ///< 最近摘录书名 / Recent excerpt book title
    char excerpt_text[256];     ///< 最近摘录正文 / Recent excerpt text
} read_pico_dashboard_stats_t;
