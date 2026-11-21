/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：BLE 配套控制协议 JSON 信封、文件名清洗、短文本归一化与 Wi-Fi 凭据校验宿主测试。
 *
 * English: Host tests for BLE companion JSON envelopes, clip filename sanitization, text normalization and Wi-Fi validation.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "read_pico_ble_protocol.h"

static void test_clip_filename_sanitization(void) {
    char out[READ_PICO_BLE_CLIP_NAME_MAX];

    assert(ble_sanitize_clip_filename("购物清单", "ble_1", out));
    assert(strcmp(out, "购物清单.txt") == 0);

    // 去除重复 .txt 扩展名。/ Avoid double .txt extension.
    assert(ble_sanitize_clip_filename("Notes.TXT", "ble_1", out));
    assert(strcmp(out, "Notes.txt") == 0);

    // 路径穿越与非法字符必须清洗或回退。/ Path traversal and illegal chars stripped or fallback.
    assert(ble_sanitize_clip_filename("../etc/passwd", "ble_fallback", out));
    assert(strstr(out, "..") == NULL && strchr(out, '/') == NULL);
    assert(strcmp(out, "etcpasswd.txt") == 0);

    assert(ble_sanitize_clip_filename("///:::*?\"<>|...", "ble_20261121", out));
    assert(strcmp(out, "ble_20261121.txt") == 0);

    assert(ble_sanitize_clip_filename("", "ble_empty", out));
    assert(strcmp(out, "ble_empty.txt") == 0);
}

static void test_clip_text_normalization(void) {
    char out[READ_PICO_BLE_CLIP_TEXT_MAX + 1];
    size_t len = 0;

    assert(ble_normalize_clip_text("第一行\r\n第二行\r第三行", out, sizeof(out), &len));
    assert(strcmp(out, "第一行\n第二行\n第三行") == 0);
    assert(len == strlen(out));

    // 拒绝空文本与非法控制字符。/ Reject empty text and bad control bytes.
    assert(!ble_normalize_clip_text("", out, sizeof(out), &len));
    const char bad_ctrl[] = { 'a', 0x07, 'b', 0 };
    assert(!ble_normalize_clip_text(bad_ctrl, out, sizeof(out), &len));
    const char bad_utf8[] = { (char)0xFF, (char)0xFE, 0 };
    assert(!ble_normalize_clip_text(bad_utf8, out, sizeof(out), &len));
}

static void test_wifi_credentials_validation(void) {
    assert(ble_valid_wifi_credentials("MyWiFi", "12345678"));
    assert(ble_valid_wifi_credentials("OpenGuest", ""));
    assert(ble_valid_wifi_credentials("中文网络", "abcdefgh"));
    assert(!ble_valid_wifi_credentials("", "12345678"));
    assert(!ble_valid_wifi_credentials("MyWiFi", "short"));
    assert(!ble_valid_wifi_credentials("Bad\nSSID", "12345678"));
    assert(!ble_valid_wifi_credentials("MyWiFi", "中文密码1234"));
}

