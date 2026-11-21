/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：直接验证触屏配网状态机及输入边界。
 * English: Exercise the device provisioning state machine and input bounds.
 * 冻结：仅供宿主测试，不访问网络。/ Frozen: Host tests only; no network access.
 */
#include "transfer_ui_test_env.h"
#include "../main/apps/app_transfer.c"

const app_desc_t app_focus = { .title = "专注时钟 Focus" };
static app_focus_status_t test_focus = { .running = false, .duration_min = 25, .completed_count = 3 };
void app_focus_get_status(app_focus_status_t* out) { *out = test_focus; }
bool app_focus_remote_start(int minutes) {
    if (minutes != 15 && minutes != 25 && minutes != 45) return false;
    test_focus.running = true; test_focus.is_break = false; test_focus.duration_min = (uint16_t)minutes;
    test_focus.remaining_sec = (uint32_t)minutes * 60U;
    return true;
}
void app_focus_remote_stop(void) { test_focus.running = false; test_focus.remaining_sec = 0; }
#include <assert.h>
static void tap(app_ctx_t* ctx, EpdRect rect) {
    ui_gesture_event_t ev = {.type = UI_GESTURE_PRESS, .x0 = rect.x + 2, .y0 = rect.y + 2, .x = rect.x + 2, .y = rect.y + 2};
    on_gesture(ctx, &ev);
    ev.type = UI_GESTURE_TAP;
    on_gesture(ctx, &ev);
}
static void qr_regression(app_ctx_t* ctx) {
    test_status = (read_pico_transfer_status_t){.mode=READ_PICO_TRANSFER_MODE_AP,.state=READ_PICO_TRANSFER_READY,.network_ready=true};
    strcpy(test_status.ssid,"ReadPico-test");
    strcpy(test_status.url,"http://192.168.4.1");
    s_mode = READ_PICO_TRANSFER_MODE_AP;
    on_enter(ctx);
    on_tick(ctx);
    assert(s_qr_ready && !s_qr_url && !strcmp(test_qr_payload,"ReadPico-test"));
    int encodes = test_qr_encodes;
    render(ctx,ctx->fb);
    assert(test_qr_encodes == encodes);
    test_status.cur_bytes++;
    ctx->now_ms += 3000;
    on_tick(ctx);
    assert(test_qr_encodes == encodes);
    EpdRect toggle = control_rect(3);
    assert(toggle.x+toggle.width < 404 && toggle.y+toggle.height <= 518);
    ui_gesture_event_t ev={.type=UI_GESTURE_PRESS,.x0=toggle.x+2,.y0=toggle.y+2,.x=toggle.x+2,.y=toggle.y+2};
    on_gesture(ctx,&ev);
    ev.type=UI_GESTURE_TAP; ev.x=650;
    on_gesture(ctx,&ev);
    assert(!s_qr_url && test_qr_encodes==encodes);
    tap(ctx,toggle);
    assert(s_qr_url && s_qr_ready && !strcmp(test_qr_payload,test_status.url));
    strcpy(test_status.url,"http://192.168.4.2");
    ctx->now_ms += 3000;
    on_tick(ctx);
    assert(!strcmp(test_qr_payload,test_status.url));
    test_status.network_ready=false;
    ctx->now_ms += 3000;
    on_tick(ctx);
    assert(!s_qr_ready && !test_qr_payload[0]);
    encodes=test_qr_encodes;
    tap(ctx,toggle);
    assert(test_qr_encodes==encodes);
    test_status.network_ready=true;
    test_qr_failure=true;
    ctx->now_ms += 3000;
    on_tick(ctx);
    assert(!s_qr_ready);
    test_qr_failure=false;
    queue_network_start(READ_PICO_TRANSFER_MODE_STA);
    assert(!s_qr_ready && !test_qr_payload[0]);
    test_status.mode=READ_PICO_TRANSFER_MODE_STA;
    strcpy(test_status.url,"http://10.20.30.40");
    on_tick(ctx);
    assert(s_qr_url && s_qr_ready && !strcmp(test_qr_payload,test_status.url));
    encodes=test_qr_encodes;
    tap(ctx,toggle);
    render(ctx,ctx->fb);
    assert(test_qr_encodes==encodes);
    stop_session();
    assert(!s_qr_ready && !test_qr_payload[0]);
    test_status=(read_pico_transfer_status_t){0};
    s_mode=READ_PICO_TRANSFER_MODE_AP;
    test_qr_encodes=0;
    ctx->now_ms=0;
}
int main(void) {
    uint8_t fb = 0;
    app_ctx_t ctx = {.fb = &fb};
    on_enter(&ctx);
    on_tick(&ctx);
    assert(test_qr_encodes == 0);
    qr_regression(&ctx);
    for (int i = 0; i < 47; ++i) {
        EpdRect r = password_control_rect(i);
        assert(r.x >= 0 && r.x + r.width <= UI_LOCK_WIDTH);
        assert(r.y >= 0 && r.y + r.height <= UI_LOCK_HEIGHT);
    }
    on_enter(&ctx);
    test_busy = true;
    tap(&ctx, control_rect(1));
    assert(s_view == TRANSFER_HOME);
    test_busy = false;
    tap(&ctx, control_rect(1));
    assert(s_view == TRANSFER_NETWORKS && s_scan_pending);
    network_ui_tick(&ctx);
    assert(s_network_count == 8 && !s_scan_pending);
    tap(&ctx, ui_row_rect(1, 2, 176, 84));
    assert(test_forget_count == 0);
    tap(&ctx, ui_row_rect(1, 2, 600, UI_BTN_H));
    assert(test_forget_count == 0 && s_saved_configured && !s_forget_confirm);
    tap(&ctx, ui_row_rect(1, 2, 176, 84));
    test_forget_error = ESP_FAIL;
    tap(&ctx, ui_row_rect(0, 2, 600, UI_BTN_H));
    assert(test_forget_count == 1 && s_saved_configured && s_forget_confirm);
    test_forget_error = ESP_OK;
    tap(&ctx, ui_row_rect(0, 2, 600, UI_BTN_H));
    assert(test_forget_count == 2 && !s_saved_configured);
    test_configured = true;
    enter_networks();
    network_ui_tick(&ctx);
    tap(&ctx, network_control_rect(9));
    assert(s_network_page == 1);
    tap(&ctx, network_control_rect(7));
    assert(s_network_page == 0);
    tap(&ctx, network_control_rect(0));
    assert(s_view == TRANSFER_PASSWORD && !s_password[0]);
    tap(&ctx, password_control_rect(46));
    assert(test_save_count == 0 && s_view == TRANSFER_PASSWORD);
    ui_gesture_event_t cancel = {.type = UI_GESTURE_PRESS,.x0 = 42,.y0 = 422,.x = 42,.y = 422};
    on_gesture(&ctx, &cancel);
    cancel.type = UI_GESTURE_TAP;
    cancel.x = 650;
    on_gesture(&ctx, &cancel);
    assert(!s_password[0]);
    bool reachable[128] = {0};
    for (int mode = 0; mode < 3; ++mode) {
        s_keyboard_mode = mode;
        for (int i = 0; i < 40; ++i) reachable[(unsigned char)keyboard_chars()[i]] = true;
    }
    reachable[' '] = true;
    for (int c = 32; c < 127; ++c) assert(reachable[c]);
    s_keyboard_mode = 0;
    for (int i = 0; i < 70; ++i) tap(&ctx, password_control_rect(0));
    assert(strlen(s_password) == 64);
    tap(&ctx, password_control_rect(43));
    assert(strlen(s_password) == 63);
    tap(&ctx, password_control_rect(45));
    assert(s_password_visible);
    test_save_error = ESP_FAIL;
    tap(&ctx, password_control_rect(46));
    assert(s_view == TRANSFER_PASSWORD && strlen(s_password) == 63);
    test_save_error = ESP_OK;
    tap(&ctx, password_control_rect(46));
    assert(s_view == TRANSFER_HOME && s_mode == READ_PICO_TRANSFER_MODE_STA && s_start_pending);
    assert(!s_password[0]);
    assert(strcmp(test_saved_ssid, "中文家庭网络") == 0);
    tap(&ctx, control_rect(1));
    network_ui_tick(&ctx);
    tap(&ctx, network_control_rect(0));
    tap(&ctx, password_control_rect(0));
    tap(&ctx, password_control_rect(44));
    assert(s_view == TRANSFER_NETWORKS && !s_password[0]);
    test_scan_count = 0;
    tap(&ctx, network_control_rect(8));
    network_ui_tick(&ctx);
    assert(!s_network_count && s_network_message[0]);
    test_scan_error = ESP_FAIL;
    tap(&ctx, network_control_rect(8));
    network_ui_tick(&ctx);
    assert(!s_network_count && strstr(s_network_message, "FFFF") != NULL);
    // 传书页进入时独占射频（BLE 关闭），退出后释放。
    // Entering the page takes the radio (BLE off); leaving releases it.
    test_scan_error = ESP_OK;
    test_scan_count = 3;
    {
        app_ctx_t c2 = {.fb = &fb};
        int holds = test_ble_hold_calls, rels = test_ble_release_calls;
        on_enter(&c2);
        assert(test_ble_held && test_ble_hold_calls == holds + 1);
        int scans_before = test_scan_calls;
        tap(&c2, network_control_rect(8));
        enter_networks();
        network_ui_tick(&c2);
        assert(test_scan_calls == scans_before + 1 && s_network_count == 3 && test_ble_held);
        transfer_on_exit(&c2);
        assert(!test_ble_held && test_ble_release_calls == rels + 1);
    }
    tap(&ctx, network_control_rect(6));
    assert(s_view == TRANSFER_HOME && s_start_pending && s_mode == READ_PICO_TRANSFER_MODE_STA);
    int stops_before = test_stop_count;
    tap(&ctx, control_rect(2));
    assert(test_stop_count == stops_before + 1 && !s_start_pending);
    assert(ctx.request_return && ctx.request_app == NULL);
    on_enter(&ctx);
    on_tick(&ctx);
    test_status.changed_count = 1;
    ctx.now_ms = 3000;
    assert(on_tick(&ctx) == APP_REDRAW_FULL);
    assert(s_status.changed_count == 1);
    transfer_on_exit(&ctx);
    assert(!test_font_suspended);
    assert(test_store_changes == 1);
    on_enter(&ctx);
    on_tick(&ctx);
    s_root.is_flash = false;
    s_free = 123456;
    stops_before = test_stop_count;
    app_transfer.on_media_lost(&ctx);
    assert(test_font_suspended);
    assert(test_stop_count == stops_before + 1);
    assert(s_media_lost && !s_free && !s_qr_ready && !s_root.path[0]);
    assert(!s_start_pending && !s_session_started && s_view == TRANSFER_HOME);
    s_root.is_flash = true;
    app_transfer.on_media_lost(&ctx);
    assert(test_stop_count == stops_before + 1);

    // 配套接口回调：设置延迟同步与远程空闲切换存储。/ Companion callbacks: deferred settings sync and remote idle storage switch.
    on_enter(&ctx);
    on_tick(&ctx);
    assert(test_last_cfg.settings_get_cb && test_last_cfg.settings_set_cb);
    assert(test_last_cfg.storage_info_cb && test_last_cfg.storage_switch_cb);
    read_pico_device_settings_t got_settings = {0};
    assert(test_last_cfg.settings_get_cb(&got_settings, test_last_cfg.settings_ctx));
    assert(got_settings.book_px == 48 && got_settings.timezone_min == 480);
    got_settings.book_px = 56;
    got_settings.timezone_min = 0;
    got_settings.charge_led = false;
    int apply_before = test_runtime_apply_calls;
    assert(test_last_cfg.settings_set_cb(&got_settings, test_last_cfg.settings_ctx) == ESP_OK);
    assert(test_runtime_apply_calls == apply_before);
    on_tick(&ctx);
    assert(test_runtime_apply_calls == apply_before + 1);
    assert(test_settings.book_px == 56 && test_settings.timezone_min == 0 && !test_settings.charge_led);

    s_storage_info.flash_available = true;
    s_storage_info.sd_available = true;
    test_busy = true;
    stops_before = test_stop_count;
    assert(test_last_cfg.storage_switch_cb(true, test_last_cfg.storage_ctx));
    on_tick(&ctx);
    assert(test_stop_count == stops_before);
    test_busy = false;
    assert(on_tick(&ctx) == APP_REDRAW_PAGE);
    assert(test_stop_count == stops_before + 1 && s_start_pending);
    on_tick(&ctx);
    assert(!s_start_pending);

    // 对时延迟写入、字体切换、看板统计与 PIN 复位。/ Deferred time sync, font select, dashboard stats and PIN reset.
    assert(test_last_cfg.time_get_cb && test_last_cfg.time_set_cb);
    assert(test_last_cfg.font_select_cb && test_last_cfg.dashboard_stats_cb);
    read_pico_time_info_t tinfo = {0};
    assert(test_last_cfg.time_get_cb(&tinfo, test_last_cfg.time_ctx) && tinfo.time_valid);
    int time_calls_before = test_time_set_calls;
    assert(test_last_cfg.time_set_cb(1763729999U, true, 540, test_last_cfg.time_ctx) == ESP_OK);
    assert(test_time_set_calls == time_calls_before);
    on_tick(&ctx);
    assert(test_time_set_calls == time_calls_before + 1 && test_utc_time == 1763729999U && test_settings.timezone_min == 540);
    assert(test_last_cfg.font_select_cb("/sdcard/fonts/New.ttf", test_last_cfg.font_ctx) == ESP_OK);
    assert(!strcmp(test_settings.font_path, "/sdcard/fonts/New.ttf"));
    read_pico_dashboard_stats_t dstats = {0};
    assert(test_last_cfg.dashboard_stats_cb(&dstats, test_last_cfg.dashboard_ctx));
    assert(dstats.total_reading_sec == 3661U && dstats.has_last_book && dstats.last_book_percent == 42 && dstats.has_excerpt);
    assert(test_pin_reset_calls > 0 && test_pmu_poll_calls > 0);

    // BLE 远程启动/切模式/停止钩子：上传忙时拒绝切换和停止，空闲时正常切换与返回。
    // BLE remote start/switch/stop hooks: refuse while busy, switch mode and request return when idle.
    app_transfer_set_preferred_mode(READ_PICO_TRANSFER_MODE_STA);
    on_enter(&ctx);
    assert(s_mode == READ_PICO_TRANSFER_MODE_STA && s_start_pending);
    on_tick(&ctx);
    assert(!s_start_pending);
    test_busy = true;
    assert(!app_transfer_remote_switch_mode(READ_PICO_TRANSFER_MODE_AP));
    assert(!app_transfer_remote_stop(&ctx));
    test_busy = false;
    assert(app_transfer_remote_switch_mode(READ_PICO_TRANSFER_MODE_AP));
    assert(s_mode == READ_PICO_TRANSFER_MODE_AP && s_start_pending);
    ctx.request_return = false;
    assert(app_transfer_remote_stop(&ctx));
    assert(ctx.request_return && !s_start_pending);

    // 传书页收到 /webopen 链接时应排队并自动请求切入网页阅读页。
    // Transfer page receiving a /webopen URL queues it and requests switching to app_web.
    on_enter(&ctx);
    on_tick(&ctx);
    ctx.request_app = NULL;
    snprintf(test_pending_webopen, sizeof(test_pending_webopen), "https://example.com/article");
    test_queued_web_url[0] = 0;
    on_tick(&ctx);
    assert(ctx.request_app == &app_web);
    assert(strcmp(test_queued_web_url, "https://example.com/article") == 0);

    // 图书进度、网页收藏夹与专注时钟伴侣回调验证。
    // Book progress, web bookmarks and focus timer companion callback checks.
    assert(test_last_cfg.book_progress_cb && test_last_cfg.webmarks_list_cb &&
           test_last_cfg.webmarks_upsert_cb && test_last_cfg.webmarks_delete_cb &&
           test_last_cfg.focus_get_cb && test_last_cfg.focus_ctrl_cb);
    read_pico_book_progress_t bp = {0};
    assert(test_last_cfg.book_progress_cb("/sdcard/books/prog.txt", 1024, &bp, test_last_cfg.book_progress_ctx));
    assert(bp.has_progress && bp.percent == 65 && bp.chapter == 2 && bp.byte_off == 512);
    assert(test_last_cfg.webmarks_upsert_cb("示例", "https://example.com", test_last_cfg.webmarks_ctx) == ESP_OK);
    read_pico_webmark_t wm[4];
    assert(test_last_cfg.webmarks_list_cb(wm, 4, test_last_cfg.webmarks_ctx) == 1);
    assert(strcmp(wm[0].url, "https://example.com") == 0);
    assert(test_last_cfg.webmarks_delete_cb("https://example.com", test_last_cfg.webmarks_ctx) == ESP_OK);
    assert(test_last_cfg.webmarks_list_cb(wm, 4, test_last_cfg.webmarks_ctx) == 0);
    read_pico_focus_status_t fs = {0};
    assert(test_last_cfg.focus_get_cb(&fs, test_last_cfg.focus_ctx) && !fs.running && fs.completed_count == 3);
    ctx.request_app = NULL;
    assert(test_last_cfg.focus_ctrl_cb(true, 45, test_last_cfg.focus_ctx) == ESP_OK);
    on_tick(&ctx);
    assert(ctx.request_app == &app_focus && test_focus.running && test_focus.duration_min == 45);

    // 经局域网 POST /transfer/stop 延迟 120 ms 停服并返回，确保 HTTP 200 响应发出后再释放射频。
    // LAN POST /transfer/stop waits 120 ms before stopping and returning so HTTP 200 flushes first.
    assert(test_last_cfg.stop_request_cb != NULL);
    ctx.request_return = false;
    ctx.now_ms = 5000;
    stops_before = test_stop_count;
    assert(test_last_cfg.stop_request_cb(test_last_cfg.stop_request_ctx));
    on_tick(&ctx);
    assert(!ctx.request_return && test_stop_count == stops_before);
    ctx.now_ms = 5080;
    on_tick(&ctx);
    assert(!ctx.request_return && test_stop_count == stops_before);
    ctx.now_ms = 5120;
    on_tick(&ctx);
    assert(ctx.request_return && test_stop_count == stops_before + 1);

    // 回归：管理已保存网络视图 TRANSFER_SAVED 必须正确渲染且 KEY1 逐级返回。
    // Regression: TRANSFER_SAVED must render the saved Wi-Fi screen and KEY1 backs out step by step.
    on_enter(&ctx);
    on_tick(&ctx);
    s_view = TRANSFER_SAVED;
    test_last_header[0] = 0;
    render(&ctx, ctx.fb);
    assert(!strcmp(test_last_header, "已保存 WiFi"));
    assert(on_key(&ctx, UI_KEY_1) == APP_REDRAW_PAGE && s_view == TRANSFER_NETWORKS);
    assert(on_key(&ctx, UI_KEY_1) == APP_REDRAW_PAGE && s_view == TRANSFER_HOME);
    ctx.request_return = false;
    assert(on_key(&ctx, UI_KEY_1) == APP_REDRAW_NONE && ctx.request_return);

    puts("transfer_ui_host_test: PASS");
}
