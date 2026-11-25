/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 事件循环。只做和页面无关的事：读触摸并去抖、三个实体键短/长按路由、8 层导航栈、
 * 切页时调 on_exit/on_enter、按回调返回值刷屏与绘制底部按键说明带、电源键与空闲锁屏、
 * SD 卡字体延迟加载与拔卡防护。页面状态和刷新节奏留在 main/apps/ 各自文件里。
 *
 * Event loop for page-agnostic work: touch debounce, short/long three-key routing,
 * 8-level nav stack, on_exit/on_enter, present with bottom key-hint strip, power-key
 * and idle lock, delayed SD font load and card-loss guard.
 *
 * 冻结：主循环只产出手势、不解释含义；取消全屏列表菜单与右下角方框箭头。
 * 屏下三键遵循全系统统一契约：
 * 短按 KEY2 整屏 GC16 强刷；长按 KEY2（500ms）全局回桌面第 1 页（桌面首页无动作）；
 * 短按 KEY1/KEY3 优先交页面 on_key，未消费时 KEY1 弹栈返回，KEY3 无动作；
 * 长按 KEY1/KEY3（500ms）优先交页面 on_key_long，未消费时 KEY1 跨页直接返回，KEY3 无动作；
 * 短按抬起提交，长按触发一次后不再触发短按，滑出/多点/读错/锁屏/切页取消；
 * 底部说明带（y=1168..1215）未隐藏时，点按/长按非空格共用同一套 KEY1/2/3 路由，空位拦截不透传。
 * Frozen: Produce gestures without interpreting them; full-screen menu list and handle are removed.
 * Physical keys follow one OS-wide contract: short KEY2 full GC16 refresh; 500ms hold KEY2 returns
 * to Home page 1 (no-op on Home page 1); short KEY1/KEY3 dispatch to on_key (unhandled KEY1 pops
 * nav stack, KEY3 no-ops); 500ms hold KEY1/KEY3 dispatch to on_key_long (unhandled KEY1 returns
 * directly to parent). Short press commits on release; holds suppress short press; slide-off,
 * multitouch, errors, lock and navigation cancel tracking. When the bottom key-hint strip
 * (y=1168..1215) is visible, tapping/holding a non-empty cell shares the KEY1/2/3 path and empty
 * cells absorb touch without leaking to the page.
 * 冻结：主循环维护 8 层导航栈；普通切页入栈（回桌面清空栈），返回弹栈恢复上一页与页码。
 * Frozen: Keep an 8-level nav stack; ordinary switches push (Home clears), Return pops page and leaf.
 */

#include "app_loop.h"

#include <string.h>

#include "app_registry.h"
#include "continuous_du.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "read_pico_board.h"
#include "read_pico_pmu.h"
#include "read_pico_sd.h"
#include "settings.h"
#include "sleep.h"
#include "ttf_font.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"

#define TAG "app_loop"

// 触摸芯片报点率远高于屏幕，主循环空转一轮只等这么久。
// Touch reports far faster than the panel; idle this long per loop.
#define LOOP_TICK_MS 5
// SD 卡插上或者刚挂好时，隔一会儿再试一次存在设置里的字体。
// Retry the saved SD font shortly after a card appears or mounts.
#define FONT_RETRY_INTERVAL_MS 3000
#define MEDIA_POLL_INTERVAL_MS 500

#define APP_NAV_STACK_MAX 8

typedef struct {
    const app_desc_t* app;
    int leaf;
} app_nav_entry_t;

static int64_t s_lock_ignore_until_ms;

void app_lock_ignore_for(int64_t ms) {
    s_lock_ignore_until_ms = esp_timer_get_time() / 1000 + ms;
}

