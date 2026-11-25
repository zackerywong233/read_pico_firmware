/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 固件应用页的统一接口。每个页面是一个 app_desc_t，放在 main/apps/ 下自己的文件里，
 * 绘制、命中、轮询和页内状态都封在那一个文件中；app_loop.c 只按这套回调调度，
 * 加一个页面不必再改主循环。
 *
 * Shared firmware app contract. Each page is an app_desc_t in its own main/apps/ file.
 * Draw, hit-test, poll and page state stay there. app_loop.c only dispatches
 * these callbacks; adding a page does not touch the loop.
 *
 * 冻结：手势回调替代旧触摸回调；取消全屏应用列表（ui_menu）与底栏菜单方框箭头；
 * 屏下三键遵循全系统统一契约：
 * 短按 KEY2 整屏 GC16 强刷；长按 KEY2（500ms）全局返回桌面第 1 页（桌面首页无动作）；
 * 短按 KEY1/KEY3 优先派发当前页 on_key，未消费时 KEY1 弹栈返回上一级，KEY3 无动作；
 * 长按 KEY1/KEY3（500ms）优先派发当前页 on_key_long，未消费时 KEY1 跨页直接返回上一级，KEY3 无动作；
 * 底部统一按键说明带（y=1170..1214）由主循环按 key_hints 渲染，纯全屏态可隐藏；
 * 非隐藏态下点按/长按说明带非空格与屏下三键共用同一套短/长按路由，消除页面重复触控导航栏。
 * Frozen: Gesture callbacks replace legacy touch callbacks. Full-screen app list (ui_menu)
 * and the bottom-right menu handle are removed. Physical keys follow one OS-wide contract:
 * short KEY2 full GC16 refresh; 500ms hold KEY2 returns to Home page 1 (no-op on Home page 1);
 * short KEY1/KEY3 dispatch to on_key first, unhandled KEY1 pops nav stack back, KEY3 no-ops;
 * 500ms hold KEY1/KEY3 dispatch to on_key_long first, unhandled KEY1 returns directly to parent;
 * bottom key-hint strip (y=1170..1214) is drawn from key_hints unless hidden in fullscreen;
 * when visible, tapping or holding a non-empty hint cell routes through the same KEY1/2/3 path
 * as the capacitive keys to eliminate duplicate in-page navigation buttons.
 * 冻结：主循环维护 8 层导航栈；普通切页入栈（回桌面清空栈），返回弹栈恢复上一页与页码。
 * Frozen: The loop keeps an 8-level nav stack; switching pushes (going Home clears), Return pops page and leaf.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cst836u.h"
#include "epd_highlevel.h"
#include "epdiy.h"
#include "app_config.h"
#include "sc7a20h_lab.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 回调返回值告诉主循环该怎么把 framebuffer 推上屏。
/// / What the loop should do with the framebuffer.
typedef enum {
    /// 什么都没变，不刷屏。/ Nothing changed.
    APP_REDRAW_NONE = 0,
    /// 只有 area_hint() 那块变了，使用 APP_DYNAMIC_REFRESH_MODE。回调必须自己先画好 fb。
    /// / Push area_hint() with APP_DYNAMIC_REFRESH_MODE. Caller paints fb first.
    APP_REDRAW_AREA,
    /// 由主循环调 render() 重画整页，使用 APP_PAGE_REFRESH_MODE。
    /// / Loop calls render() and presents with APP_PAGE_REFRESH_MODE.
    APP_REDRAW_PAGE,
    /// 同上；APP_PAGE_FORCE_FULL 置 1 时升为整屏 GC16，否则仍走 APP_PAGE_REFRESH_MODE。
    /// / Same; full-screen GC16 when APP_PAGE_FORCE_FULL is 1.
    APP_REDRAW_FULL,
    /// 回调自己已经刷完屏了（自检、刷新测试这类要分步显示进度的页）。
    /// / Page already presented (self-test, refresh bench, …).
    APP_REDRAW_DONE,
} app_redraw_t;

typedef struct app_desc_s app_desc_t;
typedef struct ui_gesture_event_s ui_gesture_event_t;

