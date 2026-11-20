/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：保存累计阅读时长，不依赖图书或 TF 卡。
 * English: Persist total reading time independently of books and the SD card.
 * 冻结：统计与摘录保存在专用命名空间，不复用图书续读记录。
 * Frozen: Keep stats and excerpts in their own namespace; do not reuse book-resume records.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

uint32_t reading_stats_total_seconds(void);
void reading_stats_add_seconds(uint32_t seconds);
void reading_stats_save_excerpt(const char* title, const char* text);
bool reading_stats_get_excerpt(char* title, uint32_t title_cap, char* text, uint32_t text_cap);
void reading_stats_set_last_book(const char* title, uint8_t percent);
bool reading_stats_get_last_book(char* title, uint32_t title_cap, uint8_t* percent);
