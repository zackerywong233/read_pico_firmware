/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 只做开机装配：拉起板级硬件、读设置、定 VCOM、放开机图、开字体；
 * 未标定则先拦住进设定页。然后把控制权交给 app_loop。
 *
 * Boot wiring only: board init, settings, VCOM, splash, fonts. If VCOM
 * is unset, the factory page blocks first. Then control goes to app_loop.
 */

#include <stdint.h>
#include <time.h>

#include "ambient_sync.h"
#include "ambient_sync_runtime.h"
#include "app_loop.h"
#include "app_registry.h"
#include "display.h"
#include "epd_highlevel.h"
#include "epdiy.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "pmu_selftest.h"
#include "read_pico_board.h"
#include "read_pico_init.h"
#include "read_pico_pmu.h"
#include "read_pico_sd.h"
#include "settings.h"
#include "sleep.h"
#include "time_sync.h"
#include "ttf_font.h"
#include "ui_kit.h"
#include "vcom_setup.h"

static const char* TAG = "read_pico";

static bool pending_image_verification(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    return running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY;
}

static bool confirm_pending_image(void) {
    if (!pending_image_verification()) return true;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) ESP_LOGE(TAG, "OTA image validation failed: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static void rollback_pending_image(void) {
    if (pending_image_verification()) {
        ESP_LOGE(TAG, "critical boot initialization failed; rolling back OTA image");
        (void)esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

extern const uint8_t lock_4bpp_bin_start[] asm("_binary_lock_4bpp_bin_start");
extern const uint8_t lock_4bpp_bin_end[] asm("_binary_lock_4bpp_bin_end");
extern const uint8_t loading_4bpp_bin_start[] asm("_binary_loading_4bpp_bin_start");
extern const uint8_t loading_4bpp_bin_end[] asm("_binary_loading_4bpp_bin_end");

// 已标定则开机读一次喂给 epdiy。未标定先用板级默认出开机图，随后拦住进标定页。
// Load VCOM once when set. Otherwise keep the board default for the splash,
// then gate into the setup page.
static bool resolve_vcom_at_boot(void) {
    int mv = 0;
    for (int i = 0; i < 3; i++) {
        if (read_pico_pmu_vcom_get(&mv) == ESP_OK) {
            epd_set_vcom((uint16_t)mv);
            ESP_LOGI(TAG, "panel VCOM loaded from PMU");
            return true;
        }
    }
    ESP_LOGW(TAG, "panel VCOM unset, keeping board default until setup");
    return false;
}

// 开机图和锁屏图是预先转好的 4bpp 全屏位图，尺寸对不上说明资源和面板不匹配。
// Splash and lock images are pre-baked 4bpp full-frames. A size mismatch
// means the assets do not match the panel.
static bool images_match_panel(void) {
    size_t lock_size = (size_t)(lock_4bpp_bin_end - lock_4bpp_bin_start);
    size_t loading_size = (size_t)(loading_4bpp_bin_end - loading_4bpp_bin_start);
    size_t expected = (size_t)epd_width() * epd_height() / 2;
    if (lock_size == expected && loading_size == expected) return true;
    ESP_LOGE(
        TAG, "Image size mismatch: lock %u loading %u expected %u",
        (unsigned)lock_size, (unsigned)loading_size, (unsigned)expected
    );
    return false;
}

static void arm_next_ambient_alarm(const app_ambient_settings_t *config) {
    if (!config || !config->enabled) return;
    uint32_t interval = (uint32_t)config->interval_min * 60U;
    uint32_t delay = interval;
    const pmu_snapshot_t *snapshot = read_pico_pmu_get();
    time_t now = snapshot && snapshot->time_ok && snapshot->unix_sec
        ? (time_t)snapshot->unix_sec : time(NULL);
    if (now >= 946684800) {
        time_t local_now = now + (time_t)app_settings_timezone_min() * 60;
        struct tm local;
        if (gmtime_r(&local_now, &local)) {
            uint32_t since_midnight = (uint32_t)local.tm_hour * 3600U +
                (uint32_t)local.tm_min * 60U + (uint32_t)local.tm_sec;
            delay = ambient_sync_next_delay(since_midnight, interval,
                                            config->quiet_start_h, config->quiet_end_h);
        }
    }
    if (delay < 10U) delay = 10U;
    if (delay > 2678400U) delay = 2678400U;
    (void)read_pico_pmu_alarm_set(1, delay);
}

void app_main(void) {
    read_pico_handle_t hw;
    if (read_pico_init(&hw) != ESP_OK) { rollback_pending_image(); return; }
    app_settings_init();
    app_settings_sync_pmu();
    const bool vcom_ok = resolve_vcom_at_boot();
    if (pending_image_verification() && (!hw.pmu_ready || !vcom_ok)) {
        rollback_pending_image();
        return;
    }
    pmu_selftest_bind(hw.sensor);
    app_sleep_bind(hw.touch, hw.sensor);
    const bool st_resume = pmu_selftest_boot_resume();

    if (!images_match_panel()) { rollback_pending_image(); return; }

    EpdiyHighlevelState hl = hw.hl;
    uint8_t* framebuffer = hw.framebuffer;

    app_ambient_settings_t ambient;
    app_settings_ambient_get(&ambient);
    if (hw.pmu_ready) {
        (void)read_pico_pmu_refresh();
        app_time_sync_from_pmu();
    }
    const pmu_snapshot_t *pmu = read_pico_pmu_get();
    if (pmu && pmu->wake_reason == 6 && ambient.enabled && !st_resume && vcom_ok &&
        !pending_image_verification()) {
        ttf_font_init();
        epd_poweron();
        bool quiet = false;
        time_t now = pmu && pmu->time_ok && pmu->unix_sec
            ? (time_t)pmu->unix_sec : time(NULL);
        struct tm local;
        time_t local_now = now + (time_t)app_settings_timezone_min() * 60;
        if (now >= 946684800 && gmtime_r(&local_now, &local)) {
            uint32_t since_midnight = (uint32_t)local.tm_hour * 3600U +
                (uint32_t)local.tm_min * 60U + (uint32_t)local.tm_sec;
            quiet = ambient_sync_is_quiet(since_midnight, ambient.quiet_start_h, ambient.quiet_end_h);
        }
        if (!quiet) (void)ambient_sync_run(&hl, framebuffer);
        arm_next_ambient_alarm(&ambient);
        if (app_enter_host_sleep(APP_SLEEP_DEEP) != ESP_OK) {
            ESP_LOGW(TAG, "ambient wake sleep failed; continuing to normal UI");
        } else return;
    }

    epd_poweron();
    if (pending_image_verification()) {
        read_pico_status_t panel_status;
        if (!read_pico_rails_on() || read_pico_get_status(&panel_status) != ESP_OK ||
            !(panel_status.ioe_input & (1U << 5))) {
            rollback_pending_image();
            return;
        }
    }
    read_pico_i2c_census_take();
    epd_clear();
    epd_hl_set_all_white(&hl);
    ui_draw_full_image(framebuffer, loading_4bpp_bin_start);
    enum EpdDrawError splash_result = update_display_from_white(&hl);
    if (pending_image_verification() && splash_result != EPD_DRAW_SUCCESS) {
        rollback_pending_image();
        return;
    }

    if (ttf_font_init() != ESP_OK) { rollback_pending_image(); return; }

    if (!hw.touch_ready) {
        ESP_LOGE(TAG, "No touch controller, UI cannot run");
        rollback_pending_image();
        return;
    }

    if (hw.pmu_ready && !vcom_ok) {
        vcom_setup_run(&hl, framebuffer, hw.touch);
    }

    if (!confirm_pending_image()) { rollback_pending_image(); return; }

    if (app_settings_saver_style() == APP_SAVER_IMAGE) {
        (void)read_pico_sd_start_probe();
    }

    app_loop_run(&(app_loop_config_t){
        .hl = &hl,
        .fb = framebuffer,
        .acc = hw.sensor,
        .tp = hw.touch,
        .sensor_ready = hw.sensor_ready,
        // 自检跑到一半断电的话，重新上电直接回自检页接着跑。
        // Resume the self-test page if a power-cut interrupted it.
        .first_app = st_resume ? app_selftest_page() : app_home_page(),
    });
}
