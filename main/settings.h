/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * NVS 里的用户设置：睡眠与字体、图书刷新/排版/方向/手势，以及看板选项。
 *
 * User settings in NVS: sleep/font controls, book refresh/layout/orientation/gesture, and dashboard options.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/// 默认深睡（PMU 软睡）：短按开机。浅睡：按键回原页；拿起唤醒默认关。关机：EN=0，长按开机。
/// Default is deep sleep (PMU soft sleep): short-press to boot. Light sleep returns to the page; pickup-wake defaults off. Power-off requires a long press to boot.
typedef enum {
    APP_SLEEP_LIGHT = 0,
    APP_SLEEP_DEEP = 1,
    APP_SLEEP_OFF = 2,
} app_sleep_mode_t;

/// 锁屏内容：默认图、TF 卡摘录、TF 卡图片。/ Lock content: built-in art, SD excerpt, or SD image.
typedef enum {
    APP_SAVER_BUILTIN = 0,
    APP_SAVER_QUOTE = 1,
    APP_SAVER_IMAGE = 2,
} app_saver_style_t;

typedef enum {
    APP_DASH_TIME = 1u << 0,
    APP_DASH_BATTERY = 1u << 1,
    APP_DASH_LAST_BOOK = 1u << 2,
    APP_DASH_NOTE = 1u << 3,
} app_dashboard_item_t;

typedef struct {
    bool enabled; ///< 自动刷新开关 / Scheduled refresh switch
    uint16_t interval_min; ///< 刷新周期分钟数 / Refresh interval in minutes
    uint8_t quiet_start_h; ///< 免打扰开始小时 / Quiet-period start hour
    uint8_t quiet_end_h; ///< 免打扰结束小时 / Quiet-period end hour
    uint8_t source_type; ///< 内容来源类型 0/1/2 / Content source type 0/1/2
    char url[257]; ///< HTTP URL，最多 256 字节 / HTTP URL, up to 256 bytes
    char auth_header[257]; ///< Authorization 请求头值，不可回显 / Authorization header value; never echo
} app_ambient_settings_t;

