/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：Pico OS 蓝牙配套控制服务桥接层，在主循环串行执行 BLE 指令并协调睡眠与传输页切换。
 *
 * English: Pico OS BLE companion service bridge; executes BLE commands on the main loop and coordinates sleep and Transfer handoff.
 *
 * 冻结：所有 PMU、I2C、存储与切页操作仅在主循环执行；不暴露 VCOM 或电源芯片写操作。
 * Frozen: Run all PMU, I2C, storage and page transitions on the main loop only; never expose VCOM or PMIC writes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 开机初始化；当 NVS 设置开启蓝牙时启动 NimBLE 广播。/ Boot init; starts NimBLE advertising when enabled in NVS.
void app_ble_init(void);
/// 切换蓝牙控制通道开关并持久化至 NVS。/ Toggle the BLE companion channel and persist to NVS.
void app_ble_set_enabled(bool enabled);
/// 切换蓝牙 6 位配对码保护并持久化至 NVS；开启时确保已生成随机配对码并重置当前连接鉴权。
/// Toggle 6-digit BLE PIN protection and persist to NVS; ensures a fresh PIN and resets current link auth.
void app_ble_set_pin_required(bool required);
/// 读取仅供设备端设置页展示的 6 位蓝牙配对码（不得通过公开网络接口回显）。
/// Read the on-device 6-digit BLE PIN for Settings display only (never echo over public interfaces).
void app_ble_get_pin(char out[7]);
/// 睡眠或关机前暂停并停止 NimBLE 协议栈，不改写 NVS 开关。/ Stop NimBLE before sleep or power-off without changing the NVS setting.
void app_ble_suspend(void);
/// 浅睡唤醒后按 NVS 设置恢复 NimBLE 广播。/ Resume NimBLE advertising after light-sleep wake if enabled in NVS.
void app_ble_resume(void);
/// 联网页面独占射频：true 时停止 NimBLE 并保持关闭，false 时按 NVS 设置恢复；不改写 NVS 开关。
/// Wi-Fi pages own the radio: true stops NimBLE and keeps it off, false restores it per NVS; never rewrites the NVS switch.
void app_ble_wifi_hold(bool hold);
/// 启动后台 OTA 任务前在主循环同步停止 NimBLE，避免刷屏期间与 OTA 联网竞争；OTA 结束后由轮询自动恢复。
/// Stop NimBLE synchronously on the main loop before spawning OTA so display redraws cannot race OTA Wi-Fi; poll resumes it when OTA ends.
void app_ble_yield_to_ota(void);
/// 主循环后台轮询：消费挂起的 BLE 指令并推送状态/电量通知；有远程活动时返回 true。
/// Main-loop background poll: process pending BLE commands and push status/battery notifications; returns true on remote activity.
bool app_ble_poll(app_ctx_t *ctx, const app_desc_t *current, bool menu_open);
/// 格式化供设置页显示的蓝牙状态摘要（纯只读快照）。/ Format a read-only BLE status summary for the Settings UI.
void app_ble_format_status(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