static void test_command_envelope_and_errors(void) {
    cJSON *root = NULL;
    bool has_id = false;
    int id = 0;
    ble_cmd_type_t cmd = BLE_CMD_UNKNOWN;
    char name[32] = {0};

    const char *req = "{\"id\":42,\"cmd\":\"transfer_start\",\"mode\":\"sta\"}";
    assert(ble_parse_command_envelope(req, strlen(req), &root, &has_id, &id, &cmd, name));
    assert(root != NULL && has_id && id == 42 && cmd == BLE_CMD_TRANSFER_START);
    assert(strcmp(name, "transfer_start") == 0);
    cJSON_Delete(root);

    const char *req_no_id = "{\"cmd\":\"ping\"}";
    assert(ble_parse_command_envelope(req_no_id, strlen(req_no_id), &root, &has_id, &id, &cmd, name));
    assert(root != NULL && !has_id && cmd == BLE_CMD_PING);
    cJSON_Delete(root);

    const char *req_unknown = "{\"id\":9,\"cmd\":\"vcom_write\"}";
    assert(ble_parse_command_envelope(req_unknown, strlen(req_unknown), &root, &has_id, &id, &cmd, name));
    assert(cmd == BLE_CMD_UNKNOWN);
    cJSON_Delete(root);

    assert(!ble_parse_command_envelope("{}", 2, &root, &has_id, &id, &cmd, name));
    assert(!ble_parse_command_envelope("{\"id\":\"bad\",\"cmd\":\"ping\"}", 26, &root, &has_id, &id, &cmd, name));
    assert(!ble_parse_command_envelope("{\"cmd\":\"ping\"}trailing", 22, &root, &has_id, &id, &cmd, name));

    cJSON *err = ble_make_error_response(true, 42, "transfer_start", "busy", "忙");
    assert(err != NULL);
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(err, "ok")));
    assert(cJSON_GetObjectItemCaseSensitive(err, "id")->valueint == 42);
    assert(strcmp(cJSON_GetObjectItemCaseSensitive(err, "code")->valuestring, "busy") == 0);
    cJSON_Delete(err);

    // 鉴权门禁、应用路由与 HTTP(S) URL 校验。/ Auth gating, app targets and HTTP(S) URL checks.
    assert(!ble_cmd_requires_auth(BLE_CMD_PING));
    assert(!ble_cmd_requires_auth(BLE_CMD_STATUS));
    assert(!ble_cmd_requires_auth(BLE_CMD_AUTH_VERIFY));
    assert(ble_cmd_requires_auth(BLE_CMD_WIFI_SAVE));
    assert(ble_cmd_requires_auth(BLE_CMD_TRANSFER_START));
    assert(ble_cmd_requires_auth(BLE_CMD_WEBOPEN));
    assert(ble_cmd_requires_auth(BLE_CMD_LOCK));
    assert(ble_parse_cmd_type("auth_verify") == BLE_CMD_AUTH_VERIFY);
    assert(ble_parse_cmd_type("webopen") == BLE_CMD_WEBOPEN);
    assert(ble_parse_cmd_type("open_app") == BLE_CMD_OPEN_APP);
    assert(ble_parse_cmd_type("agent_set") == BLE_CMD_AGENT_SET);
    assert(ble_parse_cmd_type("ambient_set") == BLE_CMD_AMBIENT_SET);
    assert(ble_parse_cmd_type("refresh_screen") == BLE_CMD_REFRESH_SCREEN);
    assert(ble_parse_cmd_type("lock") == BLE_CMD_LOCK);
    assert(ble_parse_cmd_type("webmarks_get") == BLE_CMD_WEBMARKS_GET);
    assert(ble_parse_cmd_type("webmarks_add") == BLE_CMD_WEBMARKS_ADD);
    assert(ble_parse_cmd_type("webmarks_del") == BLE_CMD_WEBMARKS_DEL);
    assert(ble_parse_cmd_type("focus_get") == BLE_CMD_FOCUS_GET);
    assert(ble_parse_cmd_type("focus_start") == BLE_CMD_FOCUS_START);
    assert(ble_parse_cmd_type("focus_stop") == BLE_CMD_FOCUS_STOP);
    assert(ble_cmd_requires_auth(BLE_CMD_WEBMARKS_ADD));
    assert(ble_cmd_requires_auth(BLE_CMD_FOCUS_START));
    assert(ble_parse_app_target("books") == BLE_APP_TARGET_BOOKS);
    assert(ble_parse_app_target("vcom") == BLE_APP_TARGET_NONE);
    assert(ble_valid_http_url("https://example.com/a?b=1", 256));
    assert(!ble_valid_http_url("ftp://example.com", 256));
    assert(!ble_valid_http_url("https://bad url", 256));
}

int main(void) {
    test_clip_filename_sanitization();
    test_clip_text_normalization();
    test_wifi_credentials_validation();
    test_command_envelope_and_errors();
    puts("ble_protocol_host_test: PASS");
    return 0;
}
