/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：五步交互式初始使用引导（OOBE）：首次开机自动进入，也可从「设置 → 设备与诊断」随时重看；
 * 涵盖时区/时间校准、屏下三键与说明带、顶部下滑快捷控制中心、无线传书与外置字库、睡眠与锁屏配置。
 *
 * English: Five-step interactive onboarding guide (OOBE): runs automatically on first boot and
 * can be reopened anytime from Settings -> Device & Diagnostics; covers timezone/clock setup,
 * three-key & hint-strip navigation, top pull-down Quick Control Menu, Wi-Fi/BLE transfer &
 * external CJK fonts, and sleep/saver configuration.
 *
 * 冻结：引导页不占用九宫格桌面槽位；首次完成或跳过后写入 NVS onboard=1 并返回来源页（首次开机回桌面）；
 * render 保持纯绘制，所有设置变更仅在手势/按键回调内通过公开接口提交。
 * Frozen: Onboarding takes no launcher grid slot; finishing or skipping persists onboard=1 in NVS
 * and returns to the caller (Home on first boot); render only paints, and settings are updated
 * solely through public APIs inside input callbacks.
 */

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "app_book.h"
#include "app_registry.h"
#include "ble_service.h"
#include "read_pico_pmu.h"
#include "settings.h"
#include "time_sync.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_icons.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include "ui_time_edit.h"

extern const app_desc_t app_transfer;

#define ONBOARD_STEPS 5
#define ONBOARD_SKIP_W 128
#define ONBOARD_SKIP_H 50
#define ONBOARD_PROGRESS_Y 164

enum {
    OB_CTRL_NONE = -1,
    OB_CTRL_SKIP = 0,
    OB_CTRL_BAR_0 = 1,
    OB_CTRL_BAR_1 = 2,
    OB_CTRL_BAR_2 = 3,
    OB_CTRL_S0_TZ_DEC = 10,
    OB_CTRL_S0_TZ_INC = 11,
    OB_CTRL_S0_TIME_EDIT = 12,
    OB_CTRL_S0_BLE = 13,
    OB_CTRL_S0_MUTE = 14,
    OB_CTRL_S0_PX = 15,
    OB_CTRL_S2_TRY_QM = 20,
    OB_CTRL_S4_SLEEP_MODE = 30,
    OB_CTRL_S4_AUTO_SLEEP = 31,
    OB_CTRL_S4_SAVER = 32,
    OB_CTRL_S4_OPEN_BOOK = 33,
};

static bool s_time_edit;
static ui_time_edit_t s_time_editor;
static int s_pressed = OB_CTRL_NONE;
static EpdRect s_dirty_area;
static char s_notice[96];

static const uint16_t k_ob_timeouts[] = { 120, 300, 600, 1800, 0 };

static int clamp_step(int leaf) {
    if (leaf < 0) return 0;
    if (leaf >= ONBOARD_STEPS) return ONBOARD_STEPS - 1;
    return leaf;
}

static void fmt_tz(char *buf, size_t n, int tz_min) {
    char sign = tz_min < 0 ? '-' : '+';
    int abs_tz = tz_min < 0 ? -tz_min : tz_min;
    snprintf(buf, n, "UTC%c%d:%02d", sign, abs_tz / 60, abs_tz % 60);
}

static const char *sleep_mode_zh(app_sleep_mode_t mode) {
    switch (mode) {
        case APP_SLEEP_LIGHT: return "浅睡（按键秒醒）";
        case APP_SLEEP_OFF: return "关机（长按开机）";
        case APP_SLEEP_DEEP:
        default: return "深睡（省电推荐）";
    }
}

static const char *saver_zh(app_saver_style_t style) {
    switch (style) {
        case APP_SAVER_QUOTE: return "最近摘录";
        case APP_SAVER_IMAGE: return "TF 卡图片";
        case APP_SAVER_BOOK_COVER: return "当前书封面";
        case APP_SAVER_HEATMAP: return "阅读热力图";
        case APP_SAVER_BUILTIN:
        default: return "默认图案";
    }
}

static void fmt_timeout_zh(char *buf, size_t n, uint16_t sec) {
    if (sec == 0) snprintf(buf, n, "关闭");
    else if (sec < 60) snprintf(buf, n, "%u 秒", (unsigned)sec);
    else snprintf(buf, n, "%u 分钟", (unsigned)(sec / 60));
}

static EpdRect skip_btn_rect(void) {
    return (EpdRect){
        .x = UI_LOCK_WIDTH - UI_MARGIN - ONBOARD_SKIP_W,
        .y = 66,
        .width = ONBOARD_SKIP_W,
        .height = ONBOARD_SKIP_H,
    };
}

