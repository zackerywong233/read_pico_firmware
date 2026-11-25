/* SPDX-License-Identifier: Apache-2.0
 * 中文：运行真实主循环，用确定性触摸和任务延时退出验证调度、8 层导航栈与新三键契约。
 * English: Run the real loop with deterministic touch and task-delay exit to test scheduler, 8-level nav stack and v3.2 key contract.
 */
#include "app_loop.h"
#include "ui_gesture.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static jmp_buf done;
typedef struct { int x, y, count, error; } sample_t;
static sample_t samples[64];
static int sample_count, step, ticks, enters, exits, touch_calls, keys[3], long_keys[3], events[8];
static int fulls, mode, tick_consumed[64];
static int lock_calls, hint_draws;
static ui_key_hints_t last_hints;
static bool key_handles_all, long_handles_all, tick_keep_awake;
static app_redraw_t response;
static bool request_on_touch, request_on_tick, cancel_clobber;
static bool home_on_touch, home_on_tick, home_on_key, start_second;
static int home_enters, home_renders, rendered_leaf;
static app_desc_t first, second, third;
static bool full_tick, lock_due, font_due, hide_hints;
static int64_t time_offset, time_step;
static int du_areas, gl_areas;
static bool media_test, sd_font, saved_sd_font;
static bool mounted_steps[64];
static int media_lost, builtin_opens, font_opens, probes, loss_step;
static uint8_t fake_fb[16];
int E0470_WAVEFORM;

int cst836u_read(void* h, cst836u_touch_t* t) {
    (void)h;
    sample_t s = samples[step];
    *t = (cst836u_touch_t){ .touched=s.count>0, .count=s.count, .x=s.x, .y=s.y };
    t->points[0].active=s.count>0;
    return s.error;
}
int cst836u_get_info(void* h,cst836u_info_t* i) {(void)h;(void)i;return 0;}
const char* esp_err_to_name(int e) {(void)e;return "test";}
int64_t esp_timer_get_time(void) {return time_offset+(int64_t)(step+1)*time_step;}
void vTaskDelay(int ms) {(void)ms;if (++step>=sample_count) longjmp(done,1);}
bool continuous_du_init(void) {return true;}
void guard_draw_result(EpdiyHighlevelState* h,enum EpdDrawError e) {(void)h;assert(e==0);}
enum EpdDrawError update_display_area_with(EpdiyHighlevelState*h,const void*w,int m,EpdRect a) {(void)h;(void)w;(void)a;if(m==MODE_DU)du_areas++;else if(m==MODE_GL16)gl_areas++;return 0;}
enum EpdDrawError update_display_full(EpdiyHighlevelState*h) {(void)h;fulls++;return 0;}
enum EpdDrawError update_display_mode(EpdiyHighlevelState*h,int m) {(void)h;(void)m;mode++;return 0;}
enum EpdDrawError update_display_white(EpdiyHighlevelState*h) {(void)h;return 0;}
bool display_take_white_exit(void) {return false;}
void rails_idle_check(int64_t n) {(void)n;}
bool read_pico_pmu_ready(void) {return lock_due;}
bool read_pico_pmu_take_key_short(void) {return true;}
void enter_lock_and_sleep(EpdiyHighlevelState*h,int64_t*t,void*a) {(void)h;(void)t;(void)a;lock_due=false;++lock_calls;time_offset+=1000000;}
bool app_lock_powers_down(void) {return false;}
const char* app_settings_font_path(void) {return "builtin";}
uint16_t app_settings_auto_sleep_sec(void) {return 120;}
bool ttf_font_path_is_builtin(const char*p) {(void)p;return !font_due&&!saved_sd_font;}
bool ttf_font_ready(void) {return true;}
bool ttf_font_is_builtin(void) {return !font_due&&!sd_font;}
const char* ttf_font_path(void) {return "other";}
int ttf_font_open(const char*p) {(void)p;font_opens++;sd_font=true;return 0;}
int ttf_font_open_builtin(void) {assert(media_lost>0);builtin_opens++;sd_font=font_due=false;return 0;}
int read_pico_sd_get_info(read_pico_sd_info_t*i) {i->mounted=media_test?mounted_steps[step]:font_due;return media_test&&!i->mounted?ESP_ERR_INVALID_STATE:0;}
void read_pico_sd_start_probe(void) {probes++;}
static void lost(app_ctx_t*c) {(void)c;media_lost++;loss_step=step;}
EpdRect ui_content_refresh_area(void) {return (EpdRect){0,0,684,1000};}
const app_desc_t* app_home_page(void) {return &first;}
const app_desc_t* app_at(int i) {return i==0?&first:i==1?&second:i==2?&third:NULL;}
int ui_key_hit_test(uint16_t x,uint16_t y) {return y>=1300 && x<480?x/160:-1;}
int ui_key_hint_hit_test(uint16_t x,uint16_t y,const ui_key_hints_t* h) {
    if (!h || h->hidden || y < UI_KEY_HINT_RULE_Y || y >= UI_LOCK_HEIGHT || x >= UI_LOCK_WIDTH) return -1;
    int col_w = UI_LOCK_WIDTH / 3;
    int col = (int)x < col_w ? UI_KEY_1 : (int)x < col_w * 2 ? UI_KEY_2 : UI_KEY_3;
    const char* label = col == UI_KEY_1 ? h->left : col == UI_KEY_2 ? (h->center ? h->center : "刷新 · 长按桌面") : h->right;
    return (label && label[0]) ? col : -1;
}
void ui_draw_key_hints(uint8_t*fb, const ui_key_hints_t* h) {(void)fb; if(h){last_hints=*h; hint_draws++;}}