static ui_key_hints_t resolve_page_key_hints(app_ctx_t* ctx, const app_desc_t* app) {
    const bool at_home_first = (app == app_home_page() && ctx->leaf == 0);
    ui_key_hints_t hints = {
        .left = (app == app_home_page()) ? NULL : "返回",
        .center = at_home_first ? "刷新" : "刷新 · 长按桌面",
        .right = NULL,
        .hidden = false,
    };
    if (app != NULL && app->key_hints != NULL) {
        app->key_hints(ctx, &hints);
        if (!hints.hidden && hints.center == NULL) {
            hints.center = at_home_first ? "刷新" : "刷新 · 长按桌面";
        }
    }
    return hints;
}

static void draw_page_key_hints(app_ctx_t* ctx, const app_desc_t* app) {
    if (ctx == NULL || ctx->fb == NULL || app == NULL) return;
    ui_key_hints_t hints = resolve_page_key_hints(ctx, app);
    if (!hints.hidden) {
        ui_draw_key_hints(ctx->fb, &hints);
    }
}

// 统一命中屏下实体三键与底部可见说明带；非隐藏说明带空位标记 in_hint_strip 以吞掉误触。
// Unified hit-test for physical keys and visible bottom key-hint cells; flags in_hint_strip so empty cells absorb taps.
static int hit_nav_key(
    app_ctx_t* ctx, const app_desc_t* app, uint16_t x, uint16_t y, bool* in_hint_strip
) {
    if (in_hint_strip != NULL) *in_hint_strip = false;
    int key = ui_key_hit_test(x, y);
    if (key >= 0) return key;
    if (y < UI_KEY_HINT_RULE_Y || y >= UI_LOCK_HEIGHT) return -1;
    ui_key_hints_t hints = resolve_page_key_hints(ctx, app);
    if (hints.hidden) return -1;
    if (in_hint_strip != NULL) *in_hint_strip = true;
    return ui_key_hint_hit_test(x, y, &hints);
}

void app_present(app_ctx_t* ctx, const app_desc_t* app, app_redraw_t redraw) {
    enum EpdDrawError result = EPD_DRAW_SUCCESS;
    if (app->present != NULL && app->present(ctx, redraw)) return;
    switch (redraw) {
        case APP_REDRAW_NONE:
        case APP_REDRAW_DONE:
            return;
        case APP_REDRAW_AREA: {
            // 回调已经画好了 fb，这里只负责把它那一块推上屏。
            // The callback already painted fb; push that rectangle only.
            EpdRect area = app->area_hint != NULL
                ? app->area_hint(ctx)
                : ui_content_refresh_area();
            result = update_display_area_with(
                ctx->hl, &E0470_WAVEFORM, APP_DYNAMIC_REFRESH_MODE, area
            );
            break;
        }
        case APP_REDRAW_FULL:
            if (app->render != NULL) {
                app->render(ctx, ctx->fb);
                draw_page_key_hints(ctx, app);
            }
            result = APP_PAGE_FORCE_FULL
                ? update_display_full(ctx->hl)
                : update_display_mode(ctx->hl, APP_PAGE_REFRESH_MODE);
            break;
        case APP_REDRAW_PAGE:
        default:
            if (app->render != NULL) {
                app->render(ctx, ctx->fb);
                draw_page_key_hints(ctx, app);
            }
            result = update_display_mode(ctx->hl, APP_PAGE_REFRESH_MODE);
            break;
    }
    guard_draw_result(ctx->hl, result);
}

// 中断边界先清页面装饰再清识别器。/ Clear decoration before resetting recognition.
static void cancel_gesture(app_ctx_t* ctx, const app_desc_t* app, ui_gesture_t* gesture) {
    if (gesture->active && app->on_gesture) {
        ui_gesture_event_t event = { .type = UI_GESTURE_CANCEL,
            .x0 = gesture->x0, .y0 = gesture->y0, .x = gesture->x, .y = gesture->y };
        const app_desc_t* requested = ctx->request_app;
        bool return_requested = ctx->request_return;
        app_redraw_t redraw = app->on_gesture(ctx, &event);
        ctx->request_app = requested;
        ctx->request_return = return_requested;
        ui_gesture_reset(gesture);
        app_present(ctx, app, redraw);
    }
    ui_gesture_reset(gesture);
}

