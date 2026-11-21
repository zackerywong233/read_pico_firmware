/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：BLE 命令与响应分包重组、广播厂商数据编码与语义版本解析，不含平台依赖。
 *
 * English: Platform-independent BLE framing, reassembly, advertising beacon encoding and semver parsing.
 *
 * 冻结：单包裸 JSON（首字节 '{'）与 6 字节 0x01 分包头并存，总载荷上限 4096 字节。
 * Frozen: Support both raw single-packet JSON (leading '{') and 6-byte 0x01 chunked envelopes up to 4096 bytes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "read_pico_ble_uuids.h"

#define BLE_FRAME_MAGIC 0x01U
#define BLE_FRAME_HDR_LEN 6U
#define BLE_FRAME_FLAG_START 0x01U
#define BLE_FRAME_FLAG_END 0x02U
#define BLE_FRAME_MAX_PAYLOAD 4096U
#define BLE_FRAME_MIN_ATT_MTU 23U
#define BLE_FRAME_TIMEOUT_MS 5000LL

/// 广播厂商数据标志位。/ Advertising manufacturer data flag bits.
#define BLE_ADV_FLAG_CHARGING (1U << 0)
#define BLE_ADV_FLAG_TRANSFER_ACTIVE (1U << 1)
#define BLE_ADV_FLAG_WIFI_CONFIGURED (1U << 2)
#define BLE_ADV_FLAG_SD_MOUNTED (1U << 3)
#define BLE_ADV_FLAG_PIN_REQUIRED (1U << 4)
#define BLE_ADV_FLAG_BATTERY_VALID (1U << 5)

typedef enum {
    BLE_RX_NEED_MORE = 0, ///< 等待后续分片 / Waiting for more chunks
    BLE_RX_COMPLETE,      ///< 完整消息已就绪 / Complete message ready
    BLE_RX_ERROR,         ///< 帧格式、顺序或长度无效 / Invalid frame, sequence or length
} ble_rx_result_t;

typedef struct {
    char *buf;            ///< 调用方提供的接收缓冲，大小至少 BLE_FRAME_MAX_PAYLOAD + 1 / Caller buffer of >= BLE_FRAME_MAX_PAYLOAD + 1 bytes
    size_t cap;           ///< 缓冲容量（不含末尾 NUL）/ Buffer capacity excluding NUL
    size_t expected;      ///< 当前消息期望总字节数 / Expected total bytes
    size_t received;      ///< 已接收字节数 / Received bytes so far
    uint8_t msg_id;       ///< 当前消息 ID / Active message ID
    uint8_t next_seq;     ///< 下一期望分片序号 / Next expected chunk sequence
    bool active;          ///< 是否正在重组分包消息 / Reassembly in progress
    int64_t last_ms;      ///< 上一分片到达时间戳（ms）/ Timestamp of last chunk (ms)
} ble_rx_reassembler_t;

typedef struct {
    uint8_t battery_soc;
    bool battery_valid;
    bool charging;
    bool transfer_active;
    uint8_t transfer_mode;
    bool wifi_configured;
    bool sd_mounted;
    bool pin_required;
    uint8_t fw_major;
    uint8_t fw_minor;
    uint8_t fw_patch;
} ble_adv_beacon_t;

static inline void ble_rx_init(ble_rx_reassembler_t *rx, char *buf, size_t cap) {
    if (!rx) return;
    memset(rx, 0, sizeof(*rx));
    rx->buf = buf;
    rx->cap = cap > BLE_FRAME_MAX_PAYLOAD ? BLE_FRAME_MAX_PAYLOAD : cap;
}

static inline void ble_rx_reset(ble_rx_reassembler_t *rx) {
    if (!rx) return;
    rx->expected = 0;
    rx->received = 0;
    rx->msg_id = 0;
    rx->next_seq = 0;
    rx->active = false;
    rx->last_ms = 0;
}

