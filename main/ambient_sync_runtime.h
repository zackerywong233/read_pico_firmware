#pragma once

/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：声明需要显示与网络栈的定时刷新执行入口。
 * English: Declares the scheduled refresh entry point that uses display and network services.
 * 冻结：调用方传入活动 framebuffer；失败时不得清空或推送屏幕。
 * Frozen: Use the caller's active framebuffer; never clear or present after failure.
 */

#include <stdbool.h>
#include <stdint.h>

#include "epd_highlevel.h"

/// 自动唤醒时拉取、绘制并刷新一次看板；失败时保留原屏幕。
/// / Fetch, draw and present one scheduled dashboard on alarm wake; preserve the old screen on failure.
bool ambient_sync_run(EpdiyHighlevelState* hl, uint8_t* framebuffer);