/// 底部三键说明带文案；由主循环在 render() 后统一绘制。
/// Bottom three-key hint labels; drawn by the loop after render().
typedef struct {
    const char* left;   ///< 左键说明（NULL/"" 留空） / Left key label (NULL/"" = blank)
    const char* center; ///< 中键说明（NULL 时填充默认） / Center key label (NULL = default)
    const char* right;  ///< 右键说明（NULL/"" 留空） / Right key label (NULL/"" = blank)
    bool hidden;        ///< true = 纯全屏态不绘制说明带 / true = hide strip in fullscreen
} ui_key_hints_t;

typedef struct {
    EpdiyHighlevelState* hl;
    uint8_t* fb;
    sc7a20h_handle_t acc;
    cst836u_handle_t tp;
    bool sensor_ready;
    bool continuous_ready;
    int64_t now_ms;
    /// 去抖后的触摸快照：空槽已清零，抬手后保留最后一次坐标。
    /// / Debounced touch: empty slots cleared; last coords kept after lift.
    const cst836u_touch_t* touch;
    const cst836u_info_t* touch_info;
    /// 本轮是不是按下沿 / 抬起沿，触摸页的跟随节流要用。
    /// / Press/release edge this tick. Touch tracking uses this to throttle.
    bool pressed;
    bool released;
    /// 分页页（字体列表）当前停在第几页，由页内按钮修改。
    /// / Leaf page index (font list), changed by in-page buttons.
    int leaf;
    /// 本轮已经被别的分支处理掉了，页内的跟随逻辑要跳过这一轮。
    /// / Another branch already handled this tick; skip in-page tracking.
    bool consumed;
    /// on_key 明确消费了本次按键（例如阅读首/末页边界无动作，避免退回全局返回或菜单）。
    /// Set by on_key when it owns the key even on a no-op boundary (e.g. first/last reading page).
    bool key_handled;
    /// on_tick 请求本轮保持唤醒（例如后台 OTA 下载中），重置空闲休眠计时。
    /// Set by on_tick to keep the device awake this tick (e.g. active OTA), resetting idle sleep.
    bool keep_awake;
    /// 页面想跳到别的页时填这里，主循环会在本轮末尾切过去；跳回桌面会自动清空导航栈。
    /// / Request a page switch; applied at tick end. Switching to Home clears the nav stack.
    const struct app_desc_s* request_app;
    /// 返回导航栈上一级（恢复原页与页码）；栈空时回桌面首页；优先于其它请求。
    /// Pop the navigation stack (restoring page and leaf); falls back to Home when empty. Overrides other requests.
    bool request_return;
    /// 本轮末对当前页执行整屏 GC16 强刷（不切页时生效）。
    /// Request a full GC16 redraw of the current page at tick end (when no navigation is pending).
    bool request_redraw;
    /// 本轮末走全局锁屏路径（页面未 holds_pmu 且无切页请求时生效）。
    /// Request the global lock/sleep flow at tick end (ignored when holds_pmu or navigation is pending).
    bool request_lock;
} app_ctx_t;