static inline ble_rx_result_t ble_rx_feed(ble_rx_reassembler_t *rx,
                                          const uint8_t *data, size_t len,
                                          int64_t now_ms, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!rx || !rx->buf || !rx->cap || !data || !len) return BLE_RX_ERROR;

    if (rx->active && rx->last_ms > 0 && now_ms - rx->last_ms > BLE_FRAME_TIMEOUT_MS) {
        ble_rx_reset(rx);
    }

    // 单包裸 JSON（以 '{' 开头），便于调试工具和短命令直发。
    // Single-packet raw JSON (leading '{') for simple short commands and BLE debuggers.
    if (data[0] == '{') {
        ble_rx_reset(rx);
        if (len > rx->cap) return BLE_RX_ERROR;
        memcpy(rx->buf, data, len);
        rx->buf[len] = '\0';
        if (out_len) *out_len = len;
        return BLE_RX_COMPLETE;
    }

    if (data[0] != BLE_FRAME_MAGIC || len < BLE_FRAME_HDR_LEN) {
        ble_rx_reset(rx);
        return BLE_RX_ERROR;
    }

    uint8_t flags = data[1];
    uint8_t seq = data[2];
    uint8_t msg_id = data[3];
    uint16_t total_len = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
    const uint8_t *slice = data + BLE_FRAME_HDR_LEN;
    size_t slice_len = len - BLE_FRAME_HDR_LEN;

    if ((flags & ~(BLE_FRAME_FLAG_START | BLE_FRAME_FLAG_END)) != 0U) {
        ble_rx_reset(rx);
        return BLE_RX_ERROR;
    }
    if (total_len == 0U || total_len > rx->cap) {
        ble_rx_reset(rx);
        return BLE_RX_ERROR;
    }

    if (flags & BLE_FRAME_FLAG_START) {
        ble_rx_reset(rx);
        if (seq != 0U || slice_len > total_len) return BLE_RX_ERROR;
        rx->active = true;
        rx->expected = total_len;
        rx->msg_id = msg_id;
        rx->next_seq = 1U;
        rx->received = 0;
    } else {
        if (!rx->active || msg_id != rx->msg_id || seq != rx->next_seq ||
            total_len != rx->expected || rx->received + slice_len > rx->expected) {
            ble_rx_reset(rx);
            return BLE_RX_ERROR;
        }
        rx->next_seq = (uint8_t)(rx->next_seq + 1U);
    }

    if (slice_len > 0U) {
        memcpy(rx->buf + rx->received, slice, slice_len);
        rx->received += slice_len;
    }
    rx->last_ms = now_ms;

    if (flags & BLE_FRAME_FLAG_END) {
        if (rx->received != rx->expected) {
            ble_rx_reset(rx);
            return BLE_RX_ERROR;
        }
        size_t total = rx->received;
        rx->buf[total] = '\0';
        ble_rx_reset(rx);
        if (out_len) *out_len = total;
        return BLE_RX_COMPLETE;
    }

    if (rx->received >= rx->expected) {
        ble_rx_reset(rx);
        return BLE_RX_ERROR;
    }
    return BLE_RX_NEED_MORE;
}

static inline size_t ble_tx_effective_value_len(uint16_t att_mtu) {
    if (att_mtu < BLE_FRAME_MIN_ATT_MTU) att_mtu = BLE_FRAME_MIN_ATT_MTU;
    return (size_t)(att_mtu - 3U);
}

static inline size_t ble_tx_chunk_count(size_t payload_len, uint16_t att_mtu) {
    if (payload_len == 0U || payload_len > BLE_FRAME_MAX_PAYLOAD) return 0U;
    size_t max_val = ble_tx_effective_value_len(att_mtu);
    if (payload_len <= max_val) return 1U;
    if (max_val <= BLE_FRAME_HDR_LEN) return 0U;
    size_t per_chunk = max_val - BLE_FRAME_HDR_LEN;
    return (payload_len + per_chunk - 1U) / per_chunk;
}

