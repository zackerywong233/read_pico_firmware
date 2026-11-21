/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：小纸 Pico BLE 配套控制服务的 UUID 常量（大端字符串与 NimBLE 小端字节序一致）。
 *
 * English: Read Pico BLE companion service UUID constants (canonical strings and NimBLE little-endian bytes).
 *
 * 冻结：已发布的 GATT 服务与特征 UUID 不得随意更改，避免破坏 iOS 客户端兼容性。
 * Frozen: Published GATT service and characteristic UUIDs must remain stable for iOS client compatibility.
 */
#pragma once

/// 配套控制主服务 UUID 字符串。/ Companion control primary service UUID string.
#define READ_PICO_BLE_SVC_UUID_STR "72656164-7069-636f-0001-000000000000"
/// 设备与传输状态特征（READ | NOTIFY）。/ Device and transfer status characteristic (READ | NOTIFY).
#define READ_PICO_BLE_CHR_STATUS_UUID_STR "72656164-7069-636f-0001-000000000001"
/// 指令写入特征（WRITE | WRITE_NO_RSP）。/ Command write characteristic (WRITE | WRITE_NO_RSP).
#define READ_PICO_BLE_CHR_CMD_UUID_STR "72656164-7069-636f-0001-000000000002"
/// 响应与异步事件通知特征（NOTIFY）。/ Response and async event notification characteristic (NOTIFY).
#define READ_PICO_BLE_CHR_RSP_UUID_STR "72656164-7069-636f-0001-000000000003"

/// NimBLE 小端 128 位 UUID 初始化列表（对应 72656164-7069-636f-0001-0000000000XX）。
/// NimBLE little-endian 128-bit UUID initializer bytes for 72656164-7069-636f-0001-0000000000XX.
#define READ_PICO_BLE_UUID128_BYTES(last_byte) \
    (last_byte), 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, \
    0x6f, 0x63, 0x69, 0x70, 0x64, 0x61, 0x65, 0x72

/// 标准 Device Information Service (0x180A) 与特征 UUID。
/// Standard Device Information Service (0x180A) and characteristic UUIDs.
#define READ_PICO_BLE_DIS_SVC_UUID 0x180AU
#define READ_PICO_BLE_DIS_MODEL_UUID 0x2A24U
#define READ_PICO_BLE_DIS_FW_REV_UUID 0x2A26U
#define READ_PICO_BLE_DIS_SW_REV_UUID 0x2A28U
#define READ_PICO_BLE_DIS_MFR_UUID 0x2A29U

/// 标准 Battery Service (0x180F) 与电量特征 UUID (0x2A19)。
/// Standard Battery Service (0x180F) and Battery Level UUID (0x2A19).
#define READ_PICO_BLE_BAS_SVC_UUID 0x180FU
#define READ_PICO_BLE_BAS_LEVEL_UUID 0x2A19U

/// 广播厂商自定义数据公司 ID 与协议版本。/ Advertising manufacturer data company ID and protocol version.
#define READ_PICO_BLE_MFG_COMPANY_ID 0xFFFFU
#define READ_PICO_BLE_MFG_PROTO_VER 0x01U
#define READ_PICO_BLE_MFG_DATA_LEN 9U