static void draw_btn(uint8_t *fb, EpdRect r, const char *label, bool primary, bool pressed) {
    if (pressed) {
        ui_draw_pressed_round_rect(fb, r, UI_BTN_RADIUS);
    } else if (primary) {
        ui_draw_selected_round_rect(fb, r, UI_BTN_RADIUS);
    } else {
        ui_draw_round_rect(fb, r, UI_BTN_RADIUS, UI_GRAY_BLACK);
    }
    ui_text_vc(fb, r.x + r.width / 2, r.y + r.height / 2, UI_PX_BTN,
               label, EPD_DRAW_ALIGN_CENTER, false);
}

static void draw_kv_ctrl(
    uint8_t *fb, EpdRect r, const char *title, const char *val, bool pressed
) {
    if (pressed) {
        ui_draw_pressed_round_rect(fb, r, UI_BTN_RADIUS);
    } else {
        ui_draw_round_rect(fb, r, UI_BTN_RADIUS, UI_GRAY_BLACK);
    }
    const int cy = r.y + r.height / 2;
    ui_text_vc(fb, r.x + 20, cy, UI_PX_BTN, title, EPD_DRAW_ALIGN_LEFT, false);
    if (val && val[0]) {
        ui_text_vc(fb, r.x + r.width - 20, cy, UI_PX_CAPTION, val, EPD_DRAW_ALIGN_RIGHT, false);
    }
}

static void draw_step_frame(uint8_t *fb, int step, const char *title, const char *sub) {
    ui_header_skel_t skel = ui_header_skel_box(title, sub, ONBOARD_SKIP_W, ONBOARD_SKIP_H);
    ui_draw_header_skel(fb, &skel, title, sub);

    EpdRect skip = skip_btn_rect();
    if (s_pressed == OB_CTRL_SKIP) {
        ui_draw_pressed_round_rect(fb, skip, 12);
    } else {
        ui_draw_round_rect(fb, skip, 12, UI_GRAY_BLACK);
    }
    ui_text_vc(fb, skip.x + skip.width / 2, skip.y + skip.height / 2, 26,
               "跳过 ›", EPD_DRAW_ALIGN_CENTER, false);

    // 五段式引导进度条。/ 5-segment onboarding progress bar.
    const int gap = 10;
    const int seg_w = (ui_content_width() - (ONBOARD_STEPS - 1) * gap) / ONBOARD_STEPS;
    for (int i = 0; i < ONBOARD_STEPS; ++i) {
        EpdRect seg = {
            .x = UI_MARGIN + i * (seg_w + gap),
            .y = ONBOARD_PROGRESS_Y,
            .width = seg_w,
            .height = 8,
        };
        if (i <= step) {
            ui_fill_round_rect(fb, seg, 4, UI_GRAY_BLACK);
        } else {
            ui_draw_round_rect(fb, seg, 4, UI_GRAY_LIGHT);
        }
    }
}

