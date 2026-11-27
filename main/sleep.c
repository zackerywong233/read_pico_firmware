/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 锁屏进睡、浅睡等待按键或拿起、软睡/关机拉掉 EN。
 *
 * Enter lock and sleep, light-sleep wait for key or pickup, and drop EN
 * for soft sleep / power-off.
 */

#include "sleep.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include "app.h"
#include "ambient_sync.h"
#include "ble_service.h"
#include "book_cover.h"
#include "book_image.h"
#include "reading_stats.h"
#include "display.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_private/sleep_cpu.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pmu_selftest.h"
#include "read_pico_board.h"
#include "read_pico_buzzer.h"
#include "read_pico_pmu.h"
#include "sc7a20h_lab.h"
#include "read_pico_sd.h"
#include "settings.h"
#include "time_sync.h"
#include "ttf_font.h"
#include "ui_heatmap.h"
#include "ui_kit.h"

static const char* TAG = "read_pico";

// 主机下电后等 EN 落下的上限；PMU 确认后约 100 ms 拉低，超时视为失败。
// Upper bound for EN to fall after power-down; the PMU drops it about 100 ms after acknowledging.
#define HOST_EN_DROP_WAIT_MS 5000

extern const uint8_t lock_4bpp_bin_start[] asm("_binary_lock_4bpp_bin_start");

static cst836u_handle_t s_tp;
static sc7a20h_handle_t s_acc;
static bool s_acc_was_on;

static void arm_ambient_alarm_before_sleep(app_sleep_mode_t mode) {
    app_ambient_settings_t cfg;
    app_settings_ambient_get(&cfg);
    if (mode != APP_SLEEP_DEEP || !read_pico_pmu_ready()) return;
    if (!cfg.enabled) {
        (void)read_pico_pmu_alarm_set(0, 10);
        return;
    }
    time_t now = 0;
    if (!app_time_now_utc(&now)) now = time(NULL);
    uint32_t interval = (uint32_t)cfg.interval_min * 60U;
    uint32_t delay = interval;
    if (now >= 946684800) {
        time_t local_now = now + (time_t)app_settings_timezone_min() * 60;
        struct tm local;
        if (gmtime_r(&local_now, &local)) {
            uint32_t midnight = (uint32_t)local.tm_hour * 3600U +
                (uint32_t)local.tm_min * 60U + (uint32_t)local.tm_sec;
            delay = ambient_sync_next_delay(midnight, interval, cfg.quiet_start_h, cfg.quiet_end_h);
        }
    }
    if (delay < 10) delay = 10;
    if (delay > 2678400U) delay = 2678400U;
    (void)read_pico_pmu_alarm_set(1, delay);
}

void app_sleep_bind(cst836u_handle_t tp, sc7a20h_handle_t acc) {
    s_tp = tp;
    s_acc = acc;
    // 没开 PM 时 IDF 不分配 CPU 保存区，浅睡里 CPU 不会掉电；这里手动申请一次。
    // Without PM, IDF never allocates CPU retention, so light sleep keeps the CPU powered; request it once here.
    esp_err_t err = sleep_cpu_configure(true);
    if (err != ESP_OK) ESP_LOGW(TAG, "CPU light-sleep power-down off: %s", esp_err_to_name(err));
}

bool app_lock_powers_down(void) {
    return app_settings_sleep_mode() != APP_SLEEP_LIGHT;
}