static void present_page(app_ctx_t* ctx, const app_desc_t* app,
                         ui_gesture_t* gesture, app_redraw_t redraw) {
    if (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL)
        cancel_gesture(ctx, app, gesture);
    app_present(ctx, app, redraw);
}

// 切页：先让上一页收尾，再给新页一次 on_enter，最后整页画出来。
// Leave the old page, enter the new one, then present a full page.
static void switch_to(
    app_ctx_t* ctx, const app_desc_t** current, const app_desc_t* next
) {
    if (next == NULL || next == *current) return;
    if ((*current)->on_exit != NULL) (*current)->on_exit(ctx);
    *current = next;
    ctx->leaf = 0;
    if (next->on_enter != NULL) next->on_enter(ctx);
    app_present(ctx, next, APP_REDRAW_FULL);
    ESP_LOGI(TAG, "page -> %s", next->title);
}

// 统一全屏 GC16 强刷。/ Unified full-screen GC16 refresh.
static void full_refresh_current(app_ctx_t* ctx, const app_desc_t* current) {
    if (current->present != NULL && current->present(ctx, APP_REDRAW_FULL)) {
        return;
    }
    if (current->render != NULL) {
        current->render(ctx, ctx->fb);
        draw_page_key_hints(ctx, current);
        guard_draw_result(ctx->hl, update_display_full(ctx->hl));
    }
}

// 空槽常带着抬起事件或残留坐标，不能进页面看到的快照；抬手后保留最后一次位置。
// Empty slots often carry lift events or stale coords; keep last position on lift.
static void debounce_touch(
    cst836u_touch_t* latest, const cst836u_touch_t* raw, bool released
) {
    if (raw->touched) {
        *latest = *raw;
        for (int i = 0; i < CST836U_MAX_POINTS; i++) {
            if (!latest->points[i].active) {
                memset(&latest->points[i], 0, sizeof(latest->points[i]));
            }
        }
    } else if (released) {
        latest->touched = false;
        latest->count = 0;
        for (int i = 0; i < CST836U_MAX_POINTS; i++) {
            latest->points[i].active = false;
        }
    }
    memcpy(latest->raw, raw->raw, sizeof(raw->raw));
}

static bool try_load_saved_sd_font(bool allow_probe) {
    const char* path = app_settings_font_path();
    if (ttf_font_path_is_builtin(path)) return false;
    if (ttf_font_ready() && !ttf_font_is_builtin()
        && strcmp(ttf_font_path(), path) == 0) {
        return false;
    }

    read_pico_sd_info_t info;
    esp_err_t err = read_pico_sd_get_info(&info);
    if (err == ESP_ERR_NOT_FINISHED) return false;
    if (!info.mounted) {
        if (allow_probe && err == ESP_ERR_INVALID_STATE) read_pico_sd_start_probe();
        return false;
    }
    return ttf_font_open(path) == ESP_OK && !ttf_font_is_builtin();
}

// 先停文件使用者，再换字体；此处只观察挂载，不卸载也不重新挂载。
// Stop file consumers before font fallback; observe mounting without unmounting or remounting.
static bool poll_media(app_ctx_t* ctx, const app_desc_t* current,
                       bool* mounted, bool* invalidated) {
    read_pico_sd_info_t info = {0};
    esp_err_t err = read_pico_sd_get_info(&info);
    if (err == ESP_ERR_NOT_FINISHED) return false;
    bool ready = err == ESP_OK && info.mounted;
    bool lost = *mounted && !ready;
    *mounted = ready;
    if (ready) *invalidated = false;
    if (!lost) return false;
    *invalidated = true;
    if (current->on_media_lost) current->on_media_lost(ctx);
    if (ttf_font_ready() && !ttf_font_is_builtin()) ttf_font_open_builtin();
    return true;
}

