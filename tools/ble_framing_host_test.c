/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：BLE 分包重组、UUID 字节序、广播信标编码与版本解析宿主测试。
 *
 * English: Host tests for BLE framing/reassembly, UUID byte order, advertising beacon encoding and semver parsing.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "read_pico_ble_framing.h"

static void test_uuid_byte_order(void) {
    // 72656164-7069-636f-0001-000000000003 在 NimBLE 小端字节序下应逐字节反转。
    // 72656164-7069-636f-0001-000000000003 reversed into NimBLE little-endian bytes.
    const uint8_t le[16] = { READ_PICO_BLE_UUID128_BYTES(0x03) };
    char formatted[37];
    snprintf(formatted, sizeof(formatted),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             le[15], le[14], le[13], le[12],
             le[11], le[10],
             le[9], le[8],
             le[7], le[6],
             le[5], le[4], le[3], le[2], le[1], le[0]);
    assert(strcmp(formatted, READ_PICO_BLE_CHR_RSP_UUID_STR) == 0);
}

static void test_raw_json_single_packet(void) {
    char buf[BLE_FRAME_MAX_PAYLOAD + 1];
    ble_rx_reassembler_t rx;
    ble_rx_init(&rx, buf, BLE_FRAME_MAX_PAYLOAD);

    const char *msg = "{\"id\":1,\"cmd\":\"ping\"}";
    size_t out_len = 0;
    assert(ble_rx_feed(&rx, (const uint8_t *)msg, strlen(msg), 1000, &out_len) == BLE_RX_COMPLETE);
    assert(out_len == strlen(msg));
    assert(strcmp(buf, msg) == 0);

    // 编码短包应直接输出裸 JSON。/ Short payloads encode directly as raw JSON.
    uint8_t pkt[256];
    assert(ble_tx_chunk_count(strlen(msg), 185) == 1);
    size_t n = ble_tx_encode_chunk(msg, strlen(msg), 185, 7, 0, pkt, sizeof(pkt));
    assert(n == strlen(msg));
    assert(memcmp(pkt, msg, n) == 0);
}

static void test_chunked_roundtrip_various_mtus(void) {
    char payload[BLE_FRAME_MAX_PAYLOAD + 1];
    payload[0] = '{';
    for (size_t i = 1; i < BLE_FRAME_MAX_PAYLOAD - 1; ++i) {
        payload[i] = (char)('a' + (i % 26));
    }
    payload[BLE_FRAME_MAX_PAYLOAD - 1] = '}';
    payload[BLE_FRAME_MAX_PAYLOAD] = '\0';

    const uint16_t mtus[] = { 23, 64, 185, 256, 512 };
    const size_t lens[] = { 25, 200, 1024, 3584, BLE_FRAME_MAX_PAYLOAD };

    char rx_buf[BLE_FRAME_MAX_PAYLOAD + 1];
    ble_rx_reassembler_t rx;
    ble_rx_init(&rx, rx_buf, BLE_FRAME_MAX_PAYLOAD);

    uint8_t msg_id = 1;
    for (size_t mi = 0; mi < sizeof(mtus) / sizeof(mtus[0]); ++mi) {
        for (size_t li = 0; li < sizeof(lens) / sizeof(lens[0]); ++li) {
            uint16_t mtu = mtus[mi];
            size_t plen = lens[li];
            size_t chunks = ble_tx_chunk_count(plen, mtu);
            assert(chunks >= 1);

            uint8_t pkt[512];
            size_t out_len = 0;
            for (size_t c = 0; c < chunks; ++c) {
                size_t n = ble_tx_encode_chunk(payload, plen, mtu, msg_id, c, pkt, sizeof(pkt));
                assert(n > 0 && n <= (size_t)(mtu - 3));
                ble_rx_result_t r = ble_rx_feed(&rx, pkt, n, 1000 + (int64_t)c * 10, &out_len);
                if (c + 1 < chunks) {
                    assert(r == BLE_RX_NEED_MORE);
                } else {
                    assert(r == BLE_RX_COMPLETE);
                    assert(out_len == plen);
                    assert(memcmp(rx_buf, payload, plen) == 0);
                    assert(rx_buf[plen] == '\0');
                }
            }
            ++msg_id;
        }
    }
}