static void render_step0_welcome(uint8_t *fb) {
    draw_step_frame(fb, 0, "欢迎使用小纸 Pico", "第 1 / 5 步 · 基础时间与偏好设置");

    EpdRect banner = { .x = UI_MARGIN, .y = 192, .width = ui_content_width(), .height = 148 };
    ui_draw_round_rect(fb, banner, 16, UI_GRAY_BLACK);
    ui_draw_icon(fb, banner.x + 56, banner.y + 74, 64, UI_ICON_BOOK);
    ui_text(fb, banner.x + 108, banner.y + 24, 34,
            "4.7 英寸墨水屏 · Pico OS", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, banner.x + 108, banner.y + 72, 26,
            "本地阅读 · 无线传书 · 极低功耗待机", EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, banner.x + 108, banner.y + 106, 26,
            "用一分钟了解核心交互并完成初始设置", EPD_DRAW_ALIGN_LEFT, false);

    int y = 360;
    struct tm local = { 0 };
    int tz_min = app_settings_timezone_min();
    char time_val[112], tz_val[24];
    fmt_tz(tz_val, sizeof(tz_val), tz_min);
    if (app_time_now_local(&local, &tz_min)) {
        snprintf(time_val, sizeof(time_val), "%04d-%02d-%02d %02d:%02d (%s)",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 local.tm_hour, local.tm_min, tz_val);
    } else {
        snprintf(time_val, sizeof(time_val), "尚未校准 (%s)", tz_val);
    }
    y = ui_draw_row(fb, y, "当前时钟", time_val);

    const int btn_y = y + 16;
    draw_btn(fb, ui_row_rect(0, 2, btn_y, UI_BTN_H), "时区 -30分", false,
             s_pressed == OB_CTRL_S0_TZ_DEC);
    draw_btn(fb, ui_row_rect(1, 2, btn_y, UI_BTN_H), "时区 +30分", false,
             s_pressed == OB_CTRL_S0_TZ_INC);

    draw_kv_ctrl(fb, ui_row_rect(0, 1, btn_y + (UI_BTN_H + UI_GAP), UI_BTN_H),
                 "手动校准日期与时间", "打开 ›", s_pressed == OB_CTRL_S0_TIME_EDIT);

    char ble_lbl[40], mute_lbl[40];
    snprintf(ble_lbl, sizeof(ble_lbl), "蓝牙伴侣：%s",
             app_settings_ble_enabled() ? "开启" : "关闭");
    snprintf(mute_lbl, sizeof(mute_lbl), "提示音效：%s",
             app_settings_muted() ? "静音" : "开启");
    const int r2_y = btn_y + 2 * (UI_BTN_H + UI_GAP);
    draw_btn(fb, ui_row_rect(0, 2, r2_y, UI_BTN_H), ble_lbl,
             app_settings_ble_enabled(), s_pressed == OB_CTRL_S0_BLE);
    draw_btn(fb, ui_row_rect(1, 2, r2_y, UI_BTN_H), mute_lbl,
             !app_settings_muted(), s_pressed == OB_CTRL_S0_MUTE);

    char px_val[32];
    snprintf(px_val, sizeof(px_val), "%u px（点按切换）", (unsigned)app_settings_book_px());
    draw_kv_ctrl(fb, ui_row_rect(0, 1, btn_y + 3 * (UI_BTN_H + UI_GAP), UI_BTN_H),
                 "默认阅读字号", px_val, s_pressed == OB_CTRL_S0_PX);

    if (s_notice[0]) {
        ui_text(fb, UI_MARGIN, btn_y + 4 * (UI_BTN_H + UI_GAP) + 8, 26,
                s_notice, EPD_DRAW_ALIGN_LEFT, false);
    }

    draw_btn(fb, ui_bar_rect(0, 2), "跳过引导", false, s_pressed == OB_CTRL_BAR_0);
    draw_btn(fb, ui_bar_rect(1, 2), "下一步 ›", true, s_pressed == OB_CTRL_BAR_1);
}

static void render_step1_keys(uint8_t *fb) {
    draw_step_frame(fb, 1, "屏下三键与说明带", "第 2 / 5 步 · 全系统统一按键导航");

    // 绘制机身下半部示意图（底部说明带 + 屏下三颗电容键）。
    // Draw lower-bezel diagram (bottom hint strip + 3 capacitive keys below panel).
    EpdRect diag = { .x = UI_MARGIN, .y = 196, .width = ui_content_width(), .height = 252 };
    ui_draw_round_rect(fb, diag, 18, UI_GRAY_BLACK);
    EpdRect strip = { .x = diag.x + 28, .y = diag.y + 30, .width = diag.width - 56, .height = 54 };
    ui_draw_round_rect(fb, strip, 8, UI_GRAY_BLACK);
    const int col_w = strip.width / 3;
    ui_text_vc(fb, strip.x + col_w / 2, strip.y + 27, 24, "返回 / 上页", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, strip.x + col_w + col_w / 2, strip.y + 27, 24, "刷新 · 长按桌面", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, strip.x + col_w * 2 + col_w / 2, strip.y + 27, 24, "下一页", EPD_DRAW_ALIGN_CENTER, false);

    ui_text_vc(fb, diag.x + diag.width / 2, diag.y + 114, 24,
               "↑ 屏幕底部说明带可直接点按或长按 ↑", EPD_DRAW_ALIGN_CENTER, false);

    const char *key_names[3] = { "左键 KEY1", "中键 KEY2", "右键 KEY3" };
    for (int i = 0; i < 3; ++i) {
        const int cx = strip.x + i * col_w + col_w / 2;
        epd_draw_circle(cx, diag.y + 168, 20, UI_GRAY_BLACK, fb);
        epd_fill_circle(cx, diag.y + 168, 6, UI_GRAY_BLACK, fb);
        ui_text_vc(fb, cx, diag.y + 216, 26, key_names[i], EPD_DRAW_ALIGN_CENTER, false);
    }

    int y = 472;
    y = ui_draw_section(fb, y, "三键操作规则");
    ui_text(fb, UI_MARGIN, y, 32, "· 左键 KEY1：短按返回上级或翻上页；长按 500ms 跨页直返上级。",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 60, 32, "· 中键 KEY2：短按整屏 GC16 清残影；长按 500ms 一键回桌面首页。",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 120, 32, "· 右键 KEY3：在桌面、列表与阅读正文内翻到下一页。",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 190, 28, "提示：屏幕外下边框内藏三颗电容触摸键，轻触圆点位置即可响应。",
            EPD_DRAW_ALIGN_LEFT, false);

    draw_btn(fb, ui_bar_rect(0, 2), "‹ 上一步", false, s_pressed == OB_CTRL_BAR_0);
    draw_btn(fb, ui_bar_rect(1, 2), "下一步 ›", true, s_pressed == OB_CTRL_BAR_1);
}