// 电源键和空闲超时共用：收掉手势，走锁屏路径，浅睡回来后重画。
// Shared by the power key and the idle timeout: cancel input, lock, redraw after light sleep.
static void lock_and_resume(
    app_ctx_t* ctx, const app_desc_t* current, ui_gesture_t* gesture,
    int* held_key, bool* held_long_fired,
    bool* media_mounted, bool* media_invalidated,
    int64_t* last_media_poll_ms
) {
    *held_key = -1;
    *held_long_fired = false;
    cancel_gesture(ctx, current, gesture);
    // 要断主机电时先让页面收尾（停网络、关传感器、存进度）；只有下电失败才会回到这里重新进页。
    // Before the host loses power let the page wind down (network, sensors, progress); re-enter only if power-down failed.
    const bool powers_down = app_lock_powers_down();
    if (powers_down && current->on_exit != NULL) current->on_exit(ctx);
    enter_lock_and_sleep(ctx->hl, &s_lock_ignore_until_ms, ctx->acc);
    if (powers_down && current->on_enter != NULL) current->on_enter(ctx);
    ctx->now_ms = esp_timer_get_time() / 1000;
    poll_media(ctx, current, media_mounted, media_invalidated);
    *last_media_poll_ms = ctx->now_ms;
    ctx->consumed = true;
    ctx->pressed = false;
    ctx->released = false;
    app_present(ctx, current, APP_REDRAW_PAGE);
}

static void nav_push(app_nav_entry_t* stack, int* depth, const app_desc_t* app, int leaf) {
    if (app == NULL) return;
    if (*depth >= APP_NAV_STACK_MAX) {
        memmove(&stack[0], &stack[1], (APP_NAV_STACK_MAX - 1) * sizeof(stack[0]));
        *depth = APP_NAV_STACK_MAX - 1;
    }
    stack[(*depth)++] = (app_nav_entry_t){ .app = app, .leaf = leaf };
}