// 触摸、加速度计都在常开 3V3 上，ESP 浅睡或 EN 拉低都不会断它们的电，必须主动送进最低功耗态。
// Touch and accelerometer sit on the always-on 3V3; neither light sleep nor EN low cuts them, so park them explicitly.
static void periph_suspend(bool host_off) {
    app_ble_suspend();
    if (read_pico_buzzer_score_busy()) (void)read_pico_buzzer_score_stop();
    esp_err_t ioe = read_pico_ioe_sleep(host_off);
    if (ioe != ESP_OK) ESP_LOGW(TAG, "ioe sleep %s", esp_err_to_name(ioe));
    s_acc_was_on = sc7a20h_powered(s_acc);
    if (s_acc_was_on) (void)sc7a20h_power_down(s_acc);
    if (s_tp != NULL && cst836u_get_mode(s_tp) != CST836U_MODE_DEEPSLEEP) {
        esp_err_t err = cst836u_set_mode(s_tp, CST836U_MODE_DEEPSLEEP);
        if (err != ESP_OK) ESP_LOGW(TAG, "touch deepsleep %s", esp_err_to_name(err));
    }
}

// 深睡的触摸只认 TP_RST 复位；加速度计按睡前的配置重新上电。
// Deep-sleeping touch only wakes via TP_RST; the accelerometer comes back with its pre-sleep config.
static void periph_resume(void) {
    (void)read_pico_ioe_wake();
    if (s_tp != NULL && cst836u_get_mode(s_tp) == CST836U_MODE_DEEPSLEEP) {
        esp_err_t err = cst836u_wake(s_tp);
        if (err != ESP_OK) ESP_LOGW(TAG, "touch wake %s", esp_err_to_name(err));
    }
    if (s_acc_was_on) (void)sc7a20h_power_up(s_acc);
    s_acc_was_on = false;
    app_ble_resume();
}