static void render_step2_gestures(uint8_t *fb) {
    draw_step_frame(fb, 2, "下拉菜单与触控手势", "第 3 / 5 步 · 顶部控制中心与阅读交互");

    EpdRect diag = { .x = UI_MARGIN, .y = 196, .width = ui_content_width(), .height = 236 };
    ui_draw_round_rect(fb, diag, 18, UI_GRAY_BLACK);
    ui_fill_round_rect(fb, (EpdRect){
        .x = diag.x + diag.width / 2 - 44, .y = diag.y + 14, .width = 88, .height = 8
    }, 4, UI_GRAY_BLACK);
    ui_text_vc(fb, diag.x + diag.width / 2, diag.y + 56, 30,
               "↓ 从屏幕最顶边向下拉动呼出控制中心 ↓", EPD_DRAW_ALIGN_CENTER, false);
    ui_hairline(fb, diag.y + 92, diag.x + 24, diag.width - 48, UI_GRAY_LIGHT);
    ui_text_vc(fb, diag.x + 130, diag.y + 158, 28, "← 右滑上页", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, diag.x + diag.width / 2, diag.y + 158, 28, "◎ 点中区菜单", EPD_DRAW_ALIGN_CENTER, false);
    ui_text_vc(fb, diag.x + diag.width - 130, diag.y + 158, 28, "左滑下页 →", EPD_DRAW_ALIGN_CENTER, false);

    int y = 456;
    y = ui_draw_section(fb, y, "手势速览");
    ui_text(fb, UI_MARGIN, y, 32, "· 顶部下滑：不退出当前书本或网页，随时调字号、静音、蓝牙与强刷。",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 60, 32, "· 桌面顶栏：在桌面直接点按顶部时间/电量状态栏也可打开控制中心。",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 120, 32, "· 阅读正文：点屏幕中央开关阅读工具栏；长按正文句子可高亮摘录。",
            EPD_DRAW_ALIGN_LEFT, false);

    EpdRect try_btn = ui_row_rect(0, 1, 880, UI_BTN_H);
    draw_btn(fb, try_btn, "立即体验：呼出快捷控制中心", true, s_pressed == OB_CTRL_S2_TRY_QM);
    ui_text_vc(fb, UI_LOCK_WIDTH / 2, 1004, 26,
               "也可现在直接从屏幕顶部边缘向下滑动试一试", EPD_DRAW_ALIGN_CENTER, false);

    draw_btn(fb, ui_bar_rect(0, 2), "‹ 上一步", false, s_pressed == OB_CTRL_BAR_0);
    draw_btn(fb, ui_bar_rect(1, 2), "下一步 ›", true, s_pressed == OB_CTRL_BAR_1);
}

