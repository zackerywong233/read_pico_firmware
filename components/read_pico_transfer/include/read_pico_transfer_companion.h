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
#include <stddef.h>
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
    bool book_top_bar;              ///< 阅读顶部状态栏开关 / Reader top status bar switch
    bool book_bottom_bar;           ///< 阅读底部信息栏开关 / Reader bottom info bar switch
    uint8_t book_para_gap;          ///< 阅读段间距档位 0..2 / Paragraph gap preset 0..2
    uint8_t book_indent;            ///< 阅读段首缩进 0..1 / First-line indent mode 0..1
    uint8_t book_shelf_view;        ///< 书架视图 0=封面网格 1=列表 / Shelf view 0=grid 1=list
    uint8_t book_cbz_dir;           ///< CBZ 翻页方向 0=右翻 1=左翻 / CBZ direction 0=RTL 1=manga
    bool book_cbz_dither;           ///< CBZ 16 灰阶有序抖动 / CBZ ordered dithering
    uint8_t saver_style;            ///< 锁屏样式 0..4 / Lock-screen style 0..4
    uint8_t dashboard_items;        ///< 看板开关位掩码 0..31 / Dashboard item bitmask 0..31
    int16_t timezone_min;           ///< 全局时区偏移分钟 -720..840 / Timezone offset minutes -720..840
    bool charge_led;                ///< 充电指示灯开关 / Charge indicator LED switch
    uint8_t low_soc_notify_percent; ///< 低电提醒 0 或 5..30 步长 5 / Low-battery alert 0 or 5..30 step 5
    bool muted;                     ///< 全局蜂鸣器静音 / Global buzzer mute
    bool transfer_pin_required;     ///< 局域网传输强制 6 位配对码 / Require 6-digit PIN for LAN transfer writes
    bool ble_enabled;               ///< 轻量 BLE 控制通道开关 / Lightweight BLE control channel switch
    bool ble_pin_required;          ///< BLE 敏感指令强制 6 位配对码 / Require 6-digit PIN for sensitive BLE commands
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
    char excerpt_text[304];     ///< 最近摘录正文（≤300 字节 UTF-8）/ Recent excerpt text (up to 300 UTF-8 bytes)
    bool has_last_book_path;    ///< 是否有自动续读路径 / Whether auto-resume last book path exists
    char last_book_path[288];   ///< 最近打开图书完整路径 / Full path of the last opened book
} read_pico_dashboard_stats_t;

typedef struct {
    bool merge;                 ///< false=覆盖恢复，true=合并（时长取大、空项补全）/ false=replace, true=merge
    bool has_total_reading_sec; ///< 是否恢复累计阅读秒数 / Whether to update total_reading_sec
    uint32_t total_reading_sec; ///< 目标累计阅读秒数 / Target total reading seconds
    bool has_last_book;         ///< 是否恢复或清除最近阅读 / Whether to update or clear last_book
    bool clear_last_book;       ///< true 表示清除最近阅读 / True to clear last_book
    char last_book_title[128];  ///< 最近阅读书名 / Recent book title
    uint8_t last_book_percent;  ///< 最近阅读进度 0..100 / Recent book progress 0..100
    bool has_excerpt;           ///< 是否恢复或清除最近摘录 / Whether to update or clear excerpt
    bool clear_excerpt;         ///< true 表示清除最近摘录 / True to clear excerpt
    char excerpt_title[128];    ///< 最近摘录书名 / Excerpt book title
    char excerpt_text[304];     ///< 最近摘录正文 / Excerpt text
} read_pico_stats_restore_t;

typedef struct {
    bool has_progress;  ///< 是否存在匹配路径与大小的阅读进度 / Whether valid progress exists for path+size
    uint8_t percent;    ///< 全书阅读百分比 0..100 / Whole-book progress percent 0..100
    uint16_t chapter;   ///< 当前章节索引 / Active chapter index
    uint32_t byte_off;  ///< 章节内 UTF-8 字节偏移 / UTF-8 byte offset inside chapter
    uint8_t px;         ///< 逐书阅读字号 36..72（步长 4），0 表示未指定 / Book font size 36..72 step 4; 0 means unspecified
    uint32_t order;     ///< 最近阅读单调序号 / Monotonic recency sequence
} read_pico_book_progress_t;

typedef struct {
    uint8_t percent;    ///< 全书阅读百分比 0..100 / Whole-book progress percent 0..100
    uint16_t chapter;   ///< 当前章节索引 / Active chapter index
    uint32_t byte_off;  ///< 章节内 UTF-8 字节偏移 / UTF-8 byte offset inside chapter
    bool has_px;        ///< 是否显式指定字号 / Whether px was explicitly supplied
    uint8_t px;         ///< 字号 36..72 步长 4 / Font size 36..72 step 4
    bool has_file_size; ///< 是否携带文件大小校验 / Whether expected file_size was supplied
    uint32_t file_size; ///< 期望图书字节大小 / Expected book size in bytes
    bool set_last;      ///< 是否设为上次阅读入口 / Also set as last-opened resume book
} read_pico_book_progress_restore_t;

typedef struct {
    bool is_excerpt;   ///< false=书签，true=摘录 / false=bookmark, true=excerpt
    uint16_t chapter;  ///< 章节索引 / Chapter index
    uint32_t byte_off; ///< 章节内 UTF-8 字节偏移 / UTF-8 byte offset inside chapter
    uint8_t percent;   ///< 全书百分比 0..100 / Whole-book progress percent 0..100
    const char *text;  ///< 摘录 UTF-8 正文（书签为 NULL）/ Excerpt UTF-8 text (NULL for bookmark)
    size_t text_len;   ///< 摘录字节长度 / Excerpt byte length
} read_pico_book_note_item_t;

#define READ_PICO_WEBMARKS_MAX 20U

typedef struct {
    char title[64];  ///< 网页收藏标题 / Bookmark title
    char url[256];   ///< 网页收藏 HTTP(S) 地址 / Bookmark HTTP(S) URL
} read_pico_webmark_t;

#define READ_PICO_FEEDS_MAX 16U

typedef struct {
    char name[64];       ///< 订阅源名称 / Feed source name
    char url[256];       ///< 订阅源 HTTP(S) 地址 / Feed source HTTP(S) URL
    uint8_t kind;        ///< 0 = RSS/Atom, 1 = OPDS / 0 = RSS/Atom, 1 = OPDS
    char auth_user[64];  ///< 可选用户名 / Optional username
    bool has_password;   ///< 是否已保存密码（绝不回显明文）/ Whether a password is stored (never plaintext)
} read_pico_feed_source_t;

typedef struct {
    bool running;             ///< 番茄钟是否运行中 / Whether pomodoro is running
    bool is_break;            ///< 是否处于休息阶段 / Whether in break phase
    uint16_t duration_min;    ///< 专注时长（15/25/45）/ Focus duration in minutes
    uint32_t remaining_sec;   ///< 剩余秒数 / Remaining seconds
    uint32_t completed_count; ///< 累计完成次数 / Total completed sessions
} read_pico_focus_status_t;