static void enter(app_ctx_t*c) {(void)c;enters++;}
static void leave(app_ctx_t*c) {(void)c;exits++;}
static void render(app_ctx_t*c,uint8_t*f) {(void)f;rendered_leaf=c->leaf;}
static void home_enter(app_ctx_t*c) {c->leaf=0;enters++;home_enters++;}
static void home_render(app_ctx_t*c,uint8_t*f) {(void)f;rendered_leaf=c->leaf;home_renders++;}
static void hints_cb(app_ctx_t*c, ui_key_hints_t*out) {
    (void)c;
    if (hide_hints) { out->hidden = true; return; }
    out->left = "上一页";
    out->right = "下一页";
}
static app_redraw_t touch(app_ctx_t*c,const cst836u_touch_t*t) {
    (void)t;touch_calls++;assert(c->pressed&&c->consumed);
    if(request_on_touch)c->request_app=&second;
    if(home_on_touch)c->request_return=true;
    return response;
}
static app_redraw_t gesture(app_ctx_t*c,const ui_gesture_event_t*e) {
    events[e->type]++;
    if(e->type==UI_GESTURE_CANCEL&&cancel_clobber) {c->request_app=NULL;c->request_return=false;}
    if(e->type==UI_GESTURE_PRESS&&request_on_touch)c->request_app=&second;
    if(e->type==UI_GESTURE_PRESS&&home_on_touch)c->request_return=true;
    return response;
}
static app_redraw_t key(app_ctx_t*c,int k) {
    keys[k]++;
    if(key_handles_all)c->key_handled=true;
    if(home_on_key)c->request_return=true;
    return APP_REDRAW_NONE;
}
static app_redraw_t key_long(app_ctx_t*c,int k) {
    long_keys[k]++;
    if(long_handles_all)c->key_handled=true;
    return APP_REDRAW_NONE;
}
static app_redraw_t tick(app_ctx_t*c) {
    ticks++;tick_consumed[step]=c->consumed;
    if(tick_keep_awake)c->keep_awake=true;
    if(request_on_tick)c->request_app=&second;
    if(home_on_tick){c->request_return=true;home_on_tick=false;}
    return full_tick ? APP_REDRAW_FULL : APP_REDRAW_NONE;
}
static void reset(void) {
    media_test=sd_font=saved_sd_font=false;media_lost=builtin_opens=font_opens=probes=0;loss_step=-1;
    memset(mounted_steps,0,sizeof(mounted_steps));
    key_handles_all=long_handles_all=tick_keep_awake=hide_hints=false;
    home_on_touch=home_on_tick=home_on_key=start_second=false;
    home_enters=home_renders=0;rendered_leaf=-1;hint_draws=0;
    memset(&last_hints,0,sizeof(last_hints));
    full_tick=lock_due=font_due=false;time_offset=0;time_step=10000;du_areas=gl_areas=0;
    memset(samples,0,sizeof(samples));memset(keys,0,sizeof(keys));memset(long_keys,0,sizeof(long_keys));
    memset(events,0,sizeof(events));memset(tick_consumed,0,sizeof(tick_consumed));
    sample_count=step=ticks=enters=exits=touch_calls=fulls=mode=lock_calls=0;
    request_on_touch=request_on_tick=cancel_clobber=false;response=APP_REDRAW_NONE;
    first=(app_desc_t){.title="first",.render=render,.on_enter=enter,.on_exit=leave,.on_touch=touch,.on_tick=tick,.on_key=key,.on_key_long=key_long};
    second=(app_desc_t){.title="second",.render=render,.on_enter=enter,.on_exit=leave,.on_key=key,.on_key_long=key_long};
    third=(app_desc_t){.title="third",.render=render,.on_enter=enter,.on_exit=leave};
}
static void add(int x,int y,int count,int error) {samples[sample_count++]=(sample_t){x,y,count,error};}
static void run(void) {app_loop_config_t cfg={.fb=fake_fb,.first_app=start_second?&second:&first};if(!setjmp(done))app_loop_run(&cfg);}

