/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：全系统顶部边缘下滑快捷控制菜单（控制中心）覆盖层：提供实时时钟/电量/存储状态、
 * 蓝牙/静音/充电灯/拿起唤醒/自动锁屏/屏保快捷开关、阅读字号/刷新/状态栏/方向即时调节，
 * 以及强刷、锁屏、无线传书与桌面/图书直达动作。
 *
 * English: System-wide top-edge pull-down Quick Control Menu overlay: live clock/battery/storage
 * status, BLE/mute/LED/pickup/auto-sleep/saver toggles, live reader font-size/refresh/HUD/rotation
 * controls, and full-refresh, lock, Wi-Fi transfer, and Home/Books quick actions.
 *
 * 冻结：打开与收起不调用底层页面的 on_exit/on_enter，保持阅读进度、网络连接与画布不断开；
 * render 保持纯绘制（无 I2C/NVS 写与蜂鸣）；收起时若修改过阅读参数则同步当前活动阅读页并整屏 GC16 恢复。
 * Frozen: Opening and closing never call the underlying page's on_exit/on_enter so active books,
 * network sessions, and canvases stay alive; render only paints; closing syncs modified reader
 * settings and restores the underlying page with full-screen GC16.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app.h"
#include "ui_gesture.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 顶部边缘下滑触发带高度（覆盖 58px 桌面顶栏与 54px 阅读顶栏，不侵入 60px+ 页眉附件与内容区）。
/// Top-edge pull-down trigger zone height (covers 58px home bar and 54px reader bar without hitting y>=60 controls).
#define UI_QUICK_PULL_TOP_Y 58

/// 快捷控制卡片几何边界（供绘制与测试共用）。/ Quick control card bounds shared by draw and tests.
#define UI_QM_CARD_X 20
#define UI_QM_CARD_Y 16
#define UI_QM_CARD_W 644
#define UI_QM_CARD_H 900
#define UI_QM_CARD_BOTTOM (UI_QM_CARD_Y + UI_QM_CARD_H)

typedef struct {
    bool open;          ///< 当前是否展开 / Whether the overlay is open
    int pressed_ctrl;   ///< 当前按下的控件编号，-1 表示无 / Pressed control index, -1 if none
    bool px_changed;    ///< 本次展开期间是否调过默认字号 / Whether book_px changed while open
    bool book_dirty;    ///< 本次展开期间是否改过阅读相关项 / Whether any reader setting changed
    EpdRect dirty_area; ///< 按下态局部 DU 刷新矩形 / Dirty rect for local DU press feedback
} ui_quick_menu_t;

/// 清零状态机。/ Reset state machine.
void ui_quick_menu_reset(ui_quick_menu_t *qm);

/// 打开快捷控制菜单并刷新一次只读硬件快照（不在 render 内做 I2C 写/轮询）。
/// Open the Quick Control Menu and refresh read-only hardware snapshots once outside render().
void ui_quick_menu_open(ui_quick_menu_t *qm);

/// 收起快捷控制菜单，并在需要时把阅读设置变更同步给当前活动页。
/// Close the Quick Control Menu and sync any reader setting changes into the active page.
void ui_quick_menu_close(ui_quick_menu_t *qm, app_ctx_t *ctx, const app_desc_t *current);

/// 纯绘制快捷控制菜单覆盖层（不含底部三键说明带，说明带由主循环统一绘制）。
/// Pure render of the Quick Control Menu overlay (excluding the bottom key-hint strip).
void ui_quick_menu_render(
    const ui_quick_menu_t *qm, app_ctx_t *ctx, const app_desc_t *current, uint8_t *fb
);

/// 填充快捷控制菜单展开时的底部三键说明带文案。
/// Populate bottom three-key hint labels while the Quick Control Menu is open.
void ui_quick_menu_key_hints(ui_key_hints_t *out);

/// 处理展开期间的触控手势；需要收起菜单时置 *out_close = true。
/// Handle touch gestures while open; sets *out_close = true when the overlay should dismiss.
app_redraw_t ui_quick_menu_on_gesture(
    ui_quick_menu_t *qm, app_ctx_t *ctx, const app_desc_t *current,
    const ui_gesture_event_t *event, bool *out_close
);

/// 处理展开期间的短按 KEY1/KEY3；需要收起菜单时置 *out_close = true。
/// Handle short KEY1/KEY3 presses while open; sets *out_close = true when dismissing.
app_redraw_t ui_quick_menu_on_key(
    ui_quick_menu_t *qm, app_ctx_t *ctx, const app_desc_t *current,
    int key, bool *out_close
);

#ifdef __cplusplus
}
#endif