static void lock_arm_ioe_wakeup(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << READ_PICO_IOE_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    read_pico_clear_ioe_int();
    gpio_wakeup_enable((gpio_num_t)READ_PICO_IOE_INT_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
}

void app_lock_wait_key_idle(int timeout_ms) {
    int64_t start = esp_timer_get_time() / 1000;
    int64_t high_from = 0;
    while (esp_timer_get_time() / 1000 - start < timeout_ms) {
        read_pico_pmu_drain_events();
        read_pico_clear_ioe_int();
        bool held = false;
        if (read_pico_pmu_poll() == ESP_OK) {
            held = (read_pico_pmu_get()->key_state & 0x01) != 0;
        }
        bool int_high = gpio_get_level((gpio_num_t)READ_PICO_IOE_INT_GPIO) != 0;
        if (!held && int_high) {
            if (high_from == 0) high_from = esp_timer_get_time() / 1000;
            if (esp_timer_get_time() / 1000 - high_from >= 150) return;
        } else {
            high_from = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

app_wake_source_t app_last_wake_source(void) {
    return (app_wake_source_t)app_settings_last_wake();
}

static void pickup_ack(sc7a20h_handle_t acc) {
    sc7a20h_events_t ev;
    sc7a20h_read_events(acc, &ev);
    sc7a20h_ack_int(acc);
}

static bool pickup_ia(sc7a20h_handle_t acc) {
    sc7a20h_events_t ev;
    sc7a20h_aoi_src_t aoi;
    if (sc7a20h_read_events(acc, &ev) != ESP_OK) return false;
    sc7a20h_aoi_decode(ev.aoi1_src, &aoi);
    return aoi.ia;
}

// 第一次低电平不够：高通余波还会再拉高。要连续安静一段时间才进浅睡。
// A first low is not enough: high-pass ringing can rise again. Wait for a quiet stretch before light sleep.
static bool pickup_wait_quiet(sc7a20h_handle_t acc, int quiet_ms, int timeout_ms) {
    int64_t start = esp_timer_get_time() / 1000;
    int64_t low_from = 0;
    while (esp_timer_get_time() / 1000 - start < timeout_ms) {
        if (sc7a20h_int1_level(acc) == 0) {
            if (low_from == 0) low_from = esp_timer_get_time() / 1000;
            if (esp_timer_get_time() / 1000 - low_from >= quiet_ms) return true;
        } else {
            pickup_ack(acc);
            low_from = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

app_wake_source_t app_light_sleep_wait(sc7a20h_handle_t acc) {
    bool pickup = acc != NULL && app_settings_pickup_wake();
    bool acc_armed = false;
    periph_suspend(false);
    lock_arm_ioe_wakeup();
    if (pickup) {
        sc7a20h_motion_cfg_t motion = SC7A20H_MOTION_DEFAULT();
        motion.ths_mg = 350;
        motion.duration = 3;
        sc7a20h_arm_pickup_wake(acc, &motion);
        acc_armed = true;
        pickup_ack(acc);
        if (!pickup_wait_quiet(acc, 400, 2000)) {
            pickup = false;
            ESP_LOGW(TAG, "pickup INT1 stuck high, key only");
        } else {
            sc7a20h_config_light_sleep_wakeup(acc);
            if (!pickup_wait_quiet(acc, 400, 1500)) {
                gpio_wakeup_disable(sc7a20h_int1_gpio(acc));
                pickup = false;
                ESP_LOGW(TAG, "pickup INT1 re-asserted, key only");
            } else {
                // 安静窗口里若 INT1 闪过，ESP 会留下高电平唤醒挂起，关掉再打开清掉。
                // If INT1 glitched in the quiet window, ESP keeps a high-level wake pending; disable then re-enable to clear it.
                gpio_wakeup_disable(sc7a20h_int1_gpio(acc));
                sc7a20h_config_light_sleep_wakeup(acc);
                if (sc7a20h_int1_level(acc) > 0) {
                    gpio_wakeup_disable(sc7a20h_int1_gpio(acc));
                    pickup = false;
                    ESP_LOGW(TAG, "pickup INT1 high at sleep, key only");
                } else {
                    ESP_LOGI(
                        TAG, "wait key GPIO%d or pickup GPIO%d",
                        READ_PICO_IOE_INT_GPIO, (int)sc7a20h_int1_gpio(acc)
                    );
                }
            }
        }
    } else {
        ESP_LOGI(TAG, "wait key on IOE_INT GPIO%d", READ_PICO_IOE_INT_GPIO);
    }

    app_wake_source_t wake = APP_WAKE_NONE;
    bool slept = false;
    for (;;) {
        read_pico_clear_ioe_int();
        if (!slept && gpio_get_level((gpio_num_t)READ_PICO_IOE_INT_GPIO) == 0) {
            read_pico_pmu_drain_events();
            read_pico_clear_ioe_int();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!slept && pickup && sc7a20h_int1_level(acc) > 0) {
            pickup_ack(acc);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (slept && gpio_get_level((gpio_num_t)READ_PICO_IOE_INT_GPIO) == 0) {
            if (read_pico_pmu_take_key_wakeup()) {
                wake = APP_WAKE_KEY;
                break;
            }
            read_pico_clear_ioe_int();
            continue;
        }
        if (slept && pickup && sc7a20h_int1_level(acc) > 0 && pickup_ia(acc)) {
            wake = APP_WAKE_PICKUP;
            ESP_LOGI(TAG, "pickup wake");
            break;
        }
        if (slept && pickup && sc7a20h_int1_level(acc) > 0) {
            pickup_ack(acc);
            pickup_wait_quiet(acc, 400, 1500);
            continue;
        }
        int64_t t0 = esp_timer_get_time();
        esp_light_sleep_start();
        slept = true;
        int64_t dt_ms = (esp_timer_get_time() - t0) / 1000;
        read_pico_clear_ioe_int();
        if (read_pico_pmu_take_key_wakeup()) {
            wake = APP_WAKE_KEY;
            break;
        }
        if (pickup && sc7a20h_int1_level(acc) > 0 && pickup_ia(acc)) {
            if (dt_ms < 80) {
                ESP_LOGW(TAG, "pickup ignored, sleep %lld ms", (long long)dt_ms);
                pickup_ack(acc);
                pickup_wait_quiet(acc, 400, 1500);
                continue;
            }
            wake = APP_WAKE_PICKUP;
            ESP_LOGI(TAG, "pickup wake");
            break;
        }
        if (pickup && sc7a20h_int1_level(acc) > 0) {
            pickup_ack(acc);
            pickup_wait_quiet(acc, 400, 1500);
        }
    }

    gpio_wakeup_disable((gpio_num_t)READ_PICO_IOE_INT_GPIO);
    if (acc_armed) {
        gpio_wakeup_disable(sc7a20h_int1_gpio(acc));
        sc7a20h_events_t ev;
        sc7a20h_read_events(acc, &ev);
        sc7a20h_power_down(acc);
        sc7a20h_int1_begin(acc);
    }
    periph_resume();
    app_settings_set_last_wake((uint8_t)wake);
    return wake;
}

esp_err_t app_enter_host_sleep(app_sleep_mode_t mode) {
    // 断电前把高精度 ESP32 晶振时钟回写 CW32 RTC，抵消本次清醒期间的 LSI 漂移。
    // Flush ESP32 crystal time to CW32 RTC before power-down to zero out awake LSI drift.
    (void)app_time_flush_to_pmu();
    periph_suspend(true);
    pmu_selftest_prepare_powerdown();
    esp_err_t err = mode == APP_SLEEP_OFF
        ? read_pico_pmu_power_off()
        : read_pico_pmu_report_sleep();
    ESP_LOGI(TAG, "%s %s", mode == APP_SLEEP_OFF ? "power off" : "SOFT_SLEEP", esp_err_to_name(err));
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(HOST_EN_DROP_WAIT_MS));
        err = ESP_ERR_TIMEOUT;
        ESP_LOGE(TAG, "EN still high after PMU ack");
    }
    // PMU 没把 EN 拉下来：恢复外设交还调用方，不能停在这里空转耗电。
    // The PMU did not drop EN: restore peripherals and return instead of idling at full power.
    periph_resume();
    return err;
}

static bool load_saver_image(uint16_t target_w, uint16_t target_h,
                             uint8_t** pixels_out, uint16_t* width_out, uint16_t* height_out) {
    read_pico_sd_info_t sd = {0};
    esp_err_t sd_err = read_pico_sd_get_info(&sd);
    if (!sd.mounted) {
        if (sd_err != ESP_ERR_NOT_FINISHED) (void)read_pico_sd_start_probe();
        return false;
    }
    static const char* paths[] = {
        "/sdcard/agent_screensaver.jpg", "/sdcard/agent_screensaver.png",
        "/sdcard/screensaver.jpg", "/sdcard/screensaver.png"
    };
    struct stat stats[sizeof(paths) / sizeof(paths[0])];
    bool exists[sizeof(paths) / sizeof(paths[0])] = {0};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i)
        exists[i] = stat(paths[i], &stats[i]) == 0;
    for (size_t tried = 0; tried < sizeof(paths) / sizeof(paths[0]); ++tried) {
        size_t best = sizeof(paths) / sizeof(paths[0]);
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
            if (exists[i] && (best == sizeof(paths) / sizeof(paths[0])
                || stats[i].st_mtime > stats[best].st_mtime)) best = i;
        }
        if (best == sizeof(paths) / sizeof(paths[0])) break;
        exists[best] = false;
        FILE* f = fopen(paths[best], "rb");
        if (!f) continue;
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); continue; }
        long size = ftell(f);
        if (size <= 0 || size > 2 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
            fclose(f);
            continue;
        }
        uint8_t* data = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!data) { fclose(f); return false; }
        size_t got = fread(data, 1, (size_t)size, f);
        fclose(f);
        if (got == (size_t)size) {
            uint8_t* pixels = NULL;
            uint16_t width = 0, height = 0;
            bool valid = book_image_decode_cover(data, got, target_w, target_h,
                                                 2 * 1024 * 1024, &pixels, &width, &height);
            heap_caps_free(data);
            if (valid && pixels && width && height) {
                *pixels_out = pixels;
                *width_out = width;
                *height_out = height;
                return true;
            }
            free(pixels);
            continue;
        }
        heap_caps_free(data);
    }
    return false;
}

static bool draw_saver_image(uint8_t* framebuffer) {
    const int screen_w = epd_rotated_display_width();
    const int screen_h = epd_rotated_display_height();
    if (screen_w < 1 || screen_h < 1) return false;
    uint8_t* pixels = NULL;
    uint16_t image_w = 0, image_h = 0;
    if (!load_saver_image((uint16_t)screen_w, (uint16_t)screen_h,
                          &pixels, &image_w, &image_h)) return false;
    ui_clear_page(framebuffer);
    // 锁屏壁纸已按屏幕尺寸等比居中铺满解码，整屏逐像素铺写无留边。
    // The wallpaper is decoded with aspect-fill center crop to screen size; paint edge-to-edge without borders.
    for (int y = 0; y < screen_h; ++y) {
        size_t sy = (size_t)((int64_t)y * image_h / screen_h);
        for (int x = 0; x < screen_w; ++x) {
            size_t sx = (size_t)((int64_t)x * image_w / screen_w);
            epd_draw_pixel(x, y, pixels[sy * image_w + sx], framebuffer);
        }
    }
    free(pixels);
    return true;
}

static bool draw_saver_quote(uint8_t* framebuffer) {
    char title[128] = {0}, excerpt[320] = {0};
    if (!reading_stats_get_excerpt(title, sizeof(title), excerpt, sizeof(excerpt))) return false;
    ui_clear_page(framebuffer);
    ui_text_vc(framebuffer, UI_LOCK_WIDTH / 2, 260, UI_PX_TITLE,
               "此刻摘录", EPD_DRAW_ALIGN_CENTER, false);
    ui_hairline(framebuffer, 330, UI_MARGIN, ui_content_width(), UI_GRAY_BLACK);
    ui_text(framebuffer, UI_MARGIN, 390, UI_PX_SECTION, title,
            EPD_DRAW_ALIGN_LEFT, false);
    const char* start = excerpt;
    int y = 470;
    char wrapped[160];
    while (*start && y < UI_LOCK_HEIGHT - 220) {
        if (*start == '\r' || *start == '\n') {
            if (*start == '\r' && start[1] == '\n') start += 2;
            else ++start;
            y += UI_PX_BODY + 18;
            continue;
        }
        const char* p = start;
        const char* end = start;
        size_t remaining = strlen(start);
        while (remaining && *p != '\r' && *p != '\n') {
            const unsigned char c = (unsigned char)*p;
            size_t char_len = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2
                : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 1;
            if (char_len > remaining) char_len = 1;
            for (size_t i = 1; i < char_len; ++i) {
                if (((unsigned char)p[i] & 0xc0) != 0x80) { char_len = 1; break; }
            }
            const char* next = p + char_len;
            size_t n = (size_t)(next - start);
            if (n >= sizeof(wrapped)) break;
            memcpy(wrapped, start, n);
            wrapped[n] = '\0';
            if (ttf_text_width_px(UI_PX_BODY, wrapped) > ui_content_width() && end != start) break;
            end = next;
            p = next;
            remaining -= char_len;
        }
        if (end == start) {
            const unsigned char c = (unsigned char)*start;
            size_t char_len = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2
                : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 1;
            size_t remaining_start = strlen(start);
            if (char_len > remaining_start) char_len = 1;
            end = start + char_len;
        }
        size_t n = (size_t)(end - start);
        memcpy(wrapped, start, n);
        wrapped[n] = '\0';
        ui_text(framebuffer, UI_MARGIN, y, UI_PX_BODY, wrapped, EPD_DRAW_ALIGN_LEFT, false);
        y += UI_PX_BODY + 18;
        start = end;
        if (*start == '\r' && start[1] == '\n') start += 2;
        else if (*start == '\r' || *start == '\n') ++start;
    }
    char line[64];
    snprintf(line, sizeof(line), "累计阅读 %lu 分钟",
             (unsigned long)(reading_stats_total_seconds() / 60));
    ui_text_vc(framebuffer, UI_LOCK_WIDTH / 2, UI_LOCK_HEIGHT - 120,
               UI_PX_CAPTION, line, EPD_DRAW_ALIGN_CENTER, false);
    return true;
}


// 当前在读书封面锁屏：优先 40ms 内直读 lock_current.4bpp 缓存，纯文本书回退满屏精装书封。
// Current-book lockscreen: fast-load lock_current.4bpp (<40ms) or draw a full-screen paperback cover fallback.
static bool draw_saver_book_cover(uint8_t* framebuffer) {
    const size_t fb_bytes = (size_t)epd_width() * (size_t)epd_height() / 2;
    if (book_cover_load_lock_cache(framebuffer, fb_bytes)) {
        return true;
    }
    char book_title[128] = {0};
    uint8_t pct = 0;
    if (!ttf_font_ready() || !reading_stats_get_last_book(book_title, sizeof(book_title), &pct) || !book_title[0]) {
        return false;
    }
    // 去掉常见扩展名。/ Strip common file extensions.
    char* dot = strrchr(book_title, '.');
    if (dot && dot > book_title && strlen(dot) <= 6) *dot = '\0';

    ui_clear_page(framebuffer);
    EpdRect outer = { 48, 64, UI_LOCK_WIDTH - 96, UI_LOCK_HEIGHT - 128 };
    ui_draw_round_rect(framebuffer, outer, 16, UI_GRAY_BLACK);
    EpdRect inner = ui_inset_rect(outer, 16);
    ui_draw_round_rect(framebuffer, inner, 10, UI_GRAY_LIGHT);
    // 书脊装饰线。/ Spine rule.
    epd_fill_rect((EpdRect){ outer.x + 36, outer.y + 18, 3, outer.height - 36 }, UI_GRAY_LIGHT, framebuffer);

    const int cx = outer.x + 24 + (outer.width - 24) / 2;
    ui_text_vc(framebuffer, cx, outer.y + 110, UI_PX_CAPTION, "正在阅读", EPD_DRAW_ALIGN_CENTER, false);

    // 居中折行绘制书名（最多 3 行）。/ Centered wrapped title (up to 3 lines).
    const int max_w = inner.width - 96;
    const char* p = book_title;
    int y = outer.y + 340;
    for (int line = 0; line < 3 && *p; ++line) {
        char buf[128] = {0};
        const char* cur = p;
        const char* best = p;
        while (*cur) {
            unsigned char c = (unsigned char)*cur;
            size_t n = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : 4;
            if (n > strlen(cur)) n = 1;
            size_t len = (size_t)(cur + n - p);
            if (len >= sizeof(buf)) break;
            memcpy(buf, p, len);
            buf[len] = '\0';
            if (ttf_text_width_px(52, buf) > max_w && best != p) break;
            cur += n;
            best = cur;
        }
        if (best == p) break;
        size_t len = (size_t)(best - p);
        if (len >= sizeof(buf)) len = sizeof(buf) - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
        ui_text_vc(framebuffer, cx, y, 52, buf, EPD_DRAW_ALIGN_CENTER, false);
        y += 74;
        p = best;
    }

    // 进度条与已读百分比。/ Reading progress bar + percentage.
    const int bar_w = 340;
    const int bar_x = cx - bar_w / 2;
    const int bar_y = outer.y + outer.height - 220;
    epd_fill_rect((EpdRect){ bar_x, bar_y, bar_w, 10 }, UI_GRAY_LIGHT, framebuffer);
    epd_fill_rect((EpdRect){ bar_x, bar_y, bar_w * (int)pct / 100, 10 }, UI_GRAY_BLACK, framebuffer);
    char prog[48];
    snprintf(prog, sizeof(prog), "已读 %u%%", (unsigned)pct);
    ui_text_vc(framebuffer, cx, bar_y + 42, UI_PX_CAPTION, prog, EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(framebuffer, cx, outer.y + outer.height - 76, UI_PX_CAPTION, "小纸 Pico", EPD_DRAW_ALIGN_CENTER, false);
    return true;
}

// 阅读热力图锁屏：纯 NVS 统计 + 本地时间，不依赖 TF 卡或 Wi-Fi。
// Reading heatmap lock face: pure NVS stats + local time, independent of SD card and Wi-Fi.
static bool draw_saver_heatmap(uint8_t* framebuffer) {
    if (!ttf_font_ready()) return false;
    struct tm local;
    uint32_t today_day = 0;
    if (app_time_now_local(&local, NULL)) {
        today_day = reading_stats_day_from_ymd(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    }
    reading_heatmap_t hm;
    (void)reading_stats_get_heatmap(today_day, &hm);
    char book_title[128] = {0};
    uint8_t pct = 0;
    bool have_book = reading_stats_get_last_book(book_title, sizeof(book_title), &pct);
    ui_heatmap_render_lockscreen(framebuffer, &hm, reading_stats_total_seconds(),
                                 have_book ? book_title : NULL, pct);
    return true;
}

void enter_lock_and_sleep(
    EpdiyHighlevelState* hl, int64_t* ignore_until_ms, sc7a20h_handle_t acc
) {
    // 乐曲的 32 kHz 采样中断和线圈电流会干扰锁屏整屏刷新，先停再上屏轨。
    // The score's 32 kHz sample ISR and coil current disturb the lock-face full refresh; stop it before the rails come up.
    if (read_pico_buzzer_score_busy()) (void)read_pico_buzzer_score_stop();
    uint8_t* framebuffer = epd_hl_get_framebuffer(hl);
    epd_poweron();
    epd_clear();
    epd_hl_set_all_white(hl);
    bool custom = false;
    if (app_settings_saver_style() == APP_SAVER_IMAGE) custom = draw_saver_image(framebuffer);
    else if (app_settings_saver_style() == APP_SAVER_QUOTE) custom = draw_saver_quote(framebuffer);
    else if (app_settings_saver_style() == APP_SAVER_BOOK_COVER) custom = draw_saver_book_cover(framebuffer);
    else if (app_settings_saver_style() == APP_SAVER_HEATMAP) custom = draw_saver_heatmap(framebuffer);
    if (!custom) ui_draw_full_image(framebuffer, lock_4bpp_bin_start);
    epd_hl_update_screen_from_white(hl, MODE_GC16, 25);

    app_lock_wait_key_idle(800);
    app_sleep_mode_t mode = app_settings_sleep_mode();
    ESP_LOGI(TAG, "lock %s", app_sleep_mode_name(mode));

    // 主机下电只在失败时返回，退到浅睡，至少外设已停、按键可唤醒。
    // Host power-down returns only on failure; fall back to light sleep with peripherals parked and the key as wake.
    if (mode == APP_SLEEP_OFF || mode == APP_SLEEP_DEEP) {
        arm_ambient_alarm_before_sleep(mode);
        ESP_LOGW(TAG, "host sleep failed: %s, light sleep", esp_err_to_name(app_enter_host_sleep(mode)));
    }
    epd_poweroff();
    app_light_sleep_wait(acc);

    // 参考帧和屏幕都归零，回到主循环后由当前页自己画一遍，不必知道是哪一页。
    // Zero the reference frame and the panel; the current page redraws after the loop resumes, without knowing which page it is.
    read_pico_pmu_drain_events();
    epd_poweron();
    epd_clear();
    epd_hl_set_all_white(hl);
    read_pico_pmu_drain_events();
    if (ignore_until_ms) {
        *ignore_until_ms = esp_timer_get_time() / 1000 + APP_LOCK_IGNORE_BOOT_MS;
    }
    ESP_LOGI(TAG, "unlocked");
}
