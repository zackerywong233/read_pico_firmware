/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：专注时钟与番茄钟对外接口，供伴侣 HTTP / BLE 读取状态并远程启停前台专注。
 *
 * English: Public hooks for Focus Clock / Pomodoro so companion HTTP and BLE can read state and start/stop foreground sessions.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const app_desc_t app_focus;

typedef struct {
    bool running;            ///< 是否正在倒计时 / Whether timer is running
    bool is_break;           ///< 是否处于 5 分钟休息阶段 / Whether in 5-min break phase
    uint16_t duration_min;   ///< 专注时长（15 / 25 / 45）/ Configured focus minutes (15 / 25 / 45)
    uint32_t remaining_sec;  ///< 当前剩余秒数 / Remaining seconds when running
    uint32_t completed_count;///< 累计完成次数 / Total completed focus sessions
} app_focus_status_t;

/// 读取专注时钟状态快照（可从任意页调用）。/ Read focus timer status snapshot (safe from any page).
void app_focus_get_status(app_focus_status_t *out);
/// 远程启动指定时长（15/25/45 分钟）的专注倒计时；调用方须同时切至 app_focus 以在前台运行。
/// Start a 15/25/45-minute focus session; caller should switch to app_focus so it runs in the foreground.
bool app_focus_remote_start(int minutes);
/// 远程停止当前专注倒计时。/ Stop the active focus timer remotely.
void app_focus_remote_stop(void);

#ifdef __cplusplus
}
#endif
