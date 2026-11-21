/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：独立 BLE 配套控制通道接口（NimBLE 外设）；只负责广播、GATT 收发与分包，不含页面或存储实现。
 *
 * English: Standalone BLE companion control interface (NimBLE peripheral); owns advertising, GATT transport and framing without page or storage dependencies.
 *
 * 冻结：不承担大文件或 OTA 传输；不暴露面板 VCOM 或电源芯片写操作。
 * Frozen: Never carry bulk file or OTA transfers; never expose panel VCOM or PMIC write operations.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "read_pico_ble_uuids.h"

#ifdef __cplusplus
extern "C" {
#endif

#define READ_PICO_BLE_API_VERSION 1U
#define READ_PICO_BLE_PAYLOAD_MAX 4096U
#define READ_PICO_BLE_NAME_MAX 24U

typedef enum {
    READ_PICO_BLE_STOPPED = 0,     ///< 已停止 / Stopped
    READ_PICO_BLE_STARTING,        ///< 启动中 / Starting
    READ_PICO_BLE_ADVERTISING,     ///< 广播等待连接 / Advertising
    READ_PICO_BLE_CONNECTED,       ///< 已连接 / Connected
    READ_PICO_BLE_ERROR,           ///< 启动或协议栈错误 / Stack error
} read_pico_ble_state_t;

typedef struct {
    read_pico_ble_state_t state;   ///< 当前 BLE 状态 / Current BLE state
    char device_name[READ_PICO_BLE_NAME_MAX]; ///< 广播设备名（ReadPico-XXXX）/ Advertised name (ReadPico-XXXX)
    bool peer_connected;           ///< 是否有中心设备连接 / Whether a central is connected
    uint16_t mtu;                  ///< 当前连接 ATT MTU / Active connection ATT MTU
    bool status_notify_enabled;    ///< 状态特征订阅开关 / Status notify subscription state
    bool rsp_notify_enabled;       ///< 响应特征订阅开关 / Response notify subscription state
    bool battery_notify_enabled;   ///< 电量特征订阅开关 / Battery notify subscription state
    uint32_t rx_count;             ///< 收到的完整命令数 / Complete commands received
    uint32_t tx_count;             ///< 发送的响应/事件数 / Responses and events sent
    esp_err_t last_error;          ///< 最近一次错误码 / Latest error code
} read_pico_ble_status_t;

typedef struct {
    uint8_t battery_soc;           ///< 电量 0..100，未知填 0xFF / Battery percent 0..100, 0xFF if unknown
    bool battery_valid;            ///< 电量有效标志 / Battery reading valid
    bool charging;                 ///< 充电中 / Charging
    bool transfer_active;          ///< 局域网传输页活动中 / LAN transfer active
    uint8_t transfer_mode;         ///< 0=空闲 1=AP 2=STA / 0=idle 1=AP 2=STA
    bool wifi_configured;          ///< 存在已保存 Wi-Fi / Saved Wi-Fi credentials exist
    bool sd_mounted;               ///< TF 卡已挂载 / TF card mounted
    bool pin_required;             ///< 传输页启用配对码 / LAN transfer PIN enforced
    uint8_t fw_major;              ///< 固件主版本号 / Firmware major version
    uint8_t fw_minor;              ///< 固件次版本号 / Firmware minor version
    uint8_t fw_patch;              ///< 固件修订号 / Firmware patch version
} read_pico_ble_adv_state_t;

typedef struct {
    /// 收到完整 UTF-8 JSON 命令时在 NimBLE 任务回调；调用方应投递至主循环处理。
    /// Invoked on the NimBLE task when a complete UTF-8 JSON command arrives; defer work to the main loop.
    void (*on_command)(const char *json, size_t len, void *ctx);
    /// 读取状态特征时在 NimBLE 任务回调；必须无阻塞且不访问 I2C。
    /// Invoked on GATT read of the status characteristic; must be non-blocking and touch no I2C.
    size_t (*on_status_read)(char *buf, size_t cap, void *ctx);
    /// 读取标准电量特征时回调；返回 0..100（未知返回 0）。不得访问 I2C。
    /// Invoked on BAS battery read; returns 0..100 (0 if unknown) without touching I2C.
    uint8_t (*on_battery_read)(void *ctx);
    /// 连接状态变化回调。/ Connection state change callback.
    void (*on_conn_change)(bool connected, void *ctx);
    void *ctx;
} read_pico_ble_callbacks_t;

/// 启动 NimBLE 外设并开始广播；同一控制任务串行调用。/ Start NimBLE peripheral and advertising; serialize on one owner task.
esp_err_t read_pico_ble_start(const read_pico_ble_callbacks_t *cbs);
/// 停止广播、断开连接并释放 NimBLE 协议栈资源；可重复调用。/ Stop advertising, disconnect and release NimBLE resources; repeatable.
void read_pico_ble_stop(void);
/// 是否处于启动或运行状态。/ Whether the BLE stack is started or running.
bool read_pico_ble_is_running(void);
/// 跨任务复制一致状态快照。/ Copy a consistent status snapshot across tasks.
void read_pico_ble_get_status(read_pico_ble_status_t *out);
/// 向已订阅的中心设备发送 JSON 响应或异步事件（超 MTU 自动分包）。/ Send a JSON response or async event to the subscribed central (auto-chunks above MTU).
esp_err_t read_pico_ble_send_response(const char *json, size_t len);
/// 向已订阅的中心设备推送状态特征通知。/ Push a notification on the status characteristic when subscribed.
esp_err_t read_pico_ble_notify_status(const char *json, size_t len);
/// 向已订阅的中心设备推送标准电量特征（0x2A19）通知。/ Push a standard Battery Level (0x2A19) notification when subscribed.
esp_err_t read_pico_ble_notify_battery(uint8_t soc_percent);
/// 更新广播扫描响应中的厂商状态数据。/ Update manufacturer beacon data in the advertising scan response.
void read_pico_ble_update_adv_state(const read_pico_ble_adv_state_t *state);

#ifdef __cplusplus
}
#endif