static void render_step3_content(uint8_t *fb) {
    draw_step_frame(fb, 3, "传书、伴侣与字库", "第 4 / 5 步 · 导入电子书与完整中文字体");

    EpdRect card = { .x = UI_MARGIN, .y = 196, .width = ui_content_width(), .height = 152 };
    ui_draw_round_rect(fb, card, 16, UI_GRAY_BLACK);
    const int third = card.width / 3;
    ui_draw_icon(fb, card.x + third / 2, card.y + 56, 56, UI_ICON_TRANSFER);
    ui_text_vc(fb, card.x + third / 2, card.y + 118, 26, "Wi-Fi 扫码传书", EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_icon(fb, card.x + third + third / 2, card.y + 56, 56, UI_ICON_BOOK);
    ui_text_vc(fb, card.x + third + third / 2, card.y + 118, 26, "EPUB/TXT/MD/CBZ", EPD_DRAW_ALIGN_CENTER, false);
    ui_draw_icon(fb, card.x + third * 2 + third / 2, card.y + 56, 56, UI_ICON_FONT);
    ui_text_vc(fb, card.x + third * 2 + third / 2, card.y + 118, 26, "TF 卡完整字库", EPD_DRAW_ALIGN_CENTER, false);

    int y = 372;
    y = ui_draw_section(fb, y, "内容与生态说明");
    ui_text(fb, UI_MARGIN, y, 30, "1. 无线传书：打开桌面「传书」，连热点或局域网即可浏览器上传图书、",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 46, 30, "   配置 OPDS/RSS 订阅书库、网页收藏夹与 MQTT Agent 收件。",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 108, 30, "2. 蓝牙伴侣：iOS 配套应用搜索 ReadPico-XXXX，免连 Wi-Fi 即可对时、",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 154, 30, "   配网、同步看板备忘与远程开启局域网传书。",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 216, 30, "3. 外置中文字体（重要）：内置字库仅覆盖系统界面与演示文稿；阅读外部",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 262, 30, "   中文书请把完整 .ttf 字体放入 TF 卡 fonts/ 目录或在传书页上传，",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, y + 308, 30, "   随后在「字体管理」中切换，避免生僻字缺字。",
            EPD_DRAW_ALIGN_LEFT, false);

    draw_btn(fb, ui_bar_rect(0, 2), "‹ 上一步", false, s_pressed == OB_CTRL_BAR_0);
    draw_btn(fb, ui_bar_rect(1, 2), "下一步 ›", true, s_pressed == OB_CTRL_BAR_1);
}

static void render_step4_finish(uint8_t *fb) {
    draw_step_frame(fb, 4, "睡眠屏保与准备就绪", "第 5 / 5 步 · 电源键与锁屏个性化");

    ui_text(fb, UI_MARGIN, 196, 28,
            "短按机身侧边电源键即可锁屏休眠；深睡模式下再次短按电源键即可快速开机。",
            EPD_DRAW_ALIGN_LEFT, false);
    ui_text(fb, UI_MARGIN, 240, 28,
            "仅在设为「关机」完全断电后，才需要长按侧边电源键开机。",
            EPD_DRAW_ALIGN_LEFT, false);

    const int y0 = 308;
    draw_kv_ctrl(fb, ui_row_rect(0, 1, y0, UI_BTN_H),
                 "锁屏休眠方式", sleep_mode_zh(app_settings_sleep_mode()),
                 s_pressed == OB_CTRL_S4_SLEEP_MODE);

    char to_buf[32];
    fmt_timeout_zh(to_buf, sizeof(to_buf), app_settings_auto_sleep_sec());
    draw_kv_ctrl(fb, ui_row_rect(0, 1, y0 + (UI_BTN_H + UI_GAP), UI_BTN_H),
                 "自动锁屏时长", to_buf, s_pressed == OB_CTRL_S4_AUTO_SLEEP);

    draw_kv_ctrl(fb, ui_row_rect(0, 1, y0 + 2 * (UI_BTN_H + UI_GAP), UI_BTN_H),
                 "锁屏壁纸样式", saver_zh(app_settings_saver_style()),
                 s_pressed == OB_CTRL_S4_SAVER);

    draw_kv_ctrl(fb, ui_row_rect(0, 1, y0 + 3 * (UI_BTN_H + UI_GAP), UI_BTN_H),
                 "完成后直接打开图书书架", "进入图书 ›", s_pressed == OB_CTRL_S4_OPEN_BOOK);

    ui_text(fb, UI_MARGIN, 736, 26,
            "日后可随时在「设置 → 设备与诊断 → 新手使用引导」重新打开本指南。",
            EPD_DRAW_ALIGN_LEFT, false);

    draw_btn(fb, ui_bar_rect(0, 3), "‹ 上一步", false, s_pressed == OB_CTRL_BAR_0);
    draw_btn(fb, ui_bar_rect(1, 3), "无线传书", false, s_pressed == OB_CTRL_BAR_1);
    draw_btn(fb, ui_bar_rect(2, 3), "开始使用", true, s_pressed == OB_CTRL_BAR_2);
}

static int hit_control(int step, uint16_t x, uint16_t y, EpdRect *out_rect) {
    if (s_time_edit) {
        return ui_time_edit_hit_rect(x, y, out_rect) ? OB_CTRL_S0_TIME_EDIT : OB_CTRL_NONE;
    }
    EpdRect skip = skip_btn_rect();
    if (ui_rect_hit(skip, x, y)) {
        if (out_rect) *out_rect = skip;
        return OB_CTRL_SKIP;
    }

    if (step == 0) {
        const int btn_y = 360 + UI_ROW_H + 16;
        const int r2_y = btn_y + 2 * (UI_BTN_H + UI_GAP);
        const struct { int id; EpdRect r; } items[] = {
            { OB_CTRL_S0_TZ_DEC, ui_row_rect(0, 2, btn_y, UI_BTN_H) },
            { OB_CTRL_S0_TZ_INC, ui_row_rect(1, 2, btn_y, UI_BTN_H) },
            { OB_CTRL_S0_TIME_EDIT, ui_row_rect(0, 1, btn_y + (UI_BTN_H + UI_GAP), UI_BTN_H) },
            { OB_CTRL_S0_BLE, ui_row_rect(0, 2, r2_y, UI_BTN_H) },
            { OB_CTRL_S0_MUTE, ui_row_rect(1, 2, r2_y, UI_BTN_H) },
            { OB_CTRL_S0_PX, ui_row_rect(0, 1, btn_y + 3 * (UI_BTN_H + UI_GAP), UI_BTN_H) },
            { OB_CTRL_BAR_0, ui_bar_rect(0, 2) },
            { OB_CTRL_BAR_1, ui_bar_rect(1, 2) },
        };
        for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
            if (ui_rect_hit(items[i].r, x, y)) {
                if (out_rect) *out_rect = items[i].r;
                return items[i].id;
            }
        }
        return OB_CTRL_NONE;
    }

    if (step == 1 || step == 3) {
        for (int i = 0; i < 2; ++i) {
            EpdRect r = ui_bar_rect(i, 2);
            if (ui_rect_hit(r, x, y)) {
                if (out_rect) *out_rect = r;
                return OB_CTRL_BAR_0 + i;
            }
        }
        return OB_CTRL_NONE;
    }

    if (step == 2) {
        EpdRect try_r = ui_row_rect(0, 1, 880, UI_BTN_H);
        if (ui_rect_hit(try_r, x, y)) {
            if (out_rect) *out_rect = try_r;
            return OB_CTRL_S2_TRY_QM;
        }
        for (int i = 0; i < 2; ++i) {
            EpdRect r = ui_bar_rect(i, 2);
            if (ui_rect_hit(r, x, y)) {
                if (out_rect) *out_rect = r;
                return OB_CTRL_BAR_0 + i;
            }
        }
        return OB_CTRL_NONE;
    }

    if (step == 4) {
        const int y0 = 308;
        const struct { int id; EpdRect r; } items[] = {
            { OB_CTRL_S4_SLEEP_MODE, ui_row_rect(0, 1, y0, UI_BTN_H) },
            { OB_CTRL_S4_AUTO_SLEEP, ui_row_rect(0, 1, y0 + (UI_BTN_H + UI_GAP), UI_BTN_H) },
            { OB_CTRL_S4_SAVER, ui_row_rect(0, 1, y0 + 2 * (UI_BTN_H + UI_GAP), UI_BTN_H) },
            { OB_CTRL_S4_OPEN_BOOK, ui_row_rect(0, 1, y0 + 3 * (UI_BTN_H + UI_GAP), UI_BTN_H) },
            { OB_CTRL_BAR_0, ui_bar_rect(0, 3) },
            { OB_CTRL_BAR_1, ui_bar_rect(1, 3) },
            { OB_CTRL_BAR_2, ui_bar_rect(2, 3) },
        };
        for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
            if (ui_rect_hit(items[i].r, x, y)) {
                if (out_rect) *out_rect = items[i].r;
                return items[i].id;
            }
        }
    }
    return OB_CTRL_NONE;
}