void app_settings_init(void);
app_sleep_mode_t app_settings_sleep_mode(void);
void app_settings_set_sleep_mode(app_sleep_mode_t mode);
const char* app_sleep_mode_name(app_sleep_mode_t mode);
/// 无操作后自动锁屏秒数；0 表示关闭，默认 120 秒。
/// Idle time before auto-lock, in seconds; 0 disables it, default 120 s.
uint16_t app_settings_auto_sleep_sec(void);
/// 保存自动锁屏秒数；仅接受睡眠页提供的选项。
/// Save auto-lock seconds; accepts only options shown on the Sleep page.
void app_settings_set_auto_sleep_sec(uint16_t seconds);
/// 空路径表示固件内建字体；非空为 SD 上的 TTF。/ Empty path is the built-in font; non-empty is a TTF on the SD card.
const char* app_settings_font_path(void);
void app_settings_set_font_path(const char* path);
/// 上次浅睡唤醒源（app_wake_source_t），掉电也保留。/ Last light-sleep wake source (app_wake_source_t); kept across power loss.
uint8_t app_settings_last_wake(void);
void app_settings_set_last_wake(uint8_t src);
/// 最近一次非 0 的 PMU wake_reason。STATUS 报 0 时用这个回显。/ Last non-zero PMU wake_reason. Used when STATUS reports 0.
uint8_t app_settings_last_boot(void);
void app_settings_set_last_boot(uint8_t reason);
/// 浅睡拿起唤醒。默认关；有加速度计也不会自动开。/ Light-sleep pickup wake. Defaults off; an accelerometer does not turn it on.
bool app_settings_pickup_wake(void);
void app_settings_set_pickup_wake(bool on);
/// 阅读默认字号，36..72、步长 4，默认 48。/ Default reading size, 36..72 in steps of 4, initially 48.
uint8_t app_settings_book_px(void);
/// 无效字号恢复 48。/ Invalid sizes fall back to 48.
void app_settings_set_book_px(uint8_t px);
/// 实验性晃动翻页，默认关闭。/ Experimental shake page turn, off by default.
bool app_settings_book_shake(void);
/// 保存实验性晃动翻页开关。/ Persist the experimental shake page-turn switch.
void app_settings_set_book_shake(bool on);
/// 图书翻页刷新档位：0 纯净、1 均衡、2 极速；默认均衡。/ Book turn refresh profile: clean, balanced, or fast; default balanced.
uint8_t app_settings_book_refresh(void);
/// 保存图书翻页刷新档位。/ Persist the book page-turn refresh profile.
void app_settings_set_book_refresh(uint8_t profile);
/// 行距档位：0 紧凑、1 标准、2 宽松；默认标准。/ Line spacing: compact, standard, or relaxed; default standard.
uint8_t app_settings_book_line_height(void);
/// 保存行距档位。/ Persist line-height preset.
void app_settings_set_book_line_height(uint8_t preset);
/// 左右边距档位：0 窄、1 标准、2 宽；默认标准。/ Side margins: narrow, standard, or wide; default standard.
uint8_t app_settings_book_margin(void);
/// 保存左右边距档位。/ Persist side-margin preset.
void app_settings_set_book_margin(uint8_t preset);
/// 字重设置；0 表示沿用字体默认值，其他值为 300..800。/ Weight; 0 uses the font default, otherwise 300..800.
uint16_t app_settings_book_weight(void);
/// 0 恢复字体默认字重。/ 0 restores the font's default weight.
void app_settings_set_book_weight(uint16_t weight);
/// 阅读方向：0 竖屏、1 横屏、2 自动。/ Reader orientation: portrait, landscape, or automatic.
uint8_t app_settings_book_rotation(void);
/// 保存阅读方向。/ Persist reader orientation.
void app_settings_set_book_rotation(uint8_t rotation);
/// 翻页手势：0 关闭、1 晃动、2 轻敲。/ Page gesture: off, shake, or tap.
uint8_t app_settings_book_gesture(void);
/// 保存翻页手势档位。/ Persist page-turn gesture preset.
void app_settings_set_book_gesture(uint8_t gesture);
app_saver_style_t app_settings_saver_style(void);
void app_settings_set_saver_style(app_saver_style_t style);
uint8_t app_settings_dashboard_items(void);
void app_settings_set_dashboard_items(uint8_t items);
/// 全局时区偏移，单位分钟，范围 UTC-12..UTC+14，默认 UTC+8。/ Global timezone offset in minutes, UTC-12..UTC+14, default UTC+8.
int16_t app_settings_timezone_min(void);
void app_settings_set_timezone_min(int16_t minutes);
bool app_settings_charge_led(void);
/// 保存并下发充电指示灯开关。/ Persist and send the charge LED switch.
void app_settings_set_charge_led(bool enabled);
uint8_t app_settings_low_soc_notify_percent(void);
/// 保存并下发低电提醒百分比，0 关闭，范围 5..30。/ Persist and send low-battery notification percent; 0 disables, 5..30 is accepted.
void app_settings_set_low_soc_notify_percent(uint8_t percent);
bool app_settings_muted(void);
/// 保存并应用全局蜂鸣器静音。/ Persist and apply global buzzer mute.
void app_settings_set_muted(bool muted);
/// 局域网传输是否强制要求 6 位配对码；默认关闭以兼容旧网页。
/// / Whether LAN transfer enforces the 6-digit PIN; defaults false for legacy webpage compatibility.
bool app_settings_transfer_pin_required(void);
/// 保存局域网传输配对码开关。/ Persist the LAN transfer PIN requirement switch.
void app_settings_set_transfer_pin_required(bool required);
/// 开机同步低电阈值与充电灯到 PMU。/ Sync the low-battery threshold and charge LED to the PMU at boot.
void app_settings_sync_pmu(void);
/// 仅保存时区到 NVS，由主循环调用 app_settings_apply_runtime 应用。
/// / Persist timezone to NVS only; apply later on the main task via app_settings_apply_runtime.
void app_settings_set_timezone_min_deferred(int16_t minutes);
/// 仅保存充电指示灯到 NVS，由主循环调用 app_settings_apply_runtime 同步 PMU。
/// / Persist charge LED to NVS only; sync PMU later on the main task via app_settings_apply_runtime.
void app_settings_set_charge_led_deferred(bool enabled);
/// 仅保存低电提醒到 NVS，由主循环调用 app_settings_apply_runtime 同步 PMU。
/// / Persist low-battery alert to NVS only; sync PMU later on the main task via app_settings_apply_runtime.
void app_settings_set_low_soc_notify_percent_deferred(uint8_t percent);
/// 在主循环应用挂起的时区、蜂鸣器静音与 PMU 配置。
/// / Apply pending timezone, buzzer mute and PMU settings on the main task.
void app_settings_apply_runtime(void);
/// 读取独立 rp_ambient 命名空间配置。/ Read configuration from the separate rp_ambient namespace.
void app_settings_ambient_get(app_ambient_settings_t* out);
/// 校验并保存定时看板配置。/ Validate and save scheduled dashboard configuration.
bool app_settings_ambient_set(const app_ambient_settings_t* config);
/// 记录最近一次自动刷新结果与时间。/ Record the latest scheduled refresh result and time.
void app_settings_ambient_result(bool success, uint32_t unix_seconds);
/// 读取最近一次自动刷新结果。/ Read the latest scheduled refresh result.
void app_settings_ambient_result_get(bool* has_result, bool* success, uint32_t* unix_seconds);