static int nav_step;
static app_redraw_t nav_first_tick(app_ctx_t*c) {
    if (nav_step == 0) { c->leaf = 2; c->request_app = &second; nav_step = 1; }
    return APP_REDRAW_NONE;
}
static app_redraw_t nav_second_tick(app_ctx_t*c) {
    if (nav_step == 1) { c->leaf = 5; c->request_app = &third; nav_step = 2; }
    else if (nav_step == 3) { c->request_return = true; nav_step = 4; }
    return APP_REDRAW_NONE;
}
static app_redraw_t nav_third_tick(app_ctx_t*c) {
    if (nav_step == 2) { c->request_return = true; nav_step = 3; }
    return APP_REDRAW_NONE;
}

static bool home_leaf_set;
static app_redraw_t home_set_leaf_tick(app_ctx_t*c) {
    if (!home_leaf_set) { c->leaf = 2; home_leaf_set = true; }
    return APP_REDRAW_NONE;
}

static bool bg_switch_triggered;
static int bg_ticks;
static bool bg_test_redraw_and_lock;
static bool bg_tick_switch(app_ctx_t* c, const app_desc_t* cur, bool menu_open) {
    (void)menu_open;
    ++bg_ticks;
    if (bg_test_redraw_and_lock) {
        if (step == 0) { c->request_redraw = true; return true; }
        if (step == 1) { c->request_lock = true; return true; }
        return false;
    }
    if (!bg_switch_triggered && cur == &first && step == 1) {
        bg_switch_triggered = true;
        c->request_app = &second;
        return true;
    }
    if (bg_switch_triggered && cur == &second && step == 2) {
        c->request_return = true;
        return true;
    }
    return false;
}
static void run_bg(void) {
    app_loop_config_t cfg = { .fb = fake_fb, .first_app = &first, .on_bg_tick = bg_tick_switch };
    if (!setjmp(done)) app_loop_run(&cfg);
}