static void finish_onboarding(app_ctx_t *ctx, const app_desc_t *jump_target) {
    app_settings_set_onboarding_done(true);
    if (jump_target != NULL) {
        ctx->request_app = jump_target;
    } else {
        ctx->request_return = true;
    }
}

static void on_enter(app_ctx_t *ctx) {
    ctx->leaf = 0;
    s_time_edit = false;
    s_pressed = OB_CTRL_NONE;
    s_dirty_area = (EpdRect){ 0 };
    s_notice[0] = '\0';
    if (read_pico_pmu_ready()) {
        (void)read_pico_pmu_refresh();
    }
    app_time_sync_from_pmu();
}

static void render(app_ctx_t *ctx, uint8_t *fb) {
    if (!ttf_font_ready()) {
        ui_draw_no_font_page(fb, false, false);
        return;
    }
    if (s_time_edit) {
        ui_time_edit_render(&s_time_editor, fb);
        return;
    }
    ui_clear_page(fb);
    switch (clamp_step(ctx->leaf)) {
        case 0: render_step0_welcome(fb); break;
        case 1: render_step1_keys(fb); break;
        case 2: render_step2_gestures(fb); break;
        case 3: render_step3_content(fb); break;
        case 4:
        default: render_step4_finish(fb); break;
    }
}