/// 编码第 chunk_idx 个发送分片；若整包可直接装入单个 ATT 通知则原样输出裸 JSON。
/// Encode chunk chunk_idx; emits raw JSON directly when the payload fits in a single ATT notification.
static inline size_t ble_tx_encode_chunk(const char *payload, size_t payload_len,
                                         uint16_t att_mtu, uint8_t msg_id,
                                         size_t chunk_idx, uint8_t *out, size_t out_cap) {
    if (!payload || !payload_len || payload_len > BLE_FRAME_MAX_PAYLOAD || !out) return 0U;
    size_t max_val = ble_tx_effective_value_len(att_mtu);
    if (payload_len <= max_val) {
        if (chunk_idx != 0U || out_cap < payload_len) return 0U;
        memcpy(out, payload, payload_len);
        return payload_len;
    }
    if (max_val <= BLE_FRAME_HDR_LEN) return 0U;
    size_t per_chunk = max_val - BLE_FRAME_HDR_LEN;
    size_t total_chunks = ble_tx_chunk_count(payload_len, att_mtu);
    if (total_chunks == 0U || chunk_idx >= total_chunks) return 0U;

    size_t offset = chunk_idx * per_chunk;
    size_t slice_len = payload_len - offset;
    if (slice_len > per_chunk) slice_len = per_chunk;
    if (out_cap < BLE_FRAME_HDR_LEN + slice_len) return 0U;

    uint8_t flags = 0U;
    if (chunk_idx == 0U) flags |= BLE_FRAME_FLAG_START;
    if (chunk_idx + 1U == total_chunks) flags |= BLE_FRAME_FLAG_END;

    out[0] = BLE_FRAME_MAGIC;
    out[1] = flags;
    out[2] = (uint8_t)(chunk_idx & 0xFFU);
    out[3] = msg_id;
    out[4] = (uint8_t)(payload_len & 0xFFU);
    out[5] = (uint8_t)((payload_len >> 8) & 0xFFU);
    memcpy(out + BLE_FRAME_HDR_LEN, payload + offset, slice_len);
    return BLE_FRAME_HDR_LEN + slice_len;
}

static inline void ble_adv_mfg_encode(const ble_adv_beacon_t *state,
                                      uint8_t out[READ_PICO_BLE_MFG_DATA_LEN]) {
    if (!out) return;
    memset(out, 0, READ_PICO_BLE_MFG_DATA_LEN);
    out[0] = (uint8_t)(READ_PICO_BLE_MFG_COMPANY_ID & 0xFFU);
    out[1] = (uint8_t)((READ_PICO_BLE_MFG_COMPANY_ID >> 8) & 0xFFU);
    out[2] = READ_PICO_BLE_MFG_PROTO_VER;
    if (!state) {
        out[4] = 0xFFU;
        return;
    }
    uint8_t flags = 0U;
    if (state->charging) flags |= BLE_ADV_FLAG_CHARGING;
    if (state->transfer_active) flags |= BLE_ADV_FLAG_TRANSFER_ACTIVE;
    if (state->wifi_configured) flags |= BLE_ADV_FLAG_WIFI_CONFIGURED;
    if (state->sd_mounted) flags |= BLE_ADV_FLAG_SD_MOUNTED;
    if (state->pin_required) flags |= BLE_ADV_FLAG_PIN_REQUIRED;
    if (state->battery_valid) flags |= BLE_ADV_FLAG_BATTERY_VALID;
    out[3] = flags;
    out[4] = state->battery_valid ? (state->battery_soc > 100U ? 100U : state->battery_soc) : 0xFFU;
    out[5] = state->transfer_mode <= 2U ? state->transfer_mode : 0U;
    out[6] = state->fw_major;
    out[7] = state->fw_minor;
    out[8] = state->fw_patch;
}

static inline void ble_parse_semver(const char *version,
                                    uint8_t *major, uint8_t *minor, uint8_t *patch) {
    if (major) *major = 0U;
    if (minor) *minor = 0U;
    if (patch) *patch = 0U;
    if (!version || !version[0]) return;
    const char *p = version;
    if (*p == 'v' || *p == 'V') ++p;
    unsigned parts[3] = {0, 0, 0};
    int idx = 0;
    while (*p && idx < 3) {
        if (*p < '0' || *p > '9') return;
        unsigned val = 0;
        while (*p >= '0' && *p <= '9') {
            if (val < 256U) val = val * 10U + (unsigned)(*p - '0');
            ++p;
        }
        parts[idx++] = val > 255U ? 255U : val;
        if (*p == '.') {
            ++p;
        } else {
            break;
        }
    }
    if (idx >= 1 && major) *major = (uint8_t)parts[0];
    if (idx >= 2 && minor) *minor = (uint8_t)parts[1];
    if (idx >= 3 && patch) *patch = (uint8_t)parts[2];
}