struct app_desc_s {
    /// 桌面九宫格与页面使用的标题与副标题。/ Grid title and subtitle.
    const char* title;
    const char* detail;
    /// true = 这一页会长时间独占 PMU，主循环的锁屏按键轮询要让路（设备功能自检）。
    /// / true = page owns the PMU; lock-key poll yields (device self-test).
    bool holds_pmu;
    /// 历史标记（阅读类页面标识）；当前所有页面均可直接通过 on_key/on_key_long 接收 KEY1 与 KEY3。
    /// Legacy marker (reading-class page flag); all pages may handle KEY1 and KEY3 via on_key/on_key_long.
    bool owns_keys;
    /// true = 停留在本页时不计空闲自动休眠。电源键锁屏仍走全局路径。
    /// true = idle time does not auto-sleep while this page is current. The power key still locks.
    bool blocks_idle_sleep;
    /// 进页/离页。on_enter 里做上电、唤醒传感器、拉一次数据这类副作用。
    /// / Enter/exit. on_enter powers up, wakes sensors, takes a first sample.
    /// 锁屏走软睡/关机前也调 on_exit；下电失败回来再调 on_enter。
    /// on_exit also runs before a soft-sleep/off lock; on_enter runs again if power-down fails.
    void (*on_enter)(app_ctx_t* ctx);
    void (*on_exit)(app_ctx_t* ctx);
    /// TF 挂载失效：停止并等待后台任务、关闭文件；主循环随后回退字体并重绘。
    /// On TF mount loss, join background work and close files before font fallback/redraw.
    void (*on_media_lost)(app_ctx_t* ctx);
    /// 纯绘制，不做 I2C 写、蜂鸣这类副作用，整屏强刷才能安全复用它。
    /// / Paint only. No I2C writes or buzzer. KEY2 full redraw reuses this.
    void (*render)(app_ctx_t* ctx, uint8_t* fb);
    /// 自定义推屏。返回 true 表示页面已经刷过。
    /// / Custom present. Return true if the page updated the display.
    bool (*present)(app_ctx_t* ctx, app_redraw_t redraw);
    /// 设置后独占触摸派发，替代 on_touch。/ Exclusive gesture dispatch replaces on_touch.
    app_redraw_t (*on_gesture)(app_ctx_t* ctx, const ui_gesture_event_t* event);
    app_redraw_t (*on_touch)(app_ctx_t* ctx, const cst836u_touch_t* touch);
    /// 短按回调（抬起提交）：接收 UI_KEY_1 与 UI_KEY_3；未消费时 KEY1 走导航返回，KEY3 无动作。
    /// Short-key callback (on release): receives UI_KEY_1 and UI_KEY_3; unhandled KEY1 returns, KEY3 no-ops.
    app_redraw_t (*on_key)(app_ctx_t* ctx, int key);
    /// 长按回调（同键保持 500ms 触发一次且不再触发短按）：接收 UI_KEY_1 与 UI_KEY_3；
    /// 未消费时 KEY1 跨页直接返回上一级，KEY3 无动作；KEY2 长按始终由主循环执行回桌面首页。
    /// Long-key callback (fires once at 500ms hold, suppresses short press): receives UI_KEY_1 and UI_KEY_3;
    /// unhandled KEY1 returns to parent directly, KEY3 no-ops; long KEY2 always returns to Home page 1.
    app_redraw_t (*on_key_long)(app_ctx_t* ctx, int key);
    /// 可选：提供底部三键说明带文案（或置 hidden=true 隐藏）；未提供时主循环使用默认“返回 | 刷新 · 长按桌面 |”。
    /// Optional: supply bottom key-hint labels (or hidden=true); default uses Back | Refresh · Hold Home |.
    void (*key_hints)(app_ctx_t* ctx, ui_key_hints_t* out);
    /// 每轮都调，页面自己判断到不到刷新间隔。
    /// / Called every tick; the page decides if its interval has elapsed.
    app_redraw_t (*on_tick)(app_ctx_t* ctx);
    /// APP_REDRAW_AREA 要刷的区域。/ Area for APP_REDRAW_AREA.
    EpdRect (*area_hint)(app_ctx_t* ctx);
    /// 同一份实现挂多个 app 时用来区分（加速度计的读数页与诊断页共享绘制代码）。
    /// / Distinguishes views that share one implementation (IMU live vs lab).
    void* user;
};

// 锁屏默认软睡，侧键短按开机；浅睡按键或拿起回原页；显式关机后需长按开机。
// Default lock uses soft sleep and a short power-key press to boot; light sleep returns on key or pickup; explicit power-off needs a long press.
// 无操作达到睡眠页设置的时长后走同一条锁屏路径；0 表示关闭自动锁屏。
// After the Sleep page's idle interval, use the same lock path; 0 disables auto-lock.
#define APP_LOCK_POLL_MS 200
#define APP_LOCK_IGNORE_BOOT_MS 2000
#define APP_LOCK_IGNORE_SELFTEST_MS 2000

/// 按 redraw 把 framebuffer 推上屏，顺带做 pclk 欠载兜底。APP_REDRAW_DONE 直接返回。
/// / Present fb for redraw, with a pclk-underrun fallback. APP_REDRAW_DONE returns.
void app_present(app_ctx_t* ctx, const app_desc_t* app, app_redraw_t redraw);
/// 之后 ms 毫秒内忽略电源键短按，避免开机或自检的残留事件立刻把屏锁掉。
/// / Ignore short power-key presses for ms so boot/self-test leftovers do not lock.
void app_lock_ignore_for(int64_t ms);

#ifdef __cplusplus
}
#endif