void app_loop_run(const app_loop_config_t* config) {
    const app_desc_t* current = config->first_app != NULL
        ? config->first_app
        : app_home_page();

    cst836u_info_t touch_info = { 0 };
    cst836u_get_info(config->tp, &touch_info);
    cst836u_touch_t latest = { 0 };

    app_ctx_t ctx = {
        .hl = config->hl,
        .fb = config->fb,
        .acc = config->acc,
        .tp = config->tp,
        .sensor_ready = config->sensor_ready,
        .continuous_ready = continuous_du_init(),
        .touch = &latest,
        .touch_info = &touch_info,
    };

    bool was_touched = false;
    ui_gesture_t gesture = {0};
    app_nav_entry_t nav_stack[APP_NAV_STACK_MAX] = {0};
    int nav_depth = 0;
    int held_key = -1;
    bool held_long_fired = false;
    int64_t held_since_ms = 0;
    int64_t last_lock_poll_ms = 0;
    int64_t last_input_ms = esp_timer_get_time() / 1000;
    int64_t last_font_retry_ms = 0;
    int64_t last_media_poll_ms = 0;
    bool media_mounted = ttf_font_ready() && !ttf_font_is_builtin();
    bool media_invalidated = false;
    app_lock_ignore_for(APP_LOCK_IGNORE_BOOT_MS);

    if (current->on_enter != NULL) current->on_enter(&ctx);
    poll_media(&ctx, current, &media_mounted, &media_invalidated);
    // 首帧必须整屏 GC16：fb 里还是 app_main 画的开机图，DU 盖不掉。
    // First frame must be full GC16: fb still holds the splash; DU cannot cover it.
    app_present(&ctx, current, APP_REDRAW_FULL);
    ESP_LOGI(TAG, "UI ready on %s", current->title);

    while (true) {
        cst836u_touch_t touch = { 0 };
        esp_err_t err = cst836u_read(config->tp, &touch);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CST836U read failed: %s", esp_err_to_name(err));
            memset(&touch, 0, sizeof(touch));
        }

        const bool pressed = touch.touched && !was_touched;
        const bool released = !touch.touched && was_touched;
        debounce_touch(&latest, &touch, released);
        ctx.now_ms = esp_timer_get_time() / 1000;
        // 触摸、独占 PMU、以及声明不自动休眠的页都算活动。页面自己的轮询刷新不重置。
        // Touch, PMU-owning pages, and pages that block idle sleep count as activity. A page's own poll does not reset it.
        if (current->holds_pmu || current->blocks_idle_sleep
            || (err == ESP_OK && (touch.touched || released))) {
            last_input_ms = ctx.now_ms;
        }
        ctx.pressed = pressed;
        ctx.released = released;
        ctx.consumed = false;
        ctx.key_handled = false;
        ctx.keep_awake = false;
        ctx.request_app = NULL;
        ctx.request_return = false;
        ctx.request_redraw = false;
        ctx.request_lock = false;

        if (ctx.now_ms - last_media_poll_ms >= MEDIA_POLL_INTERVAL_MS) {
            last_media_poll_ms = ctx.now_ms;
            if (poll_media(&ctx, current, &media_mounted, &media_invalidated)) {
                held_key = -1;
                held_long_fired = false;
                cancel_gesture(&ctx, current, &gesture);
                ctx.consumed = true;
                ctx.pressed = ctx.released = false;
                was_touched = touch.touched;
                app_present(&ctx, current, APP_REDRAW_FULL);
                // 本轮旧输入不再触发动作，下一轮恢复正常事件处理。
                // Discard this tick's stale input, then resume normal dispatch next tick.
                vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
                continue;
            }
        }

        if (err != ESP_OK) {
            held_key = -1;
            held_long_fired = false;
            cancel_gesture(&ctx, current, &gesture);
            ctx.consumed = true;
        }
        if (pressed && err == ESP_OK) {
            held_key = -1;
            held_long_fired = false;
            bool in_hint_strip = false;
            const int key = hit_nav_key(&ctx, current, touch.x, touch.y, &in_hint_strip);
            ctx.consumed = true;
            if (key >= 0) {
                cancel_gesture(&ctx, current, &gesture);
                // 实体键与说明带按下仅记录起点：抬起提交短按，满 500ms 触发长按且不再触发短按。
                // Physical key or hint-cell press arms tracking: release commits short press, 500ms fires hold once.
                if (touch.count == 1) {
                    held_key = key;
                    held_since_ms = ctx.now_ms;
                    held_long_fired = false;
                }
            } else if (in_hint_strip) {
                cancel_gesture(&ctx, current, &gesture);
            } else if (current->on_gesture) {
                ctx.consumed = false;
            } else {
                // 旧页按下先收到 consumed=true，仅 NONE 为同轮 tick 放行。
                // Legacy press sees consumed=true; only NONE clears it for this tick.
                app_redraw_t redraw = current->on_touch
                    ? current->on_touch(&ctx, &latest) : APP_REDRAW_NONE;
                app_present(&ctx, current, redraw);
                if (redraw == APP_REDRAW_NONE) ctx.consumed = false;
            }
        }
        if (err == ESP_OK) was_touched = touch.touched;
        if (held_key >= 0) ctx.consumed = true;

        if (held_key < 0 && current->on_gesture && err == ESP_OK) {
            ui_gesture_event_t event;
            if (ui_gesture_feed(&gesture, &ctx, &event)) {
                app_redraw_t redraw = current->on_gesture(&ctx, &event);
                present_page(&ctx, current, &gesture, redraw);
                if (redraw != APP_REDRAW_NONE) ctx.consumed = true;
            }
        }

        ctx.now_ms = esp_timer_get_time() / 1000;
        rails_idle_check(ctx.now_ms);

        // 电源键短按或空闲超时 = 锁屏。自检页要连着占用 PMU，这时不抢它的事件队列。
        // Short power-key press or idle timeout locks. A page that holds the PMU keeps its event queue.
        if (read_pico_pmu_ready() && !current->holds_pmu
            && ctx.now_ms - last_lock_poll_ms >= APP_LOCK_POLL_MS) {
            last_lock_poll_ms = ctx.now_ms;
            if (read_pico_pmu_take_key_short()) {
                if (ctx.now_ms < s_lock_ignore_until_ms) {
                    ESP_LOGI(TAG, "ignore boot KEY_SHORT");
                } else {
                    lock_and_resume(
                        &ctx, current, &gesture, &held_key, &held_long_fired,
                        &media_mounted, &media_invalidated, &last_media_poll_ms
                    );
                    last_input_ms = ctx.now_ms;
                }
            } else if (ctx.now_ms >= s_lock_ignore_until_ms
                       && app_settings_auto_sleep_sec() != 0
                       && ctx.now_ms - last_input_ms
                           >= (int64_t)app_settings_auto_sleep_sec() * 1000) {
                ESP_LOGI(TAG, "idle lock");
                lock_and_resume(
                    &ctx, current, &gesture, &held_key, &held_long_fired,
                    &media_mounted, &media_invalidated, &last_media_poll_ms
                );
                last_input_ms = ctx.now_ms;
            }
        }

        // 设置里存的字体在 SD 卡上，开机时卡可能还没挂好，这里定期重试。
        // The saved font lives on the card; retry until the mount is ready.
        if (ctx.now_ms - last_font_retry_ms >= FONT_RETRY_INTERVAL_MS) {
            last_font_retry_ms = ctx.now_ms;
            if (try_load_saved_sd_font(!media_invalidated)) {
                held_key = -1;
                held_long_fired = false;
                cancel_gesture(&ctx, current, &gesture);
                ctx.consumed = true;
                app_present(&ctx, current, APP_REDRAW_FULL);
                ESP_LOGI(TAG, "TTF font ready");
            }
        }

        if (held_key >= 0) {
            if (ctx.request_app || ctx.request_return) {
                held_key = -1;
                held_long_fired = false;
            } else if (released && err == ESP_OK) {
                int key = held_key;
                bool was_long = held_long_fired;
                held_key = -1;
                held_long_fired = false;
                ctx.consumed = true;
                if (!was_long) {
                    if (key == UI_KEY_2) {
                        full_refresh_current(&ctx, current);
                    } else {
                        ctx.key_handled = false;
                        app_redraw_t redraw = current->on_key != NULL
                            ? current->on_key(&ctx, key) : APP_REDRAW_NONE;
                        const bool handled = ctx.key_handled || redraw != APP_REDRAW_NONE
                            || ctx.request_app != NULL || ctx.request_return
                            || ctx.request_redraw || ctx.request_lock;
                        if (handled) {
                            present_page(&ctx, current, &gesture, redraw);
                        } else if (key == UI_KEY_1 && current != app_home_page()) {
                            ctx.request_return = true;
                        }
                    }
                }
            } else if (!touch.touched || touch.count != 1
                       || hit_nav_key(&ctx, current, touch.x, touch.y, NULL) != held_key
                       || ctx.now_ms < held_since_ms) {
                held_key = -1;
                held_long_fired = false;
            } else if (!pressed && !held_long_fired
                       && ctx.now_ms - held_since_ms >= UI_LONG_PRESS_MS) {
                int key = held_key;
                held_long_fired = true;
                ctx.consumed = true;
                if (key == UI_KEY_2) {
                    // 长按中键全局返回桌面第 1 页；已在桌面第 1 页时无动作，避免误锁屏。
                    // Hold KEY2 returns to Home page 1 globally; no-op when already on Home page 1.
                    if (current != app_home_page()) {
                        ctx.request_app = app_home_page();
                    } else if (ctx.leaf != 0) {
                        nav_depth = 0;
                        ctx.leaf = 0;
                        app_present(&ctx, current, APP_REDRAW_FULL);
                    }
                } else {
                    ctx.key_handled = false;
                    app_redraw_t redraw = current->on_key_long != NULL
                        ? current->on_key_long(&ctx, key) : APP_REDRAW_NONE;
                    const bool handled = ctx.key_handled || redraw != APP_REDRAW_NONE
                        || ctx.request_app != NULL || ctx.request_return
                        || ctx.request_redraw || ctx.request_lock;
                    if (handled) {
                        present_page(&ctx, current, &gesture, redraw);
                    } else if (key == UI_KEY_1 && current != app_home_page()) {
                        // 多页列表任意页长按左键跨页直接返回上级。
                        // Hold KEY1 on any page returns directly to the parent in nav_stack.
                        ctx.request_return = true;
                    }
                }
            }
        }

        if (!ctx.request_app && !ctx.request_return && current->on_tick != NULL) {
            app_redraw_t redraw = current->on_tick(&ctx);
            if (ctx.keep_awake) last_input_ms = ctx.now_ms;
            if (redraw == APP_REDRAW_PAGE || redraw == APP_REDRAW_FULL) {
                held_key = -1;
                held_long_fired = false;
            }
            present_page(&ctx, current, &gesture, redraw);
        }
        if (config->on_bg_tick != NULL && config->on_bg_tick(&ctx, current, false)) {
            last_input_ms = ctx.now_ms;
        }
        if (ctx.request_lock && !current->holds_pmu &&
            !ctx.request_return && !ctx.request_app) {
            ctx.request_lock = false;
            ctx.request_redraw = false;
            lock_and_resume(
                &ctx, current, &gesture, &held_key, &held_long_fired,
                &media_mounted, &media_invalidated, &last_media_poll_ms
            );
            last_input_ms = ctx.now_ms;
        } else if (ctx.request_redraw && !ctx.request_return && !ctx.request_app) {
            ctx.request_redraw = false;
            present_page(&ctx, current, &gesture, APP_REDRAW_FULL);
        }

        // 返回请求优先；普通切页入栈，回桌面清空栈。
        // Return wins; ordinary switches push onto nav_stack, switching to Home clears it.
        if (ctx.request_return || ctx.request_app) {
            held_key = -1;
            held_long_fired = false;
            const app_desc_t* next = ctx.request_app;
            const bool go_back = ctx.request_return;
            cancel_gesture(&ctx, current, &gesture);
            ctx.request_app = NULL;
            ctx.request_return = false;
            ctx.consumed = true;
            if (go_back) {
                if (nav_depth > 0) {
                    app_nav_entry_t entry = nav_stack[--nav_depth];
                    if (current->on_exit != NULL) current->on_exit(&ctx);
                    current = entry.app;
                    if (current->on_enter != NULL) current->on_enter(&ctx);
                    // 初始化后恢复来源页码，避免 on_enter 覆盖原分页状态。
                    // Restore origin leaf after on_enter overrides its default.
                    ctx.leaf = entry.leaf;
                    app_present(&ctx, current, APP_REDRAW_FULL);
                } else if (current != app_home_page()) {
                    switch_to(&ctx, &current, app_home_page());
                }
            } else if (next == app_home_page()) {
                nav_depth = 0;
                if (current == app_home_page()) {
                    if (ctx.leaf != 0) {
                        ctx.leaf = 0;
                        app_present(&ctx, current, APP_REDRAW_FULL);
                    }
                } else {
                    switch_to(&ctx, &current, next);
                }
            } else if (next != NULL && next != current) {
                nav_push(nav_stack, &nav_depth, current, ctx.leaf);
                switch_to(&ctx, &current, next);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(LOOP_TICK_MS));
    }
}