int main(void) {
    // 1. 基本触摸与手势派发。/ Basic touch and gesture dispatch.
    reset();add(100,400,1,0);add(110,400,1,0);add(0,0,0,0);run();
    assert(touch_calls==1&&ticks==3&&!tick_consumed[0]&&hint_draws==1);
    reset();response=APP_REDRAW_AREA;add(100,400,1,0);run();assert(touch_calls==1&&tick_consumed[0]);
    reset();first.on_gesture=gesture;response=APP_REDRAW_AREA;add(100,400,1,0);add(0,0,0,0);run();
    assert(!touch_calls&&events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_TAP]==1&&tick_consumed[0]);
    reset();first.on_gesture=gesture;add(100,400,1,0);add(0,0,0,9);add(100,400,1,0);add(0,0,0,0);run();
    assert(events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]);
    reset();first.on_gesture=gesture;request_on_touch=true;cancel_clobber=true;add(100,400,1,0);run();
    assert(enters==2&&events[UI_GESTURE_CANCEL]==1);

    // 2. 底部按键说明带回调与隐藏。/ Key hints callback and fullscreen hiding.
    reset();first.key_hints=hints_cb;add(0,0,0,0);run();
    assert(hint_draws==1&&strcmp(last_hints.left,"上一页")==0&&strcmp(last_hints.right,"下一页")==0);
    reset();first.key_hints=hints_cb;hide_hints=true;add(0,0,0,0);run();
    assert(hint_draws==0);

    // 3. 三键短按：任意页均接收 KEY1/KEY3，KEY2 恒定整屏强刷。
    // Short press: any page receives KEY1/KEY3; KEY2 always runs full GC16 refresh.
    reset();key_handles_all=true;for(int k=0;k<3;k++){add(k*160+80,1500,1,0);add(0,0,0,0);}run();
    assert(keys[0]==1&&!keys[1]&&keys[2]==1&&fulls==2);

    // 4. 非首页未消费短按 KEY1 返回上级；未消费短按 KEY3 无动作。
    // Non-home unhandled short KEY1 returns; unhandled short KEY3 is no-op.
    reset();start_second=true;first.on_enter=home_enter;first.render=home_render;
    add(400,1500,1,0);add(0,0,0,0); // KEY3 unhandled -> no-op
    add(80,1500,1,0);add(0,0,0,0);  // KEY1 unhandled -> return home
    run();
    assert(keys[2]==1&&keys[0]==1&&exits==1&&home_enters==1&&home_renders==1);

    // 5. 长按 KEY2 (500ms) 从子页全局返回桌面第 1 页；抬起不触发短按。
    // Long KEY2 returns to Home page 1 from any sub-page without firing short press.
    reset();start_second=true;key_handles_all=true;first.on_enter=home_enter;first.render=home_render;
    time_step=200000;
    add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(0,0,0,0);run();
    assert(exits==1&&home_enters==1&&home_renders==1&&rendered_leaf==0&&fulls==2);

    // 6. 桌面第 1 页长按 KEY2 无动作（不刷屏、不锁屏）；桌面第 3 页长按 KEY2 回第 1 页。
    // Long KEY2 on Home page 1 is no-op; on Home page 3 resets to page 1.
    reset();first.render=home_render;time_step=200000;
    add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(0,0,0,0);run();
    assert(home_renders==1&&fulls==1&&lock_calls==0);

    reset();home_leaf_set=false;first.on_tick=home_set_leaf_tick;first.render=home_render;time_step=200000;
    add(0,0,0,0);add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(0,0,0,0);run();
    assert(home_renders==2&&rendered_leaf==0&&fulls==2);

    // 7. 长按 KEY1 跨页直接返回上级（即使页面消费短按 KEY1）；长按 KEY3 派发 on_key_long 且不触发短按。
    // Long KEY1 returns directly even if short KEY1 is handled; long KEY3 fires on_key_long without short press.
    reset();start_second=true;key_handles_all=true;first.on_enter=home_enter;first.render=home_render;
    time_step=200000;
    add(80,1500,1,0);add(80,1500,1,0);add(80,1500,1,0);add(80,1500,1,0);add(0,0,0,0);run();
    assert(!keys[0]&&long_keys[0]==1&&exits==1&&home_enters==1);

    reset();key_handles_all=true;long_handles_all=true;time_step=200000;
    add(400,1500,1,0);add(400,1500,1,0);add(400,1500,1,0);add(400,1500,1,0);add(0,0,0,0);run();
    assert(!keys[2]&&long_keys[2]==1);

    // 8. 滑出按键、读错或锁屏打断取消长按与短按。
    // Slide-off, read error or lock cancels both hold and short press.
    reset();key_handles_all=true;long_handles_all=true;time_step=300000;
    add(400,1500,1,0);add(80,1500,1,0);add(400,1500,1,0);add(0,0,0,0);run();assert(!keys[2]&&!long_keys[2]);
    reset();key_handles_all=true;long_handles_all=true;time_step=300000;
    add(400,1500,1,0);add(0,0,0,9);add(400,1500,1,0);add(400,1500,1,0);run();assert(!keys[2]&&!long_keys[2]);
    reset();key_handles_all=true;long_handles_all=true;time_step=1000000;lock_due=true;
    add(0,0,0,0);add(400,1500,1,0);add(400,1500,1,0);add(0,0,0,0);run();assert(!lock_due&&!keys[2]&&!long_keys[2]);

    // 9. 多级导航栈：first(leaf=2) -> second(leaf=5) -> third -> return second(leaf=5) -> return first(leaf=2)。
    // Multi-level nav stack restores both page and leaf on each pop.
    reset();nav_step=0;
    first.on_tick=nav_first_tick;first.on_enter=home_enter;first.render=home_render;
    second.on_tick=nav_second_tick;
    third.on_tick=nav_third_tick;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(nav_step==4&&enters==5&&exits==4&&home_renders==2&&rendered_leaf==2);

    // 10. 整页刷新/延迟字体/锁屏/拔卡打断手势与长按。
    // Full tick, delayed font, lock and card removal cancel gestures and key holds.
    reset();first.on_gesture=gesture;full_tick=true;add(100,400,1,0);add(0,0,0,0);run();
    assert(events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]);
    reset();first.on_gesture=gesture;font_due=true;time_offset=4000000;add(100,400,1,0);add(0,0,0,0);run();
    assert(events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]&&tick_consumed[0]);
    reset();first.on_gesture=gesture;time_step=1000000;lock_due=true;
    add(100,400,1,0);add(100,400,1,0);add(100,400,1,0);add(0,0,0,0);run();
    assert(!lock_due&&events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]&&tick_consumed[2]);

    reset();media_test=sd_font=true;mounted_steps[0]=true;time_step=600000;first.on_media_lost=lost;
    add(0,0,0,0);add(80,1500,1,0);add(0,0,0,0);run();
    assert(media_lost==1&&loss_step==1&&builtin_opens==1&&!keys[0]&&fulls==2);
    reset();media_test=true;mounted_steps[1]=true;time_step=600000;first.on_media_lost=lost;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==1&&loss_step==2&&!builtin_opens);
    reset();media_test=true;mounted_steps[0]=true;time_step=600000;first.on_media_lost=lost;first.on_gesture=gesture;
    add(100,400,1,0);add(100,400,1,0);add(0,0,0,0);run();
    assert(media_lost==1&&events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]);
    reset();media_test=sd_font=saved_sd_font=true;mounted_steps[0]=mounted_steps[2]=true;time_step=4000000;first.on_media_lost=lost;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==2&&builtin_opens==2&&!probes&&font_opens==2);

    // 11. 后台服务回调（如 BLE）切页、返回、强刷与锁屏。
    // Background service callback switch, return, redraw and lock.
    reset();bg_switch_triggered=false;bg_ticks=0;bg_test_redraw_and_lock=false;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run_bg();
    assert(bg_ticks==4&&enters==3&&exits==2);
    reset();bg_test_redraw_and_lock=true;bg_ticks=0;lock_calls=0;
    add(0,0,0,0);add(0,0,0,0);run_bg();
    assert(fulls==2&&lock_calls==1);
    reset();bg_test_redraw_and_lock=true;bg_ticks=0;lock_calls=0;first.holds_pmu=true;
    add(0,0,0,0);add(0,0,0,0);run_bg();
    assert(fulls==2&&lock_calls==0);

    // 12. 底部说明带触控与屏下三键共享短/长按路由，空位拦截不透传，全屏隐藏态透传。
    // Bottom key-hint strip shares short/long KEY1/2/3 routing; empty cells absorb taps; hidden strip passes through.
    reset();first.key_hints=hints_cb;key_handles_all=true;
    add(100,1190,1,0);add(0,0,0,0); // 左格 -> KEY1
    add(342,1190,1,0);add(0,0,0,0); // 中格 -> KEY2 强刷
    add(580,1190,1,0);add(0,0,0,0); // 右格 -> KEY3
    run();
    assert(keys[0]==1&&!keys[1]&&keys[2]==1&&fulls==2&&touch_calls==0);

    // 子页长按说明带中格回桌面首页；未消费左格短按返回上级。
    // Sub-page holding hint center returns to Home; tapping hint left returns to parent.
    reset();start_second=true;first.on_enter=home_enter;first.render=home_render;
    add(100,1190,1,0);add(0,0,0,0);run();
    assert(keys[0]==1&&exits==1&&home_enters==1);

    reset();start_second=true;key_handles_all=true;first.on_enter=home_enter;first.render=home_render;
    time_step=200000;
    add(342,1190,1,0);add(342,1190,1,0);add(342,1190,1,0);add(342,1190,1,0);add(0,0,0,0);run();
    assert(exits==1&&home_enters==1&&rendered_leaf==0&&fulls==2);

    // 首页左格与右格均为空：点击说明带空位被吸收，不触发 on_touch 也不触发 on_key。
    // Empty hint cells on Home absorb taps without firing on_touch or on_key.
    reset();add(100,1190,1,0);add(0,0,0,0);add(580,1190,1,0);add(0,0,0,0);run();
    assert(touch_calls==0&&keys[0]==0&&keys[2]==0);

    // 全屏隐藏态（hidden=true）：点击 y=1190 正常透传给页面。
    // Fullscreen hidden strip lets y=1190 pass through to the page.
    reset();first.key_hints=hints_cb;hide_hints=true;add(100,1190,1,0);add(0,0,0,0);run();
    assert(touch_calls==1&&keys[0]==0);

    puts("app_loop: v3.2 scheduler, nav stack and 3-key scenarios passed");
    return 0;
}
