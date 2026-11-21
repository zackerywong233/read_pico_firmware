/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：桌面时钟与前台番茄钟；蜂鸣提示遵守全局静音。
 * English: Desk clock and foreground pomodoro timer; alerts honor global mute.
 *
 * 冻结：倒计时仅在页面活动时运行，不在后台唤醒设备。
 * Frozen: The timer runs only while this page is active and never wakes the device.
 */
#include "app.h"
#include "display.h"
#include "e0470_epaper_waveform.h"
#include "esp_timer.h"
#include "read_pico_buzzer.h"
#include "read_pico_pmu.h"
#include "reading_stats.h"
#include "settings.h"
#include "nvs.h"
#include "ui_kit.h"
#include <stdio.h>
#include <time.h>

static bool s_timer, s_break; static int s_minutes=25; static int64_t s_end; static int s_done; static int64_t s_last_draw;
static void done_load(void){nvs_handle_t h;if(nvs_open("rp_focus",NVS_READONLY,&h)==ESP_OK){uint32_t n=0;if(nvs_get_u32(h,"done",&n)==ESP_OK)s_done=(int)n;nvs_close(h);}}
static void done_save(void){nvs_handle_t h;if(nvs_open("rp_focus",NVS_READWRITE,&h)==ESP_OK){nvs_set_u32(h,"done",(uint32_t)s_done);nvs_commit(h);nvs_close(h);}}
static void render(app_ctx_t*ctx,uint8_t*fb){(void)ctx;ui_draw_header(fb,"专注时钟 Focus","时钟 / 番茄钟 Clock / Pomodoro");if(!s_timer){time_t t=time(NULL)+(time_t)app_settings_timezone_min()*60;struct tm tmv;gmtime_r(&t,&tmv);char timebuf[16],date[32],line[64];strftime(timebuf,sizeof(timebuf),"%H:%M",&tmv);strftime(date,sizeof(date),"%Y-%m-%d",&tmv);ui_text_vc(fb,epd_rotated_display_width()/2,UI_CONTENT_TOP+180,UI_PX_HERO,timebuf,EPD_DRAW_ALIGN_CENTER,false);ui_text_vc(fb,epd_rotated_display_width()/2,UI_CONTENT_TOP+270,UI_PX_BODY,date,EPD_DRAW_ALIGN_CENTER,false);const pmu_snapshot_t*p=read_pico_pmu_get();snprintf(line,sizeof(line),"电量 %u%% · 累计阅读 %u 分钟",p?p->soc_permille/10:0,reading_stats_total_seconds()/60);ui_text_vc(fb,epd_rotated_display_width()/2,UI_CONTENT_TOP+360,UI_PX_CAPTION,line,EPD_DRAW_ALIGN_CENTER,false);ui_text_vc(fb,epd_rotated_display_width()/2,UI_CONTENT_BOTTOM-30,UI_PX_BTN,"轻触开始专注",EPD_DRAW_ALIGN_CENTER,false);}else{int64_t left=s_end-esp_timer_get_time()/1000000;if(left<0)left=0;char value[64];snprintf(value,sizeof(value),"%02lld:%02lld",(long long)(left/60),(long long)(left%60));ui_text_vc(fb,epd_rotated_display_width()/2,UI_CONTENT_TOP+230,UI_PX_HERO,value,EPD_DRAW_ALIGN_CENTER,false);ui_text_vc(fb,epd_rotated_display_width()/2,UI_CONTENT_TOP+330,UI_PX_BODY,s_break?"休息 Rest":"专注 Focus",EPD_DRAW_ALIGN_CENTER,false);snprintf(value,sizeof(value),"时长 %d 分钟 · 完成 %d",s_minutes,s_done);ui_text_vc(fb,epd_rotated_display_width()/2,UI_CONTENT_BOTTOM-30,UI_PX_CAPTION,value,EPD_DRAW_ALIGN_CENTER,false);}}
static app_redraw_t touch(app_ctx_t*ctx,const cst836u_touch_t*t){if(t->y<UI_CONTENT_TOP||t->y>UI_CONTENT_BOTTOM)return APP_REDRAW_NONE;if(!s_timer){s_timer=true;s_break=false;s_end=esp_timer_get_time()/1000000+s_minutes*60;}else{s_timer=false;s_break=false;}s_last_draw=ctx->now_ms;return APP_REDRAW_FULL;}
static app_redraw_t key(app_ctx_t*ctx,int key){if(key==UI_KEY_1){if(!s_timer){s_minutes=s_minutes==15?25:s_minutes==25?45:15;return APP_REDRAW_FULL;}s_timer=false;return APP_REDRAW_FULL;}if(key==UI_KEY_2){s_timer=false;s_break=false;s_done++;return APP_REDRAW_FULL;}return APP_REDRAW_NONE;}
static app_redraw_t tick(app_ctx_t*ctx){if(!s_timer){if(ctx->now_ms-s_last_draw>=60000){s_last_draw=ctx->now_ms;render(ctx,ctx->fb);return APP_REDRAW_AREA;}return APP_REDRAW_NONE;}int64_t now=esp_timer_get_time()/1000000;if(now>=s_end){if(!s_break){s_break=true;s_end=now+5*60;s_done++;done_save();if(!app_settings_muted())(void)read_pico_buzzer_tone(880,120);}else{s_break=false;s_timer=false;}return APP_REDRAW_FULL;}if(now%60==0&&ctx->now_ms-s_last_draw>50000){s_last_draw=ctx->now_ms;render(ctx,ctx->fb);return APP_REDRAW_AREA;}return APP_REDRAW_NONE;}
static EpdRect area(app_ctx_t*ctx){(void)ctx;int w=epd_rotated_display_width();return(EpdRect){.x=w/2-230,.y=UI_CONTENT_TOP+105,.width=460,.height=240};}
static void enter(app_ctx_t*ctx){(void)ctx;done_load();s_last_draw=0;}
const app_desc_t app_focus={.title="专注时钟 Focus",.detail="桌面时钟与番茄钟 Clock & Pomodoro",.blocks_idle_sleep=true,.render=render,.on_enter=enter,.on_touch=touch,.on_key=key,.on_tick=tick,.area_hint=area};
