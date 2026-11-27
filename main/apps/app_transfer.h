/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：传书页对外控制接口，供 BLE 配套控制通道远程启动或停止局域网传输。
 *
 * English: Public control hooks for the Transfer page so the BLE companion service can start or stop LAN transfer remotely.
 */
#pragma once

#include <stdbool.h>

#include "app.h"
#include "read_pico_transfer.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const app_desc_t app_transfer;

/// 进入传书页前预设网络模式（AP 或 STA）。/ Preset network mode (AP or STA) before entering the Transfer page.
void app_transfer_set_preferred_mode(read_pico_transfer_mode_t mode);
/// 当前已处于传书页时，在空闲状态切换网络模式并重启服务；上传中返回 false。
/// While already on Transfer, switch mode and restart when idle; returns false if busy.
bool app_transfer_remote_switch_mode(read_pico_transfer_mode_t mode);
/// 当前处于传书页且空闲时停止传输并请求返回上一页；上传中返回 false。
/// Stop Transfer and request return when idle; returns false if an upload is active.
bool app_transfer_remote_stop(app_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
