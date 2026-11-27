/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：保存累计阅读时长、按日阅读热力图、最近摘录与最近阅读，不依赖图书或 TF 卡。
 * English: Persist total reading time, daily reading heatmap, excerpt, and recent book independently of books and SD.
 * 冻结：统计与摘录保存在专用命名空间，不复用图书续读记录。
 * Frozen: Keep stats and excerpts in their own namespace; do not reuse book-resume records.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/// 按日热力图滚动窗口天数（26 周 / 半年）。/ Rolling daily heatmap window in days (26 weeks / half-year).
#define READING_HEATMAP_DAYS 182U

typedef struct {
    bool has_date;                            ///< anchor_day 是否有效 / Whether anchor_day is valid
    uint32_t anchor_day;                      ///< 最右侧日期（1970-01-01 起本地天数）/ Anchor local day since 1970-01-01
    uint16_t daily_sec[READING_HEATMAP_DAYS]; ///< 从旧到新每日阅读秒数，末项对应 anchor_day / Daily seconds oldest..newest
    uint32_t today_sec;                       ///< anchor_day 当天阅读秒数 / Reading seconds on anchor_day
    uint32_t window_total_sec;                ///< 窗口内总阅读秒数 / Total reading seconds in window
    uint16_t max_daily_sec;                   ///< 窗口内单日最高秒数 / Peak daily reading seconds in window
    uint16_t active_days;                     ///< 窗口内有阅读记录的天数 / Days with reading > 0 in window
    uint16_t current_streak;                  ///< 当前连续阅读天数 / Current consecutive reading days
    uint16_t longest_streak;                  ///< 窗口内最长连续阅读天数 / Longest consecutive reading streak in window
} reading_heatmap_t;

uint32_t reading_stats_total_seconds(void);
void reading_stats_add_seconds(uint32_t seconds);
/// 在指定本地天数上累计阅读秒数（local_day 为 0 时仅更新累计总时长）。
/// Add reading seconds for an explicit local day index (0 updates total seconds only).
void reading_stats_add_seconds_on_day(uint32_t local_day, uint32_t seconds);
/// 覆盖写入累计阅读秒数，用于备份恢复。/ Overwrite cumulative reading seconds for backup restore.
esp_err_t reading_stats_set_total_seconds(uint32_t seconds);
/// 按本地年月日换算自 1970-01-01 起的天数；非法日期返回 0。/ Convert local Y/M/D to days since 1970-01-01; 0 if invalid.
uint32_t reading_stats_day_from_ymd(int year, int month, int day);
/// 将自 1970-01-01 起的天数还原为年月日。/ Convert days since 1970-01-01 back to Y/M/D.
void reading_stats_ymd_from_day(uint32_t day_index, int* year, int* month, int* day);
/// 将单日阅读秒数映射为 0..4 热力强度档位。/ Map daily reading seconds to heatmap intensity level 0..4.
uint8_t reading_stats_heatmap_level(uint32_t seconds);
/// 读取以 anchor_day 结尾的 182 天热力图快照；anchor_day 为 0 时回退最后记录日。
/// Read a 182-day heatmap snapshot ending at anchor_day; falls back to last recorded day when anchor_day is 0.
bool reading_stats_get_heatmap(uint32_t anchor_day, reading_heatmap_t* out);
void reading_stats_save_excerpt(const char* title, const char* text);
/// 写入或清空最近摘录（text 为空串或 NULL 表示清空）。/ Set or clear the saved excerpt (empty/NULL text clears it).
esp_err_t reading_stats_set_excerpt_checked(const char* title, const char* text);
bool reading_stats_get_excerpt(char* title, uint32_t title_cap, char* text, uint32_t text_cap);
void reading_stats_set_last_book(const char* title, uint8_t percent);
/// 写入或清空最近阅读图书（title 为空串或 NULL 表示清空）。/ Set or clear recent book progress (empty/NULL title clears it).
esp_err_t reading_stats_set_last_book_checked(const char* title, uint8_t percent);
bool reading_stats_get_last_book(char* title, uint32_t title_cap, uint8_t* percent);
