/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：将触摸笔迹保存在 PSRAM，并支持撤销、清空与 PNG 导出。
 * English: Keep touch strokes in PSRAM and support undo, clear and PNG export.
 *
 * 冻结：页面离开时释放画布；仅写入挂载的 SD 卡。
 * Frozen: Release the canvas on exit and write only to a mounted SD card.
 */
#include "app.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "settings.h"
#include "read_pico_sd.h"
#include "ui_kit.h"
#include "ui_menu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define PSRAM (MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)
static uint8_t *s_canvas,*s_undo;static size_t s_bytes;static int s_brush=4;static bool s_erase;
static uint32_t crc_step(uint32_t c,const uint8_t*p,size_t n){while(n--){c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^(0xedb88320u&-(c&1));}return c;}
static void be32(uint8_t*p,uint32_t x){p[0]=x>>24;p[1]=x>>16;p[2]=x>>8;p[3]=x;}
static bool chunk(FILE*f,const char type[4],const uint8_t*data,uint32_t n){uint8_t h[8],c[4];be32(h,n);memcpy(h+4,type,4);if(fwrite(h,1,8,f)!=8||(n&&fwrite(data,1,n,f)!=n))return false;uint32_t crc=crc_step(crc_step(~0u,(const uint8_t*)type,4),data,n)^~0u;be32(c,crc);return fwrite(c,1,4,f)==4;}
static bool save_png(const char*path){int w=epd_width(),h=epd_height();if(!s_canvas||w<1||h<1)return false;size_t raw_n=(size_t)(w+1)*h;uint8_t*raw=heap_caps_malloc(raw_n,PSRAM);if(!raw)return false;for(int y=0;y<h;y++){raw[(size_t)y*(w+1)]=0;for(int x=0;x<w;x++){size_t p=(size_t)y*w+x;uint8_t v=(p&1)?s_canvas[p/2]&15:s_canvas[p/2]>>4;raw[(size_t)y*(w+1)+1+x]=(uint8_t)(v*17);}}
    size_t zcap=raw_n+raw_n/65535*5+16;uint8_t*z=heap_caps_malloc(zcap,PSRAM);if(!z){free(raw);return false;}size_t q=0;z[q++]=0x78;z[q++]=0x01;size_t off=0;uint32_t a=1,b=0;while(off<raw_n){uint16_t take=raw_n-off>65535?65535:(uint16_t)(raw_n-off);bool last=off+take==raw_n;z[q++]=last?1:0;z[q++]=take;z[q++]=take>>8;uint16_t inv=~take;z[q++]=inv;z[q++]=inv>>8;memcpy(z+q,raw+off,take);for(size_t i=off;i<off+take;i++){a=(a+raw[i])%65521;b=(b+a)%65521;}q+=take;off+=take;}be32(z+q,(b<<16)|a);q+=4;FILE*f=fopen(path,"wb");bool ok=false;if(f){static const uint8_t sig[]={137,80,78,71,13,10,26,10};uint8_t ihdr[13];be32(ihdr,w);be32(ihdr+4,h);ihdr[8]=8;ihdr[9]=0;ihdr[10]=ihdr[11]=ihdr[12]=0;ok=fwrite(sig,1,8,f)==8&&chunk(f,"IHDR",ihdr,13)&&chunk(f,"IDAT",z,(uint32_t)q)&&chunk(f,"IEND",NULL,0);if(fclose(f))ok=false;}free(raw);free(z);return ok;}
static void render(app_ctx_t*ctx,uint8_t*fb){if(!s_canvas){s_bytes=(size_t)epd_width()*epd_height()/2;s_canvas=heap_caps_malloc(s_bytes,PSRAM);s_undo=heap_caps_malloc(s_bytes,PSRAM);if(s_canvas)memset(s_canvas,0xff,s_bytes);}if(s_canvas)memcpy(fb,s_canvas,s_bytes);else ui_clear_page(fb);ui_draw_header(fb,"随手便签 Sketch","细 / 中 / 粗 · 橡皮 · 撤销");const char*labels[]={"保存","撤销","清空","粗细","橡皮","设锁屏"};for(int i=0;i<6;i++)ui_text_vc(fb,UI_MARGIN+48+i*105,UI_CONTENT_BOTTOM-34,UI_PX_CAPTION,labels[i],EPD_DRAW_ALIGN_CENTER,false);ui_draw_menu_handle(fb,false);(void)ctx;}
static void on_enter(app_ctx_t*ctx){(void)ctx;}
static void on_exit(app_ctx_t*ctx){(void)ctx;free(s_canvas);free(s_undo);s_canvas=s_undo=NULL;s_bytes=0;}
static app_redraw_t touch(app_ctx_t*ctx,const cst836u_touch_t*t){if(!s_canvas||!s_undo)return APP_REDRAW_FULL;int w=epd_rotated_display_width();if(t->y>=UI_CONTENT_BOTTOM-72&&t->y<UI_CONTENT_BOTTOM){int hit=(int)t->x*6/w;read_pico_sd_info_t i={0};read_pico_sd_get_info(&i);if(hit==0&&i.mounted){mkdir("/sdcard/notes",0777);mkdir("/sdcard/notes/sketch",0777);char path[96];snprintf(path,sizeof(path),"/sdcard/notes/sketch_%lld.png",(long long)time(NULL));(void)save_png(path);}else if(hit==1)memcpy(s_canvas,s_undo,s_bytes);else if(hit==2){memcpy(s_undo,s_canvas,s_bytes);memset(s_canvas,0xff,s_bytes);}else if(hit==3)s_brush=s_brush==2?5:s_brush==5?9:2;else if(hit==4)s_erase=!s_erase;else if(hit==5&&i.mounted&&save_png("/sdcard/screensaver.png"))app_settings_set_saver_style(APP_SAVER_IMAGE);return APP_REDRAW_FULL;}if(t->y<UI_CONTENT_TOP||t->y>=UI_CONTENT_BOTTOM)return APP_REDRAW_NONE;if(ctx->pressed)memcpy(s_undo,s_canvas,s_bytes);int x=t->x,y=t->y,r=s_brush;uint8_t val=s_erase?15:0;for(int dy=-r;dy<=r;dy++)for(int dx=-r;dx<=r;dx++)if(dx*dx+dy*dy<=r*r){int xx=x+dx,yy=y+dy;if(xx<0||xx>=epd_width()||yy<0||yy>=epd_height())continue;size_t p=(size_t)yy*epd_width()+xx;uint8_t*b=&s_canvas[p/2];if(p&1)*b=(uint8_t)((*b&0xf0)|val);else*b=(uint8_t)((*b&0x0f)|(val<<4));}render(ctx,ctx->fb);return ctx->released?APP_REDRAW_FULL:APP_REDRAW_AREA;}
static EpdRect area(app_ctx_t*ctx){(void)ctx;return(EpdRect){.x=0,.y=UI_CONTENT_TOP,.width=epd_rotated_display_width(),.height=UI_CONTENT_BOTTOM-UI_CONTENT_TOP};}
const app_desc_t app_sketch={.title="随手便签 Sketch",.detail="手写便签与 PNG 保存 Handwritten notes",.render=render,.on_enter=on_enter,.on_exit=on_exit,.on_touch=touch,.area_hint=area};