static app_redraw_t commit_control(app_ctx_t *ctx, int step, int ctrl, uint16_t x, uint16_t y) {
    if (s_time_edit) {
        ui_time_edit_result_t res = ui_time_edit_tap(&s_time_editor, x, y);
        if (res == UI_TIME_EDIT_SAVED) {
            snprintf(s_notice, sizeof(s_notice), "%s",
                     s_time_editor.pmu_synced ? "时间已保存并同步主板 RTC" : "时间已保存至主机");
            s_time_edit = false;
            return APP_REDRAW_FULL;
        }
        if (res == UI_TIME_EDIT_CANCELLED) {
            s_time_edit = false;
            return APP_REDRAW_FULL;
        }
        return res == UI_TIME_EDIT_CHANGED ? APP_REDRAW_PAGE : APP_REDRAW_NONE;
    }

    if (ctrl == OB_CTRL_SKIP) {
        finish_onboarding(ctx, NULL);
        return APP_REDRAW_NONE;
    }

    if (step == 0) {
        if (ctrl == OB_CTRL_S0_TZ_DEC || ctrl == OB_CTRL_S0_TZ_INC) {
            int next = app_settings_timezone_min() + (ctrl == OB_CTRL_S0_TZ_DEC ? -30 : 30);
            if (next < -720) next = 840;
            if (next > 840) next = -720;
            app_settings_set_timezone_min((int16_t)next);
            s_notice[0] = '\0';
            return APP_REDRAW_PAGE;
        }
        if (ctrl == OB_CTRL_S0_TIME_EDIT) {
            ui_time_edit_open(&s_time_editor);
            s_time_edit = true;
            return APP_REDRAW_FULL;
        }
        if (ctrl == OB_CTRL_S0_BLE) {
            app_ble_set_enabled(!app_settings_ble_enabled());
            return APP_REDRAW_PAGE;
        }
        if (ctrl == OB_CTRL_S0_MUTE) {
            app_settings_set_muted(!app_settings_muted());
            return APP_REDRAW_PAGE;
        }
        if (ctrl == OB_CTRL_S0_PX) {
            uint8_t px = app_settings_book_px();
            app_settings_set_book_px(px >= 72 ? 36 : (uint8_t)(px + 4));
            return APP_REDRAW_PAGE;
        }
        if (ctrl == OB_CTRL_BAR_0) {
            finish_onboarding(ctx, NULL);
            return APP_REDRAW_NONE;
        }
        if (ctrl == OB_CTRL_BAR_1) {
            ctx->leaf = 1;
            s_notice[0] = '\0';
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }

    if (step >= 1 && step <= 3) {
        if (step == 2 && ctrl == OB_CTRL_S2_TRY_QM) {
            ctx->request_quick_menu = true;
            return APP_REDRAW_NONE;
        }
        if (ctrl == OB_CTRL_BAR_0) {
            ctx->leaf = step - 1;
            return APP_REDRAW_PAGE;
        }
        if (ctrl == OB_CTRL_BAR_1) {
            ctx->leaf = step + 1;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }

    if (step == 4) {
        if (ctrl == OB_CTRL_S4_SLEEP_MODE) {
            app_settings_set_sleep_mode((app_sleep_mode_t)((app_settings_sleep_mode() + 1) % 3));
            return APP_REDRAW_PAGE;
        }
        if (ctrl == OB_CTRL_S4_AUTO_SLEEP) {
            uint16_t cur = app_settings_auto_sleep_sec();
            size_t count = sizeof(k_ob_timeouts) / sizeof(k_ob_timeouts[0]);
            size_t idx = 0;
            while (idx + 1 < count && k_ob_timeouts[idx] != cur) ++idx;
            app_settings_set_auto_sleep_sec(k_ob_timeouts[(idx + 1) % count]);
            return APP_REDRAW_PAGE;
        }
        if (ctrl == OB_CTRL_S4_SAVER) {
            app_settings_set_saver_style((app_saver_style_t)((app_settings_saver_style() + 1) % 5));
            return APP_REDRAW_PAGE;
        }
        if (ctrl == OB_CTRL_S4_OPEN_BOOK) {
            finish_onboarding(ctx, &app_book);
            return APP_REDRAW_NONE;
        }
        if (ctrl == OB_CTRL_BAR_0) {
            ctx->leaf = 3;
            return APP_REDRAW_PAGE;
        }
        if (ctrl == OB_CTRL_BAR_1) {
            finish_onboarding(ctx, &app_transfer);
            return APP_REDRAW_NONE;
        }
        if (ctrl == OB_CTRL_BAR_2) {
            finish_onboarding(ctx, NULL);
            return APP_REDRAW_NONE;
        }
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_gesture(app_ctx_t *ctx, const ui_gesture_event_t *ev) {
    if (!ev) return APP_REDRAW_NONE;
    const int step = clamp_step(ctx->leaf);

    if (!s_time_edit) {
        if (ev->type == UI_GESTURE_SWIPE_L) {
            s_pressed = OB_CTRL_NONE;
            if (step + 1 < ONBOARD_STEPS) {
                ctx->leaf = step + 1;
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
        if (ev->type == UI_GESTURE_SWIPE_R) {
            s_pressed = OB_CTRL_NONE;
            if (step > 0) {
                ctx->leaf = step - 1;
                return APP_REDRAW_PAGE;
            }
            return APP_REDRAW_NONE;
        }
    }

    if (ev->type == UI_GESTURE_PRESS) {
        EpdRect rect = { 0 };
        int hit = hit_control(step, ev->x0, ev->y0, &rect);
        s_pressed = hit;
        if (hit != OB_CTRL_NONE && !s_time_edit && ctx->fb) {
            s_dirty_area = rect;
            render(ctx, ctx->fb);
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }

    if (ev->type == UI_GESTURE_CANCEL) {
        if (s_pressed == OB_CTRL_NONE) return APP_REDRAW_NONE;
        s_pressed = OB_CTRL_NONE;
        if (!s_time_edit && ctx->fb && s_dirty_area.width > 0) {
            render(ctx, ctx->fb);
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }

    if (ev->type != UI_GESTURE_TAP) return APP_REDRAW_NONE;

    const int prev = s_pressed;
    s_pressed = OB_CTRL_NONE;
    EpdRect r0 = { 0 }, r1 = { 0 };
    const int h0 = hit_control(step, ev->x0, ev->y0, &r0);
    const int h1 = hit_control(step, ev->x, ev->y, &r1);
    if (h0 == OB_CTRL_NONE || h0 != h1) {
        if (prev != OB_CTRL_NONE && !s_time_edit && ctx->fb && s_dirty_area.width > 0) {
            render(ctx, ctx->fb);
            return APP_REDRAW_AREA;
        }
        return APP_REDRAW_NONE;
    }
    return commit_control(ctx, step, h1, ev->x, ev->y);
}

static app_redraw_t on_key(app_ctx_t *ctx, int key) {
    s_pressed = OB_CTRL_NONE;
    if (s_time_edit) {
        if (key == UI_KEY_1) {
            ctx->key_handled = true;
            s_time_edit = false;
            return APP_REDRAW_FULL;
        }
        return APP_REDRAW_NONE;
    }
    const int step = clamp_step(ctx->leaf);
    if (key == UI_KEY_1) {
        ctx->key_handled = true;
        if (step > 0) {
            ctx->leaf = step - 1;
            return APP_REDRAW_PAGE;
        }
        finish_onboarding(ctx, NULL);
        return APP_REDRAW_NONE;
    }
    if (key == UI_KEY_3) {
        ctx->key_handled = true;
        if (step + 1 < ONBOARD_STEPS) {
            ctx->leaf = step + 1;
            return APP_REDRAW_PAGE;
        }
        finish_onboarding(ctx, NULL);
        return APP_REDRAW_NONE;
    }
    return APP_REDRAW_NONE;
}

static app_redraw_t on_key_long(app_ctx_t *ctx, int key) {
    if (key == UI_KEY_1) {
        ctx->key_handled = true;
        s_pressed = OB_CTRL_NONE;
        if (s_time_edit) {
            s_time_edit = false;
            return APP_REDRAW_FULL;
        }
        finish_onboarding(ctx, NULL);
    }
    return APP_REDRAW_NONE;
}

static EpdRect area_hint(app_ctx_t *ctx) {
    (void)ctx;
    return s_dirty_area;
}

static void key_hints(app_ctx_t *ctx, ui_key_hints_t *out) {
    if (s_time_edit) {
        out->left = "取消";
        out->right = NULL;
        return;
    }
    const int step = clamp_step(ctx->leaf);
    out->left = step > 0 ? "上一步" : "跳过";
    out->right = (step + 1 < ONBOARD_STEPS) ? "下一步" : "完成";
}

const app_desc_t app_onboarding = {
    .title = "使用引导",
    .detail = "小纸 Pico 新手快速上手指南",
    .on_enter = on_enter,
    .render = render,
    .on_gesture = on_gesture,
    .on_key = on_key,
    .on_key_long = on_key_long,
    .key_hints = key_hints,
    .area_hint = area_hint,
};