static void test_chunked_error_rejections(void) {
    char rx_buf[BLE_FRAME_MAX_PAYLOAD + 1];
    ble_rx_reassembler_t rx;
    ble_rx_init(&rx, rx_buf, BLE_FRAME_MAX_PAYLOAD);
    size_t out_len = 0;

    // 未知魔数与截断头。/ Unknown magic and truncated header.
    const uint8_t bad_magic[] = { 0x99, 0x03, 0x00, 0x01, 0x02, 0x00, '{', '}' };
    assert(ble_rx_feed(&rx, bad_magic, sizeof(bad_magic), 100, &out_len) == BLE_RX_ERROR);
    const uint8_t short_hdr[] = { BLE_FRAME_MAGIC, 0x03, 0x00 };
    assert(ble_rx_feed(&rx, short_hdr, sizeof(short_hdr), 100, &out_len) == BLE_RX_ERROR);

    // 超过容量总长。/ Total length beyond capacity.
    uint8_t too_big[BLE_FRAME_HDR_LEN + 2] = { BLE_FRAME_MAGIC, BLE_FRAME_FLAG_START, 0, 1, 0x01, 0x10, '{', '}' };
    assert(ble_rx_feed(&rx, too_big, sizeof(too_big), 100, &out_len) == BLE_RX_ERROR);

    // 乱序分片拒绝。/ Out-of-order chunk rejected.
    uint8_t c0[] = { BLE_FRAME_MAGIC, BLE_FRAME_FLAG_START, 0, 5, 4, 0, '{', 'a' };
    uint8_t c2_bad_seq[] = { BLE_FRAME_MAGIC, BLE_FRAME_FLAG_END, 2, 5, 4, 0, 'b', '}' };
    assert(ble_rx_feed(&rx, c0, sizeof(c0), 100, &out_len) == BLE_RX_NEED_MORE);
    assert(ble_rx_feed(&rx, c2_bad_seq, sizeof(c2_bad_seq), 110, &out_len) == BLE_RX_ERROR);

    // 超时后继分片拒绝。/ Continuation after timeout rejected.
    uint8_t c1[] = { BLE_FRAME_MAGIC, BLE_FRAME_FLAG_END, 1, 5, 4, 0, 'b', '}' };
    assert(ble_rx_feed(&rx, c0, sizeof(c0), 100, &out_len) == BLE_RX_NEED_MORE);
    assert(ble_rx_feed(&rx, c1, sizeof(c1), 100 + BLE_FRAME_TIMEOUT_MS + 1, &out_len) == BLE_RX_ERROR);

    // END 时字节数不足拒绝。/ Premature END rejected.
    uint8_t c1_short[] = { BLE_FRAME_MAGIC, BLE_FRAME_FLAG_END, 1, 5, 4, 0, '}' };
    assert(ble_rx_feed(&rx, c0, sizeof(c0), 200, &out_len) == BLE_RX_NEED_MORE);
    assert(ble_rx_feed(&rx, c1_short, sizeof(c1_short), 210, &out_len) == BLE_RX_ERROR);
}

static void test_adv_beacon_and_semver(void) {
    uint8_t maj = 0, min = 0, pat = 0;
    ble_parse_semver("v0.1.23", &maj, &min, &pat);
    assert(maj == 0 && min == 1 && pat == 23);
    ble_parse_semver("1.12.3-dirty", &maj, &min, &pat);
    assert(maj == 1 && min == 12 && pat == 3);
    ble_parse_semver("invalid", &maj, &min, &pat);
    assert(maj == 0 && min == 0 && pat == 0);

    ble_adv_beacon_t beacon = {
        .battery_soc = 87,
        .battery_valid = true,
        .charging = true,
        .transfer_active = true,
        .transfer_mode = 2,
        .wifi_configured = true,
        .sd_mounted = true,
        .pin_required = false,
        .fw_major = 0,
        .fw_minor = 1,
        .fw_patch = 23,
    };
    uint8_t mfg[READ_PICO_BLE_MFG_DATA_LEN];
    ble_adv_mfg_encode(&beacon, mfg);
    assert(mfg[0] == 0xFF && mfg[1] == 0xFF);
    assert(mfg[2] == READ_PICO_BLE_MFG_PROTO_VER);
    assert(mfg[3] == (BLE_ADV_FLAG_CHARGING | BLE_ADV_FLAG_TRANSFER_ACTIVE |
                      BLE_ADV_FLAG_WIFI_CONFIGURED | BLE_ADV_FLAG_SD_MOUNTED |
                      BLE_ADV_FLAG_BATTERY_VALID));
    assert(mfg[4] == 87);
    assert(mfg[5] == 2);
    assert(mfg[6] == 0 && mfg[7] == 1 && mfg[8] == 23);
}

int main(void) {
    test_uuid_byte_order();
    test_raw_json_single_packet();
    test_chunked_roundtrip_various_mtus();
    test_chunked_error_rejections();
    test_adv_beacon_and_semver();
    puts("ble_framing_host_test: PASS");
    return 0;
}
