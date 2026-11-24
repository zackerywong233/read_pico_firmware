"""Compile actual shelf helpers for deterministic ordering and failure regressions.

SPDX-FileCopyrightText: 2026 mindreset
SPDX-License-Identifier: Apache-2.0
中文：抽取真实静态助手，避免宿主链接硬件页面依赖。
English: Extract real static helpers without linking hardware page dependencies.
"""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1] / "main/apps/app_book.c"

def function(name):
    text = source.read_text(encoding="utf-8")
    import re
    found = re.search(r"^static [^\n]+\b" + name + r"\([^\n]*\) \{", text, re.M)
    assert found, name
    start, at, depth, quote, escape = found.start(), found.end(), 1, None, False
    while depth:
        c = text[at]
        if quote:
            if escape: escape = False
            elif c == "\\": escape = True
            elif c == quote: quote = None
        elif c in "\"'": quote = c
        elif c == "{": depth += 1
        elif c == "}": depth -= 1
        at += 1
    return text[start:at]

unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdatomic.h>
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_INVALID_SIZE 3
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
typedef struct {int leaf;bool request_menu;bool key_handled;int64_t now_ms;} app_ctx_t;
typedef enum {APP_REDRAW_PAGE,APP_REDRAW_AREA,APP_REDRAW_NONE,APP_REDRAW_FULL} app_redraw_t;
typedef struct {int x,y,width,height;} EpdRect;
static EpdRect ui_bar_rect(int i,int count){int width=(508-(count-1)*12)/count;return(EpdRect){40+i*(width+12),1096,width,96};}
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;}
typedef struct {char name[256],path[288];uint32_t size;bool is_flash,has_progress;uint8_t pct;uint32_t recent;bool selected,removed,search_match;} shelf_entry_t;
#define BOOK_ROWS 7
#define BOOK_GRID_ROWS 6
#define BOOK_SEARCH_RESULTS 7
static int s_recent_index = -1;
static uint8_t app_settings_book_shelf_view(void) { return 1; }
static void book_cover_clear_lru(void) {}
#define BOOK_NOTE_ROWS 8
#define UI_BTN_H 84
static EpdRect ui_row_rect(int i,int count,int y,int height){EpdRect r=ui_bar_rect(i,count);r.y=y;r.height=height;return r;}
static char s_query[65],s_search_draft[65],s_batch_message[128];
static bool s_batch_confirm,s_batch_delete;
static bool read_pico_search_match(const char* name,const char* query){return !*query||strstr(name,query)!=NULL;}
static int book_chapter_count(void){return 1;}
#define BOOK_TOC_ROWS 10
static int s_filter;
static bool s_recent_sort;
static shelf_entry_t* s_shelf;
static size_t s_shelf_capacity;
static int s_count,s_visible_count;
static char s_message[128],s_shelf_warning[128],s_storage[128],s_book_name[128];
static bool s_pending_invalidated,test_oom,test_degraded;
static unsigned s_store_revision;
typedef struct {char path[288];bool is_flash;} book_store_root_t;
typedef struct {uint32_t file_size;uint16_t chapter;uint32_t byte_off;uint8_t px,pct;uint32_t last_open_s;} book_progress_t;
typedef struct book_progress_watch {char path[288];atomic_bool invalidated;struct book_progress_watch* next;} book_progress_watch_t;
static book_progress_watch_t* test_watches;
static book_progress_watch_t* book_progress_watch_create(const char* path){book_progress_watch_t* w=calloc(1,sizeof(*w));assert(w);strcpy(w->path,path);atomic_init(&w->invalidated,false);w->next=test_watches;test_watches=w;return w;}
static bool book_progress_watch_invalidated(const book_progress_watch_t* w){return atomic_load(&w->invalidated);}
static void book_progress_watch_destroy(book_progress_watch_t* w){book_progress_watch_t** p=&test_watches;while(*p!=w)p=&(*p)->next;*p=w->next;free(w);}
static book_store_root_t test_roots[2];
static int test_root_count=1;
static void* heap_caps_realloc(void* p,size_t n,int caps){(void)caps;return test_oom?NULL:realloc(p,n);}
static void* heap_caps_malloc(size_t n,int caps){(void)caps;return test_oom?NULL:malloc(n);}
static bool book_store_flash_ready(void){return true;}
static bool book_store_roots_degraded(void){return test_degraded;}
static uint64_t book_store_free_bytes(const book_store_root_t* root){(void)root;return 1000000;}
static int book_store_roots(book_store_root_t out[2],int* n){*n=test_root_count;memcpy(out,test_roots,sizeof(test_roots));return 0;}
typedef struct {char path[288];book_progress_t value;bool used;} test_prog_entry_t;
static test_prog_entry_t test_progs[8];
static void test_prog_set(const char* path,uint32_t size,uint8_t pct,uint32_t seq){
    for(int i=0;i<8;i++){if(test_progs[i].used&&!strcmp(test_progs[i].path,path)){test_progs[i].value=(book_progress_t){.file_size=size,.px=48,.pct=pct,.last_open_s=seq};return;}}
    for(int i=0;i<8;i++){if(!test_progs[i].used){test_progs[i].used=true;snprintf(test_progs[i].path,sizeof(test_progs[i].path),"%s",path);test_progs[i].value=(book_progress_t){.file_size=size,.px=48,.pct=pct,.last_open_s=seq};return;}}
    assert(false);
}
static void test_prog_clear(const char* path){for(int i=0;i<8;i++)if(test_progs[i].used&&!strcmp(test_progs[i].path,path))test_progs[i].used=false;}
static bool book_progress_load(const char* p,uint32_t n,book_progress_t* out){
    for(int i=0;i<8;i++)if(test_progs[i].used&&!strcmp(test_progs[i].path,p)&&test_progs[i].value.file_size==n){*out=test_progs[i].value;return true;}
    *out=(book_progress_t){0};return false;
}
static void loading(app_ctx_t* ctx,const char* text){(void)ctx;(void)text;}
typedef struct pending_progress {char path[288];book_progress_t value;bool dirty,progress_saved;book_progress_watch_t* watch;struct pending_progress* next;} pending_progress_t;
static pending_progress_t* s_pending;
typedef struct delete_retry {shelf_entry_t entry;struct delete_retry* next;} delete_retry_t;
static delete_retry_t* s_delete_retries;
static char s_latest_path[288],test_last_path[288];
static bool s_save_failed;
static char s_path[288], s_resume_path[288];
#define BOOK_STORE_PATH_MAX 288
static unsigned test_open_calls;
static char test_open_path[288];
static bool open_book(app_ctx_t* ctx,const char* path){(void)ctx;test_open_calls++;strcpy(test_open_path,path);return true;}
static char* s_text;
static int s_unsaved,s_px=48;
static uint32_t s_file_size=1000;
static size_t s_chapter,s_page;
typedef struct {char* image_src;bool image_repeated;uint16_t image_first_chapter;bool image_title;} blk_t;
typedef struct {size_t bookmark_count,excerpt_count;} book_notes_t;
static book_notes_t s_notes;
static blk_t* s_blocks;
static size_t s_block_count;
static bool s_image_open;
static uint8_t* s_image_pixels;
static uint16_t s_image_width,s_image_height;
static size_t s_image_block=SIZE_MAX;
static const char* s_image_error;
static char s_image_origin[224];
static int test_image_loads,test_image_error;
static int book_chapter_load_image(size_t chapter,const char* ref,uint8_t** pixels,uint16_t* width,uint16_t* height){(void)chapter;assert(ref);++test_image_loads;*pixels=NULL;*width=*height=0;if(test_image_error)return test_image_error;*pixels=malloc(4);assert(*pixels);*width=*height=2;return ESP_OK;}
static int book_chapter_title(size_t chapter,char* out,size_t cap){snprintf(out,cap,"Fixture chapter %zu",chapter);return ESP_OK;}
static void lock_draw(void){}
static void unlock_draw(void){}
static void loading_detail(app_ctx_t* ctx,const char* text,const char* detail){(void)ctx;(void)text;(void)detail;}
static void free_image(void);
static int test_save_error,test_last_error,test_save_calls,test_last_calls;
static size_t book_layout_page_count(void){return 3;}
static size_t book_layout_page_start_offset(size_t page){return page*100;}
static unsigned percent(size_t page){return (unsigned)page*20;}
static int book_progress_save(const char* p,const book_progress_t* value){(void)p;(void)value;test_save_calls++;return test_save_error;}
static int book_progress_set_last_path(const char* p){test_last_calls++;if(!test_last_error)snprintf(test_last_path,sizeof(test_last_path),"%s",p);return test_last_error;}
static bool book_progress_last_path(char* out,size_t cap){if(!out||!cap||!test_last_path[0]){if(out&&cap)out[0]=0;return false;}snprintf(out,cap,"%s",test_last_path);return true;}
static void save_progress(void);
static void invalidate_prep(void){}
static shelf_entry_t s_managed;
static bool s_delete_confirm,s_file_removed,s_clear_confirm;
static char s_manage_message[128];
typedef enum {SHELF,READING,TOC,MANAGE,BULK,SEARCH,BOOKMARKS,EXCERPTS,TYPOGRAPHY,CHAPTER_RESULTS} book_view_t;
static book_view_t s_view,s_search_parent;
#define UI_KEY_1 1
#define UI_KEY_2 2
#define UI_KEY_3 3
static int s_pressed_control;
static bool s_scan_pending,s_toolbar;
static bool s_resume_pending,s_full,s_shake_enabled;
static bool s_chapter_search;
static size_t s_chapter_matches[BOOK_SEARCH_RESULTS],s_chapter_match_count,s_text_len;
static bool s_note_delete_confirm,s_note_delete_index_valid,s_reader_landscape,s_selecting,s_cbz_mode;
static size_t s_sel_start,s_sel_end,s_cbz_page;
static int s_sel_line_y;
static size_t book_comic_page_count(void){return 0;}
#define BOOK_KIND_CBZ 3
#define MODE_DU 1
#define MODE_GL16 2
static int book_kind(void){return 0;}
static bool book_layout_hit_text(size_t p,EpdRect r,int x,int y,size_t* off,int* ly){(void)p;(void)r;(void)x;(void)y;if(off)*off=0;if(ly)*ly=100;return true;}
static bool book_layout_sentence_bounds(const char* t,size_t l,size_t o,size_t a,size_t b,size_t* s,size_t* e){(void)t;(void)l;(void)o;(void)a;(void)b;if(s)*s=0;if(e)*e=1;return true;}
static app_redraw_t paint_reading(app_ctx_t* ctx,int mode){(void)ctx;(void)mode;return APP_REDRAW_AREA;}
static bool save_selected_excerpt(size_t a,size_t b){(void)a;(void)b;return true;}
static int64_t s_reading_tick_ms;
static uint32_t s_reading_acc_ms;
static int s_du_count;
static int64_t s_size_settle_ms,s_poll_ms;
static void* s_prep_task,*s_prep_done,*s_draw_lock,*s_next_fb;
static uint8_t* s_cover_thumb;
#define BOOK_COVER_THUMB_BYTES (144 * 196 / 2)
static bool test_cover_hit;
static unsigned test_cover_blits, test_cover_paperbacks;
static bool book_cover_load_thumb(const char* path,uint32_t size,uint8_t out[BOOK_COVER_THUMB_BYTES]){(void)path;(void)size;if(!test_cover_hit)return false;memset(out,0xAA,BOOK_COVER_THUMB_BYTES);return true;}
static void blit_cover_4bpp_scaled(uint8_t* fb,const uint8_t* thumb,EpdRect dst){(void)fb;(void)dst;assert(thumb==s_cover_thumb&&thumb[0]==0xAA);++test_cover_blits;}
static void draw_paperback_cover(uint8_t* fb,EpdRect box,const char* name){(void)fb;(void)box;(void)name;++test_cover_paperbacks;}
static void ensure_prep(void){}
static int app_settings_book_px(void){return 48;}
static void read_pico_sd_start_probe(void){}
static void sensor_set(app_ctx_t* ctx,bool on){(void)ctx;(void)on;}
static bool reader_sensor_required(void){return false;}
static int app_settings_book_rotation(void){return 0;}
static int app_settings_book_gesture(void){return 0;}
static void reading_stats_set_last_book(const char* name,uint8_t pct){(void)name;(void)pct;}
static void reading_stats_add_seconds(uint32_t seconds){(void)seconds;}
static void reader_rotation_end(void){}
static void reader_touch_map(uint16_t* x,uint16_t* y){(void)x;(void)y;}
static size_t book_text_search(const char* text,size_t len,const char* query,size_t from,size_t* offsets,size_t cap){(void)text;(void)len;(void)query;(void)from;(void)offsets;(void)cap;return 0;}
static void vTaskDelete(void* p){(void)p;}
static void vSemaphoreDelete(void* p){(void)p;}
static unsigned test_turns;
static app_redraw_t turn_page(app_ctx_t* ctx,int dir){(void)ctx;(void)dir;++test_turns;return APP_REDRAW_NONE;}
typedef enum {UI_GESTURE_PRESS,UI_GESTURE_TAP,UI_GESTURE_LONG_PRESS,UI_GESTURE_SWIPE_L,UI_GESTURE_SWIPE_R,UI_GESTURE_SWIPE_U,UI_GESTURE_SWIPE_D,UI_GESTURE_CANCEL} ui_gesture_type_t;
typedef struct {ui_gesture_type_t type;uint16_t x0,y0,x,y;} ui_gesture_event_t;
static EpdRect body_rect(void){return(EpdRect){40,24,604,1000};}
static int control_at(app_ctx_t* ctx,uint16_t x,uint16_t y,EpdRect* rect){(void)ctx;*rect=(EpdRect){48,100,588,88};return ui_rect_hit(*rect,x,y)?1000:-1;}
static app_redraw_t open_image(app_ctx_t* ctx,size_t block);
static app_redraw_t action_at(app_ctx_t* ctx,uint16_t x,uint16_t y){(void)x;(void)y;return open_image(ctx,0);}
static app_redraw_t paint_control(app_ctx_t* ctx,EpdRect rect){(void)ctx;(void)rect;return APP_REDRAW_AREA;}
static int test_delete_error,test_forget_error,test_notify_count,test_delete_calls;
static bool test_removed,test_mixed;
static int book_store_delete(const char* path,bool* removed){(void)path;test_delete_calls++;*removed=test_removed;return test_mixed ? (strstr(path,"book001") ? -1 : 0) : test_delete_error;}
static int book_progress_forget(const char* path){for(book_progress_watch_t* w=test_watches;w;w=w->next)if(!strcmp(w->path,path))atomic_store(&w->invalidated,true);if(!test_forget_error){test_prog_clear(path);if(!strcmp(test_last_path,path))test_last_path[0]=0;}return test_forget_error;}
static void book_store_notify_changed(void){test_notify_count++;}
static unsigned book_store_revision(void){return (unsigned)test_notify_count;}
static void free_book(void){s_text=NULL;s_path[0]=0;free_image();}
static char test_wrapped[512];
#define UI_PX_CAPTION 28
#define UI_MARGIN 40
#define EPD_DRAW_ALIGN_LEFT 0
static int ui_content_width(void){return 604;}
static int ttf_text_width_px(int px,const char* text){int width=0;for(;*text;text++)if(((unsigned char)*text&0xc0)!=0x80)width+=px;return width;}
static void ui_text(uint8_t* fb,int x,int y,int px,const char* text,int align,bool inv){(void)fb;(void)x;(void)y;(void)px;(void)align;(void)inv;assert(strlen(test_wrapped)+strlen(text)<sizeof(test_wrapped));strcat(test_wrapped,text);}
'''
for name in ("copy_text", "shelf_matches", "compare_books", "pending_find", "pending_reserve", "pending_restore", "pending_discard", "pending_mark_latest", "pending_drop_invalidated", "pending_flush", "resolve_recent_index", "sort_shelf", "shelf_reserve", "delete_retry_find", "delete_retry_reserve", "delete_retry_discard", "scan_shelf",
             "save_progress", "retry_progress", "layout_name", "manage_panel", "manage_rect", "batch_rect", "shelf_is_grid", "shelf_per_page", "leaves", "selected_count", "clear_selection", "toggle_selection", "select_page", "search_keys", "search_begin", "refresh_search_matches", "search_finish", "search_action", "refresh_capacity", "manage_apply", "manage_action", "batch_apply", "batch_action", "resume_choice", "free_image", "close_image", "open_image", "draw_book_cover_box", "gesture_event", "on_key", "draw_wrapped_name", "on_enter", "book_on_exit"):
    unit += function(name) + "\n"
unit += r'''
int main(void) {
    app_ctx_t image_ctx={0};
    blk_t image_blocks[]={{.image_src="first.png",.image_repeated=true,.image_first_chapter=2},{.image_src="second.jpg"}};
    s_blocks=image_blocks;s_block_count=2;s_chapter=9;s_page=6;s_view=READING;
    ui_gesture_event_t image_event={.type=UI_GESTURE_PRESS,.x0=300,.y0=150,.x=300,.y=150};
    assert(gesture_event(&image_ctx,&image_event)==APP_REDRAW_NONE && test_image_loads==0);
    image_event.type=UI_GESTURE_SWIPE_L;image_event.x=236;
    gesture_event(&image_ctx,&image_event);assert(test_turns==1 && test_image_loads==0 && !s_image_open);
    image_event.type=UI_GESTURE_TAP;image_event.x=300;
    assert(gesture_event(&image_ctx,&image_event)==APP_REDRAW_PAGE && test_image_loads==1 && s_image_open);
    assert(s_image_pixels && strstr(s_image_origin,"第 3 节") && s_page==6 && s_chapter==9);
    image_event.type=UI_GESTURE_SWIPE_L;image_event.x=236;
    assert(gesture_event(&image_ctx,&image_event)==APP_REDRAW_NONE && test_turns==1 && s_image_open);
    assert(on_key(&image_ctx,UI_KEY_3)==APP_REDRAW_NONE && s_image_open && test_turns==1 && s_page==6);
    assert(on_key(&image_ctx,UI_KEY_1)==APP_REDRAW_PAGE && !s_image_open && test_turns==1 && s_page==6);
    assert(open_image(&image_ctx,0)==APP_REDRAW_PAGE && test_image_loads==1);
    image_event.type=UI_GESTURE_TAP;image_event.x=300;
    assert(gesture_event(&image_ctx,&image_event)==APP_REDRAW_PAGE && !s_image_open && s_page==6);
    test_image_error=ESP_ERR_NOT_FOUND;
    assert(open_image(&image_ctx,1)==APP_REDRAW_PAGE && s_image_open && !s_image_pixels && s_image_error && test_image_loads==2);
    assert(on_key(&image_ctx,UI_KEY_1)==APP_REDRAW_PAGE && !s_image_open && test_turns==1);
    test_image_error=0;assert(open_image(&image_ctx,1)==APP_REDRAW_PAGE && s_image_pixels && test_image_loads==3);
    free_image();assert(!s_image_open && !s_image_pixels && s_image_block==SIZE_MAX);
    image_blocks[0].image_title=true;
    assert(open_image(&image_ctx,0)==APP_REDRAW_PAGE && !s_image_origin[0]);
    free_image();
    // 统一三键：正文无弹窗时 KEY1/KEY3 翻页并置 key_handled，弹窗或书架仅 KEY1 返回/取消。
    // Unified keys: clean reading pages with KEY1/KEY3 and sets key_handled; modals/shelf use KEY1 back/cancel.
    image_ctx.key_handled=false;
    assert(on_key(&image_ctx,UI_KEY_1)==APP_REDRAW_NONE && image_ctx.key_handled && test_turns==2);
    image_ctx.key_handled=false;
    assert(on_key(&image_ctx,UI_KEY_3)==APP_REDRAW_NONE && image_ctx.key_handled && test_turns==3);
    s_toolbar=true;
    assert(on_key(&image_ctx,UI_KEY_1)==APP_REDRAW_PAGE && !s_toolbar && test_turns==3);
    s_view=SHELF;
    app_ctx_t resume_ctx={0};
    strcpy(s_resume_path,"/sdcard/books/large.epub");
    assert(on_key(&resume_ctx,UI_KEY_1)==APP_REDRAW_PAGE && test_open_calls==0 && !s_resume_path[0]);
    strcpy(s_resume_path,"/sdcard/books/large.epub");
    assert(resume_choice(&resume_ctx,true)==APP_REDRAW_PAGE && test_open_calls==1 && !s_resume_path[0]);
    assert(!strcmp(test_open_path,"/sdcard/books/large.epub"));
    assert(resume_choice(&resume_ctx,true)==APP_REDRAW_NONE && test_open_calls==1);
    strcpy(s_resume_path,"/sdcard/books/large.epub");
    assert(on_key(&resume_ctx,UI_KEY_2)==APP_REDRAW_NONE && s_resume_path[0] && test_open_calls==1);
    s_resume_path[0]=0;

    shelf_entry_t a={.search_match=true,.name="Same.txt",.path="/sdcard/books/A/Same.txt"};
    shelf_entry_t b={.search_match=true,.name="Same.txt",.path="/sdcard/books/B/Same.txt"};
    assert(compare_books(&a,&b)<0);
    b.recent=10;s_recent_sort=true;assert(compare_books(&a,&b)>0);
    b.is_flash=true;s_filter=1;assert(compare_books(&a,&b)<0);
    s_filter=2;assert(compare_books(&a,&b)>0);
    s_filter=0;s_recent_sort=false;
    snprintf(test_roots[0].path,sizeof(test_roots[0].path),"/tmp/book-ui-%d",(int)getpid());
    assert(mkdir(test_roots[0].path,0700)==0);
    for(int i=0;i<65;i++){char path[340];snprintf(path,sizeof(path),"%s/book%03d.txt",test_roots[0].path,i);FILE* f=fopen(path,"w");assert(f);fputs("x",f);fclose(f);}
    app_ctx_t ctx={0};scan_shelf(&ctx);
    assert(s_count==65&&s_visible_count==65&&s_shelf_capacity>=65);
    ctx.leaf=3;s_view=MANAGE;s_clear_confirm=false;s_file_removed=false;
    EpdRect back=manage_rect(0,3);manage_action(&ctx,back.x+1,back.y+1);
    assert(s_view==SHELF&&ctx.leaf==3);
    assert(!strcmp(s_shelf[0].name,"book000.txt")&&!strcmp(s_shelf[64].name,"book064.txt"));
    test_degraded=true;scan_shelf(&ctx);assert(s_count==65&&s_shelf_warning[0]);test_degraded=false;
    test_root_count=2;strcpy(test_roots[1].path,"/nonexistent-book-root");scan_shelf(&ctx);assert(s_count==65&&s_shelf_warning[0]);test_root_count=1;
    free(s_shelf);s_shelf=NULL;s_shelf_capacity=0;s_count=0;test_oom=true;scan_shelf(&ctx);assert(s_count==0&&s_shelf_warning[0]);test_oom=false;
    strcpy(s_path,"/sdcard/books/a.txt");s_text="text";s_page=1;s_unsaved=8;
    assert(pending_reserve(s_path));pending_mark_latest(s_path);test_save_error=-1;save_progress();
    assert(s_unsaved==8&&s_save_failed&&pending_find(s_path)->dirty);
    book_progress_t restored={0};assert(pending_reserve("/sdcard/books/b.txt"));pending_mark_latest("/sdcard/books/b.txt");
    assert(pending_restore(s_path,s_file_size,&restored)&&restored.byte_off==100);
    assert(!pending_restore(s_path,s_file_size+1,&restored));pending_mark_latest(s_path);
    test_save_error=0;test_last_error=-1;save_progress();assert(s_unsaved==8&&pending_find(s_path)->progress_saved);
    int calls=test_save_calls;save_progress();assert(test_save_calls==calls&&s_unsaved==8);
    test_last_error=0;retry_progress();assert(!s_unsaved&&!s_save_failed);
    s_unsaved=7;retry_progress();assert(!s_unsaved);
    pending_progress_t* earlier=pending_find(s_path);earlier->dirty=true;earlier->progress_saved=false;
    assert(pending_reserve("/sdcard/books/b.txt"));pending_progress_t* newer=pending_find("/sdcard/books/b.txt");newer->dirty=true;
    pending_mark_latest("/sdcard/books/b.txt");pending_mark_latest(s_path);
    s_text=NULL;retry_progress();assert(!strcmp(test_last_path,s_path)&&!s_save_failed);
    strcpy(s_managed.path,s_path);s_view=MANAGE;s_delete_confirm=true;s_clear_confirm=true;
    EpdRect cancel=manage_rect(0,2);manage_action(&ctx,cancel.x+1,cancel.y+1);assert(!s_clear_confirm&&test_delete_calls==0);
    EpdRect del=manage_rect(2,3);manage_action(&ctx,del.x+1,del.y+1);assert(s_clear_confirm&&s_delete_confirm&&test_delete_calls==0);
    test_removed=false;test_delete_error=-1;manage_apply(&ctx);assert(s_view==MANAGE&&!s_file_removed&&s_manage_message[0]);
    test_removed=true;manage_apply(&ctx);assert(s_view==MANAGE&&s_file_removed&&test_notify_count==1&&!pending_find(s_managed.path));assert(s_store_revision==book_store_revision());
    test_forget_error=-1;manage_apply(&ctx);assert(s_view==MANAGE&&test_notify_count==1);
    test_forget_error=0;manage_apply(&ctx);assert(s_view==SHELF&&test_notify_count==1);
    strcpy(s_managed.name,"Short.txt");assert(manage_panel().height<500);
    char long_name[256];memset(long_name,'W',255);long_name[255]=0;test_wrapped[0]=0;
    assert(draw_wrapped_name(NULL,long_name,176)+104<870);assert(!strcmp(test_wrapped,long_name));
    for(int i=0;i<80;i++){memcpy(long_name+i*3,"书",3);}long_name[240]=0;test_wrapped[0]=0;
    assert(draw_wrapped_name(NULL,long_name,176)+104<870);assert(!strcmp(test_wrapped,long_name));
    scan_shelf(&ctx);s_view=BULK;toggle_selection(0);toggle_selection(14);assert(selected_count()==2);
    s_recent_sort=true;sort_shelf(&ctx);assert(selected_count()==2);select_page(1);assert(selected_count()==9);
    ctx.leaf=2;strcpy(s_query,"book");search_begin();strcpy(s_search_draft,"bad");search_finish(&ctx,false);
    assert(ctx.leaf==2&&!strcmp(s_query,"book")&&selected_count()==9);
    search_begin();assert(on_key(&ctx,UI_KEY_3)==APP_REDRAW_NONE&&ctx.leaf==2);assert(on_key(&ctx,UI_KEY_1)==APP_REDRAW_PAGE&&ctx.leaf==2&&s_view==BULK);
    s_batch_confirm=true;assert(on_key(&ctx,UI_KEY_3)==APP_REDRAW_NONE&&ctx.leaf==2);assert(on_key(&ctx,UI_KEY_1)==APP_REDRAW_PAGE&&!s_batch_confirm&&ctx.leaf==2);
    search_begin();strcpy(s_search_draft,"book001");search_finish(&ctx,true);assert(!selected_count()&&s_visible_count==1);
    search_begin();memset(s_search_draft,'x',64);s_search_draft[64]=0;search_action(&ctx,0);assert(strlen(s_search_draft)==64);search_action(&ctx,41);assert(strlen(s_search_draft)==63);search_action(&ctx,42);assert(!s_search_draft[0]);search_action(&ctx,43);
    strcpy(s_query,"");refresh_search_matches();sort_shelf(&ctx);clear_selection();s_view=BULK;toggle_selection(0);toggle_selection(1);
    s_batch_confirm=true;calls=test_delete_calls;EpdRect bc=ui_row_rect(0,2,620,UI_BTN_H);batch_action(&ctx,bc.x+1,bc.y+1);assert(!s_batch_confirm&&test_delete_calls==calls);
    s_batch_delete=true;test_removed=true;test_delete_error=-1;batch_apply(&ctx);assert(selected_count()==2);
    calls=test_delete_calls;test_forget_error=0;batch_apply(&ctx);assert(!selected_count()&&test_delete_calls==calls&&s_count==63);
    scan_shelf(&ctx);clear_selection();toggle_selection(0);toggle_selection(1);test_mixed=true;
    batch_apply(&ctx);assert(s_count==64&&selected_count()==1&&s_shelf[0].removed);
    calls=test_delete_calls;batch_apply(&ctx);assert(s_count==63&&!selected_count()&&test_delete_calls==calls);
    // 保存失败的 A 不应因无关 B 变动而丢失。/ An unrelated B change must retain A's failed save.
    strcpy(s_path,"/sdcard/books/a.txt");s_text="text";s_page=2;s_unsaved=8;
    assert(pending_reserve(s_path));pending_mark_latest(s_path);test_save_error=-1;save_progress();
    book_on_exit(&ctx);assert(book_progress_forget("/sdcard/books/b.txt")==ESP_OK);book_store_notify_changed();on_enter(&ctx);
    assert(pending_restore("/sdcard/books/a.txt",s_file_size,&restored)&&restored.byte_off==200);
    test_save_error=0;calls=test_save_calls;retry_progress();assert(test_save_calls==calls+1&&!s_save_failed);
    // 同路径替换即使 NVS 清理失败，也不能回写旧进度。/ Replacing A must not restore stale progress even when cleanup fails.
    strcpy(s_path,"/sdcard/books/a.txt");s_text="text";s_unsaved=8;assert(pending_reserve(s_path));
    pending_mark_latest(s_path);test_save_error=-1;save_progress();book_on_exit(&ctx);
    test_forget_error=-1;assert(book_progress_forget("/sdcard/books/a.txt")!=ESP_OK);book_store_notify_changed();
    on_enter(&ctx);assert(!pending_find("/sdcard/books/a.txt"));test_save_error=0;
    calls=test_save_calls;retry_progress();assert(test_save_calls==calls);test_forget_error=0;
    // 文件删除后清理失败，切页回来仍可重试。/ Cleanup after deletion remains retryable across page exits.
    scan_shelf(&ctx);s_managed=s_shelf[0];s_view=MANAGE;s_delete_confirm=true;s_file_removed=false;
    test_mixed=false;test_removed=true;test_delete_error=-1;manage_apply(&ctx);
    assert(s_file_removed);assert(unlink(s_managed.path)==0);
    book_on_exit(&ctx);on_enter(&ctx);scan_shelf(&ctx);
    bool found_retry=false;
    for(int i=0;i<s_count;i++)if(!strcmp(s_shelf[i].path,s_managed.path)){found_retry=s_shelf[i].removed;}
    assert(found_retry);
    s_view=MANAGE;calls=test_delete_calls;test_forget_error=0;manage_apply(&ctx);
    assert(s_view==SHELF&&test_delete_calls==calls);
    // 批量失败记录同样跨页存活，重试仅清元数据。/ Batch cleanup retries also survive exits without repeating unlink.
    scan_shelf(&ctx);clear_selection();toggle_selection(0);toggle_selection(1);s_batch_delete=true;
    batch_apply(&ctx);assert(selected_count()==2&&s_delete_retries);
    for(int i=0;i<s_count;i++)if(s_shelf[i].removed)assert(unlink(s_shelf[i].path)==0);
    book_on_exit(&ctx);on_enter(&ctx);scan_shelf(&ctx);
    int retry_count=0;
    for(int i=0;i<s_count;i++)if(s_shelf[i].removed){s_shelf[i].selected=true;++retry_count;}
    assert(retry_count==2);calls=test_delete_calls;batch_apply(&ctx);
    assert(!s_delete_retries&&test_delete_calls==calls&&!selected_count());
    // 封面缩略图复用堆缓冲并在退页释放，OOM 时回退纸书封。
    // Cover thumbnail reuses a heap buffer freed on exit and falls back to paperback on OOM.
    shelf_entry_t cov_item = {.name = "demo.epub", .path = "/sdcard/books/demo.epub", .size = 123};
    EpdRect cov_box = {40, 264, 144, 196};
    test_oom = true; test_cover_hit = true;
    draw_book_cover_box(NULL, cov_box, &cov_item);
    assert(!s_cover_thumb && test_cover_blits == 0 && test_cover_paperbacks == 1);
    test_oom = false;
    draw_book_cover_box(NULL, cov_box, &cov_item);
    assert(s_cover_thumb && test_cover_blits == 1 && test_cover_paperbacks == 1);
    test_cover_hit = false;
    draw_book_cover_box(NULL, cov_box, &cov_item);
    assert(s_cover_thumb && test_cover_blits == 1 && test_cover_paperbacks == 2);
    book_on_exit(&ctx);
    assert(!s_cover_thumb);

    // 最近阅读卡片回归：读完一本书（100%）返回书架重扫、切换书籍、删除/清进度后绝不跳到未打开的 0% 书籍。
    // Recent-card regressions: finishing a book (100%), rescanning, switching, deleting, or clearing progress must never pick an unopened 0% book.
    memset(test_progs, 0, sizeof(test_progs));
    test_last_path[0] = 0;
    s_latest_path[0] = 0;
    while (s_pending) pending_discard(s_pending->path);
    char book_a[340], book_b[340], book_c[340];
    snprintf(book_a, sizeof(book_a), "%s/book005.txt", test_roots[0].path);
    snprintf(book_b, sizeof(book_b), "%s/book010.txt", test_roots[0].path);
    snprintf(book_c, sizeof(book_c), "%s/book020.txt", test_roots[0].path);
    // 1. 此前 s_recent_index 指向旧下标，读完 book010 (100%) 后 scan_shelf 仍准确锁定 book010 (100%)。
    // With a prior s_recent_index, finishing book010 (100%) and calling scan_shelf resolves to book010 (100%).
    test_prog_set(book_b, 1, 100, 7);
    strcpy(test_last_path, book_b);
    s_recent_index = 25;
    s_recent_sort = false;
    s_filter = 0;
    s_query[0] = 0;
    scan_shelf(&ctx);
    assert(s_recent_index >= 0 && s_recent_index < s_count);
    assert(!strcmp(s_shelf[s_recent_index].path, book_b));
    assert(s_shelf[s_recent_index].has_progress && s_shelf[s_recent_index].pct == 100);
    // 2. 切去读完 book020 (100%) 后返回书架，最近阅读更新为 book020 (100%) 而非旧书或 0% 书。
    // Finishing book020 (100%) and rescanning updates the recent card to book020 (100%).
    test_prog_set(book_c, 1, 100, 8);
    strcpy(test_last_path, book_c);
    scan_shelf(&ctx);
    assert(s_recent_index >= 0 && !strcmp(s_shelf[s_recent_index].path, book_c));
    assert(s_shelf[s_recent_index].has_progress && s_shelf[s_recent_index].pct == 100);
    // 3. 删除位于最近阅读之前的无关书籍 book005，最近阅读仍指向 book020，不会下标左移落到 0% 书。
    // Deleting an earlier unrelated book keeps recent card on book020 instead of shifting onto a 0% neighbor.
    int idx_a = -1;
    for (int i = 0; i < s_count; ++i) if (!strcmp(s_shelf[i].path, book_a)) { idx_a = i; break; }
    assert(idx_a >= 0 && idx_a < s_recent_index);
    s_managed = s_shelf[idx_a];
    s_view = MANAGE; s_delete_confirm = true; s_file_removed = false; s_clear_confirm = true;
    test_delete_error = 0; test_removed = true; test_forget_error = 0;
    manage_apply(&ctx);
    assert(s_view == SHELF && s_recent_index >= 0 && !strcmp(s_shelf[s_recent_index].path, book_c));
    assert(s_shelf[s_recent_index].has_progress && s_shelf[s_recent_index].pct == 100);
    // 4. 清除最近阅读书籍 book020 的进度后，最近阅读卡片清空（s_recent_index == -1），绝不显示 0% 书。
    // Clearing progress on book020 clears s_recent_index (-1) rather than showing a 0% book.
    s_managed = s_shelf[s_recent_index];
    s_view = MANAGE; s_delete_confirm = false; s_file_removed = false; s_clear_confirm = true;
    manage_apply(&ctx);
    assert(s_view == SHELF && s_recent_index == -1);
    // 5. 批量删除最近阅读书籍 book010 后，s_recent_index 归 -1，不落到相邻未打开的 0% 书。
    // Bulk-deleting the recent book resets s_recent_index to -1 instead of shifting onto an unopened 0% book.
    strcpy(test_last_path, book_b);
    scan_shelf(&ctx);
    assert(s_recent_index >= 0 && !strcmp(s_shelf[s_recent_index].path, book_b));
    s_view = BULK; clear_selection(); toggle_selection(s_recent_index);
    s_batch_delete = true; s_batch_confirm = true;
    batch_apply(&ctx);
    assert(s_recent_index == -1);
    // 6. 瞬时保存失败（pending dirty）时返回书架，仍显示脏进度书籍而不显示 0% 书。
    // On transient NVS save failure (dirty pending progress), scan_shelf resolves the dirty book with its pct.
    test_last_path[0] = 0;
    assert(pending_reserve(book_c));
    pending_progress_t* dirty_c = pending_find(book_c);
    dirty_c->value = (book_progress_t){.file_size = 1, .px = 48, .pct = 100};
    dirty_c->dirty = true;
    pending_mark_latest(book_c);
    scan_shelf(&ctx);
    assert(s_recent_index >= 0 && !strcmp(s_shelf[s_recent_index].path, book_c));
    assert(s_shelf[s_recent_index].has_progress && s_shelf[s_recent_index].pct == 100);
    pending_discard(book_c);
    // 7. 持久 last 指向无进度/已删书籍时，resolve_recent_index(true) 清理过期 last 并置 -1。
    // Stale NVS last path with no progress is pruned by resolve_recent_index(true).
    strcpy(test_last_path, book_c);
    scan_shelf(&ctx);
    assert(s_recent_index == -1 && test_last_path[0] != 0);
    resolve_recent_index(true);
    assert(s_recent_index == -1 && test_last_path[0] == 0);

    free(s_shelf);
    while(s_pending)pending_discard(s_pending->path);
    for(int i=0;i<65;i++){char path[340];snprintf(path,sizeof(path),"%s/book%03d.txt",test_roots[0].path,i);unlink(path);}rmdir(test_roots[0].path);
    puts("book_ui_host_test: PASS");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / "test.c"
    exe = Path(tmp) / "test"
    obj = Path(tmp) / "frame_check.o"
    c.write_text(unit, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-fsanitize=address,undefined", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    # 防止 draw_book_cover_box 再次在栈上分配 14 KB 缩略图缓冲导致主任务栈溢出。
    # Guard against placing the 14 KB cover thumbnail buffer back on the main task stack.
    frame_unit = (
        "#include <stdbool.h>\n#include <stdint.h>\n#include <stdlib.h>\n"
        "#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n"
        "#define BOOK_COVER_THUMB_BYTES (144 * 196 / 2)\n"
        "typedef struct { int x, y, width, height; } EpdRect;\n"
        "typedef struct { char name[256]; char path[288]; uint32_t size; bool removed; } shelf_entry_t;\n"
        "static uint8_t* s_cover_thumb;\n"
        "void* heap_caps_malloc(size_t n, int caps);\n"
        "bool book_cover_load_thumb(const char* p, uint32_t s, uint8_t out[BOOK_COVER_THUMB_BYTES]);\n"
        "void blit_cover_4bpp_scaled(uint8_t* fb, const uint8_t* thumb, EpdRect dst);\n"
        "void draw_paperback_cover(uint8_t* fb, EpdRect box, const char* name);\n"
        + function("draw_book_cover_box")
        + "\nvoid cover_entry(uint8_t* fb, EpdRect box, const shelf_entry_t* item) { draw_book_cover_box(fb, box, item); }\n"
    )
    frame_c = Path(tmp) / "frame_check.c"
    frame_c.write_text(frame_unit, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wframe-larger-than=1024", "-c", str(frame_c), "-o", str(obj)], check=True)

    # 验证书架提示条不与书籍/最近阅读卡片重合，且单页/首末页翻页按钮不可点击、不触发重刷。
    # Verify shelf notices never overlap books/recent card and inactive paging buttons are non-clickable without redraw.
    import re
    src_text = source.read_text(encoding="utf-8")
    shelf_defines = "\n".join(re.findall(r"^#define (?:SHELF_[A-Z0-9_]+|BOOK_TYPO_COUNT) [0-9]+$", src_text, re.M))
    assert "SHELF_NOTICE_Y" in shelf_defines and "BOOK_TYPO_COUNT" in shelf_defines
    assert "run_wavefront_turn" not in src_text and "app_settings_book_anim" not in src_text
    shelf_unit = r'''
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define UI_MARGIN 40
#define UI_GAP 12
#define UI_BTN_H 84
#define UI_PX_CAPTION 28
#define UI_CONTENT_TOP 176
#define UI_CONTENT_BOTTOM 1056
#define UI_BAR_TOP 1072
#define UI_BAR_H 84
#define BOOK_ROWS 7
#define BOOK_GRID_ROWS 6
#define BOOK_TOC_ROWS 10
#define BOOK_NOTE_ROWS 8
#define BOOK_TOOL_COUNT 12
#define BOOK_SEARCH_RESULTS 7
#define BOOK_STORE_PATH_MAX 288
#define BOOK_KIND_CBZ 3
#define MODE_DU 1
#define MODE_GL16 2
typedef struct { int x, y, width, height; } EpdRect;
typedef struct { int leaf; bool request_menu; bool key_handled; int64_t now_ms; } app_ctx_t;
typedef enum { APP_REDRAW_PAGE, APP_REDRAW_AREA, APP_REDRAW_NONE, APP_REDRAW_FULL } app_redraw_t;
typedef enum { SHELF, READING, TOC, MANAGE, BULK, SEARCH, BOOKMARKS, EXCERPTS, TYPOGRAPHY, CHAPTER_RESULTS } book_view_t;
typedef enum { UI_GESTURE_PRESS, UI_GESTURE_TAP, UI_GESTURE_LONG_PRESS, UI_GESTURE_SWIPE_L, UI_GESTURE_SWIPE_R, UI_GESTURE_SWIPE_U, UI_GESTURE_SWIPE_D, UI_GESTURE_CANCEL } ui_gesture_type_t;
typedef struct { ui_gesture_type_t type; uint16_t x0, y0, x, y; } ui_gesture_event_t;
typedef struct { char name[256], path[BOOK_STORE_PATH_MAX]; uint32_t size; bool is_flash, has_progress; uint8_t pct; uint32_t recent; bool selected, removed, search_match; } shelf_entry_t;
typedef struct { size_t bookmark_count, excerpt_count; } book_notes_t;
static int ui_content_width(void) { return 684 - 2 * UI_MARGIN; }
static EpdRect ui_grid_rect(int col, int cols, int row, int y0, int h) {
    int w = (ui_content_width() - (cols - 1) * UI_GAP) / cols;
    return (EpdRect){ UI_MARGIN + col * (w + UI_GAP), y0 + row * (h + UI_GAP), w, h };
}
static EpdRect ui_row_rect(int col, int cols, int y0, int h) { return ui_grid_rect(col, cols, 0, y0, h); }
static EpdRect ui_bar_rect(int col, int cols) {
    int w = (ui_content_width() - (cols - 1) * UI_GAP) / cols;
    return (EpdRect){ UI_MARGIN + col * (w + UI_GAP), UI_BAR_TOP, w, UI_BAR_H };
}
static int ui_bar_hit(uint16_t x, uint16_t y, int cols) {
    for (int i = 0; i < cols; ++i) {
        EpdRect r = ui_bar_rect(i, cols);
        if ((int)x >= r.x && (int)x < r.x + r.width && (int)y >= r.y && (int)y < r.y + r.height) return i;
    }
    return -1;
}
static bool ui_rect_hit(EpdRect r, uint16_t x, uint16_t y) {
    return (int)x >= r.x && (int)x < r.x + r.width && (int)y >= r.y && (int)y < r.y + r.height;
}
static book_view_t s_view;
static shelf_entry_t s_entries[16];
static shelf_entry_t* s_shelf = s_entries;
static shelf_entry_t s_managed;
static int s_count, s_visible_count, s_recent_index = -1, s_pressed_control = -1;
static char s_message[128], s_shelf_warning[128], s_query[65], s_resume_path[BOOK_STORE_PATH_MAX], s_manage_message[128];
static bool s_batch_confirm, s_note_delete_confirm, s_clear_confirm, s_delete_confirm, s_file_removed, s_toolbar, s_save_failed;
static bool s_image_open, s_scan_pending, s_selecting;
static size_t s_chapter_match_count, s_page, s_text_len, s_sel_start, s_sel_end;
static int s_sel_line_y;
static char* s_text;
static book_notes_t s_notes;
static uint8_t test_shelf_view;
static unsigned test_paint_calls, test_action_calls;
static uint8_t app_settings_book_shelf_view(void) { return test_shelf_view; }
static size_t book_chapter_count(void) { return 1; }
static EpdRect search_rect(int i) { (void)i; return (EpdRect){0}; }
static EpdRect note_delete_rect(int i) { (void)i; return (EpdRect){0}; }
static EpdRect note_action_rect(int i) { (void)i; return (EpdRect){0}; }
static EpdRect batch_rect(int i) { (void)i; return (EpdRect){0}; }
static EpdRect manage_rect(int i, int c) { (void)i; (void)c; return (EpdRect){0}; }
static EpdRect tool_rect(int i) { (void)i; return (EpdRect){0}; }
static EpdRect body_rect(void) { return (EpdRect){40, 24, 604, 1000}; }
static void lock_draw(void) {}
static void unlock_draw(void) {}
static size_t book_layout_image_at(size_t p, EpdRect r, uint16_t x, uint16_t y, EpdRect* out) { (void)p; (void)r; (void)x; (void)y; (void)out; return SIZE_MAX; }
static app_redraw_t close_image(app_ctx_t* ctx) { (void)ctx; return APP_REDRAW_PAGE; }
static void reader_touch_map(uint16_t* x, uint16_t* y) { (void)x; (void)y; }
static int book_kind(void) { return 0; }
static bool book_layout_hit_text(size_t p, EpdRect r, uint16_t x, uint16_t y, size_t* o, int* ly) { (void)p; (void)r; (void)x; (void)y; (void)o; (void)ly; return false; }
static size_t book_layout_page_start_offset(size_t p) { (void)p; return 0; }
static size_t book_layout_page_count(void) { return 1; }
static bool book_layout_sentence_bounds(const char* t, size_t l, size_t o, size_t a, size_t b, size_t* s, size_t* e) { (void)t; (void)l; (void)o; (void)a; (void)b; (void)s; (void)e; return false; }
static void invalidate_prep(void) {}
static app_redraw_t paint_reading(app_ctx_t* ctx, int m) { (void)ctx; (void)m; return APP_REDRAW_AREA; }
static app_redraw_t turn_page(app_ctx_t* ctx, int d) { (void)ctx; (void)d; return APP_REDRAW_PAGE; }
static app_redraw_t paint_control(app_ctx_t* ctx, EpdRect rect) { (void)ctx; (void)rect; ++test_paint_calls; return APP_REDRAW_AREA; }
static app_redraw_t shelf_nav_action(app_ctx_t* ctx, int nav);
static app_redraw_t action_at(app_ctx_t* ctx, uint16_t x, uint16_t y) {
    ++test_action_calls;
    int nav = ui_bar_hit(x, y, 3);
    if (nav == 0 || nav == 2) return shelf_nav_action(ctx, nav);
    return APP_REDRAW_NONE;
}
static void copy_text(char* dst, size_t cap, const char* src) { snprintf(dst, cap, "%s", src); }
''' + shelf_defines + "\n"
    for name in ("row_rect", "typo_rect", "shelf_has_notice", "shelf_is_grid", "shelf_has_recent_card",
                 "recent_card_rect", "shelf_grid_rect", "shelf_list_rect", "shelf_per_page",
                 "leaves", "can_prev_leaf", "can_next_leaf", "shelf_nav_action",
                 "control_at", "gesture_event"):
        shelf_unit += function(name) + "\n"
    shelf_unit += r'''
int main(void) {
    app_ctx_t ctx = {0};
    s_view = SHELF;
    s_count = s_visible_count = 3;
    test_shelf_view = 0; // 网格模式 / Grid mode
    int notice_bottom = SHELF_NOTICE_Y + UI_PX_CAPTION;
    int footer_top = UI_CONTENT_BOTTOM - UI_PX_CAPTION;

    // 1. 无警告时保持原排版位置。/ Preserve normal layout coordinates without warnings.
    s_shelf_warning[0] = 0;
    s_message[0] = 0;
    assert(!shelf_has_notice());
    assert(recent_card_rect().y == SHELF_CONTENT_Y_PLAIN);
    assert(shelf_grid_rect(0, false).y == SHELF_GRID_Y_PLAIN);
    assert(shelf_grid_rect(0, true).y == 424);

    // 2. 存在扫描不可读警告时，首行网格/最近阅读卡片与警告文字不重合且不越过底栏说明。
    // With unreadable warning, first grid row and recent card stay below notice text and above footer hint.
    strcpy(s_shelf_warning, "部分目录或文件不可读，请检查后重扫");
    assert(shelf_has_notice());
    EpdRect g0 = shelf_grid_rect(0, false);
    EpdRect g5 = shelf_grid_rect(5, false);
    assert(g0.y >= notice_bottom + 12);
    assert(g5.y + g5.height <= footer_top - 12);

    s_recent_index = 0;
    assert(!shelf_has_recent_card(&ctx));
    s_entries[0].has_progress = true;
    assert(shelf_has_recent_card(&ctx));
    EpdRect rc = recent_card_rect();
    EpdRect gr0 = shelf_grid_rect(0, true);
    EpdRect gr5 = shelf_grid_rect(5, true);
    assert(rc.y >= notice_bottom + 12);
    assert(gr0.y >= rc.y + rc.height + UI_GAP);
    assert(gr5.y + gr5.height <= footer_top - 12);

    // 列表模式（含/不含最近阅读）同样不重合。/ List mode with or without recent card also stays clear.
    test_shelf_view = 1;
    EpdRect lr0 = shelf_list_rect(0, false, true);
    EpdRect lr4 = shelf_list_rect(4, false, true);
    assert(lr0.y >= rc.y + rc.height + UI_GAP);
    assert(lr4.y + lr4.height <= footer_top - 12);
    EpdRect ln0 = shelf_list_rect(0, false, false);
    assert(ln0.y >= notice_bottom + 12);
    test_shelf_view = 0;

    // 3. 单页书架：上一页与下一页均禁用，按下和点按都不触发重刷。
    // Single-page shelf: both prev and next are disabled; press and tap trigger zero redraws.
    assert(leaves() == 1 && !can_prev_leaf(&ctx) && !can_next_leaf(&ctx));
    EpdRect prev_btn = ui_bar_rect(0, 3);
    EpdRect mid_btn = ui_bar_rect(1, 3);
    EpdRect next_btn = ui_bar_rect(2, 3);
    EpdRect hit_r = {0};
    assert(control_at(&ctx, (uint16_t)(prev_btn.x + 10), (uint16_t)(prev_btn.y + 10), &hit_r) == -1);
    assert(control_at(&ctx, (uint16_t)(next_btn.x + 10), (uint16_t)(next_btn.y + 10), &hit_r) == -1);
    assert(control_at(&ctx, (uint16_t)(mid_btn.x + 10), (uint16_t)(mid_btn.y + 10), &hit_r) == 101);
    ui_gesture_event_t ev = { .type = UI_GESTURE_PRESS, .x0 = (uint16_t)(next_btn.x + 10), .y0 = (uint16_t)(next_btn.y + 10),
                              .x = (uint16_t)(next_btn.x + 10), .y = (uint16_t)(next_btn.y + 10) };
    assert(gesture_event(&ctx, &ev) == APP_REDRAW_NONE);
    ev.type = UI_GESTURE_TAP;
    assert(gesture_event(&ctx, &ev) == APP_REDRAW_NONE);
    assert(test_paint_calls == 0 && test_action_calls == 0 && ctx.leaf == 0);
    assert(shelf_nav_action(&ctx, 0) == APP_REDRAW_NONE && ctx.leaf == 0);
    assert(shelf_nav_action(&ctx, 2) == APP_REDRAW_NONE && ctx.leaf == 0);

    // 4. 多页书架：首页仅下一页可用，末页仅上一页可用。
    // Multi-page shelf: first page enables only next; last page enables only prev.
    s_count = s_visible_count = 8;
    assert(leaves() == 2 && !can_prev_leaf(&ctx) && can_next_leaf(&ctx));
    assert(control_at(&ctx, (uint16_t)(prev_btn.x + 10), (uint16_t)(prev_btn.y + 10), &hit_r) == -1);
    assert(control_at(&ctx, (uint16_t)(next_btn.x + 10), (uint16_t)(next_btn.y + 10), &hit_r) == 102);
    ev.type = UI_GESTURE_PRESS;
    assert(gesture_event(&ctx, &ev) == APP_REDRAW_AREA && test_paint_calls == 1);
    ev.type = UI_GESTURE_TAP;
    assert(gesture_event(&ctx, &ev) == APP_REDRAW_PAGE && ctx.leaf == 1);
    assert(can_prev_leaf(&ctx) && !can_next_leaf(&ctx));
    // 末页再点下一页不触发重刷。/ Clicking next on the last page triggers no redraw.
    test_paint_calls = 0; test_action_calls = 0;
    ev.type = UI_GESTURE_PRESS;
    assert(gesture_event(&ctx, &ev) == APP_REDRAW_NONE);
    ev.type = UI_GESTURE_TAP;
    assert(gesture_event(&ctx, &ev) == APP_REDRAW_NONE);
    assert(test_paint_calls == 0 && test_action_calls == 0 && ctx.leaf == 1);
    // 末页点上一页回到首页。/ Clicking prev on the last page returns to page 0.
    ev.x0 = ev.x = (uint16_t)(prev_btn.x + 10);
    ev.y0 = ev.y = (uint16_t)(prev_btn.y + 10);
    ev.type = UI_GESTURE_TAP;
    assert(gesture_event(&ctx, &ev) == APP_REDRAW_PAGE && ctx.leaf == 0);

    // 5. 排版与显示页共 7 项（已移除翻页动效），字重占满底行。
    // Typography page has 7 controls (turn animation removed), with weight spanning full width.
    s_view = TYPOGRAPHY;
    assert(BOOK_TYPO_COUNT == 7);
    EpdRect wrect = typo_rect(6);
    assert(wrect.width == ui_content_width());
    assert(control_at(&ctx, (uint16_t)(wrect.x + 20), (uint16_t)(wrect.y + 20), &hit_r) == 906);

    puts("book_ui_shelf_layout_and_paging_test: PASS");
}
'''
    shelf_c = Path(tmp) / "shelf_check.c"
    shelf_exe = Path(tmp) / "shelf_check"
    shelf_c.write_text(shelf_unit, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-variable", "-fsanitize=address,undefined", str(shelf_c), "-o", str(shelf_exe)], check=True)
    subprocess.run([str(shelf_exe)], check=True)

    # 验证首页页码指示当前页使用实心圆而非实心方块。
    # Verify home page indicator draws a filled black circle (not a square) for the current page.
    home_src = (Path(__file__).resolve().parents[1] / "main/apps/app_pico_home.c").read_text(encoding="utf-8")
    m = re.search(r"^static void draw_page_dots\([^\n]*\) \{", home_src, re.M)
    assert m, "draw_page_dots"
    at, depth = m.end(), 1
    while depth:
        if home_src[at] == "{": depth += 1
        elif home_src[at] == "}": depth -= 1
        at += 1
    dots_fn = home_src[m.start():at]
    home_unit = (
        "#include <assert.h>\n#include <stdint.h>\n#include <stdio.h>\n"
        "#define HOME_PAGES 3\n#define HOME_DOTS_Y 1076\n#define UI_GRAY_BLACK 0x00\n"
        "static int fill_circles, draw_circles, fill_x, fill_r;\n"
        "static int epd_rotated_display_width(void) { return 684; }\n"
        "static void epd_fill_circle(int x, int y, int r, uint8_t c, uint8_t* fb) { (void)fb; assert(y == HOME_DOTS_Y && c == UI_GRAY_BLACK); fill_circles++; fill_x = x; fill_r = r; }\n"
        "static void epd_draw_circle(int x, int y, int r, uint8_t c, uint8_t* fb) { (void)x; (void)fb; assert(y == HOME_DOTS_Y && r == 6 && c == UI_GRAY_BLACK); draw_circles++; }\n"
        + dots_fn
        + "\nint main(void) { draw_page_dots(NULL, 1); assert(fill_circles == 1 && draw_circles == 2 && fill_x == 342 && fill_r == 7); puts(\"home_page_dots_test: PASS\"); }\n"
    )
    home_c = Path(tmp) / "home_dots.c"
    home_exe = Path(tmp) / "home_dots"
    home_c.write_text(home_unit, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", str(home_c), "-o", str(home_exe)], check=True)
    subprocess.run([str(home_exe)], check=True)

    # 验证图书 GC16 翻页与全刷均使用原厂完整直流平衡波形 E0470_FULL_WAVEFORM。
    # Verify book GC16 page turns and full refreshes use the DC-balanced E0470_FULL_WAVEFORM.
    present_fn = function("present")
    assert "update_display_full_with(ctx->hl, &E0470_FULL_WAVEFORM)" in present_fn
    assert "update_display_full(ctx->hl)" not in present_fn
    gc16_src = (Path(__file__).resolve().parents[1] / "components/e0470_epaper_waveform/waveforms/gc16.h").read_text(encoding="utf-8")
    gm = re.search(r"\[[^\]]+\]\[16\]\[4\]\s*=\s*\{(.*?)\};", gc16_src, re.S)
    assert gm, "gc16 LUT"
    gvals = [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+", gm.group(1))]
    gframes = len(gvals) // 64
    def gc16_dc(fr, to):
        b = w = 0
        for f in range(gframes):
            act = (gvals[(f * 16 + to) * 4 + fr // 4] >> (6 - 2 * (fr % 4))) & 3
            if act == 1: b += 1
            elif act == 2: w += 1
        return w - b
    assert abs(gc16_dc(15, 15)) <= 1, f"full GC16 15->15 DC imbalance: {gc16_dc(15, 15)}"
    assert gc16_dc(15, 0) + gc16_dc(0, 15) == 0, "full GC16 15->0->15 DC imbalance"
    print("book_gc16_full_waveform_test: PASS")

    # 验证阅读页底部进度条防误触：正文态底部栏不响应点按，仅打开工具栏后可点按进度条跳转。
    # Verify reader bottom progress bar guard: ignored during clean reading, tappable only when toolbar is open.
    action_fn = function("action_at")
    rm = re.search(r"if \(s_view == READING\) \{", action_fn)
    assert rm, "READING branch in action_at"
    at, depth = rm.end(), 1
    while depth:
        if action_fn[at] == "{": depth += 1
        elif action_fn[at] == "}": depth -= 1
        at += 1
    reading_action_branch = action_fn[rm.start():at]
    reader_bar_unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define UI_GAP 12
#define UI_MARGIN 40
#define UI_BTN_H 84
#define UI_BAR_TOP 1072
#define BOOK_TOOL_COUNT 12
#define BOOK_TOOL_ROWS 4
#define BOOK_TOC_ROWS 10
#define MODE_DU 1
#define MODE_GL16 2
typedef struct { int x, y, width, height; } EpdRect;
typedef struct { int leaf; bool request_menu; bool key_handled; int64_t now_ms; } app_ctx_t;
typedef enum { APP_REDRAW_PAGE, APP_REDRAW_AREA, APP_REDRAW_NONE, APP_REDRAW_FULL } app_redraw_t;
typedef enum { SHELF, READING, TOC, MANAGE, BULK, SEARCH, BOOKMARKS, EXCERPTS, TYPOGRAPHY, CHAPTER_RESULTS } book_view_t;
static int epd_width(void) { return 1216; }
static int epd_height(void) { return 684; }
static int ui_content_width(void) { return 684 - 2 * UI_MARGIN; }
static EpdRect ui_grid_rect(int col, int cols, int row, int y0, int h) {
    int w = (ui_content_width() - (cols - 1) * UI_GAP) / cols;
    return (EpdRect){ UI_MARGIN + col * (w + UI_GAP), y0 + row * (h + UI_GAP), w, h };
}
static bool ui_rect_hit(EpdRect r, uint16_t x, uint16_t y) {
    return (int)x >= r.x && (int)x < r.x + r.width && (int)y >= r.y && (int)y < r.y + r.height;
}
static EpdRect book_layout_side_margins(EpdRect r, unsigned m) {
    return (EpdRect){ r.x + (int)m, r.y, r.width - 2 * (int)m, r.height };
}
static book_view_t s_view = READING, s_search_parent;
static bool s_image_open, s_cbz_mode, s_selecting, s_toolbar, s_reader_landscape, s_shake_enabled, s_full, s_chapter_search;
static size_t s_page, s_cbz_page, s_text_len = 100, s_sel_start, s_sel_end, s_chapter, s_block_count;
static int s_px = 48, s_sel_line_y;
static unsigned s_pages_since_clean;
static char *s_text = "abc";
static void *s_blocks;
static char s_note_message[128], s_search_draft[65];
static bool test_top_bar = true, test_bottom_bar = true;
static uint8_t test_margin = 1;
static unsigned test_seek_calls, test_turn_calls;
static int test_turn_dir;
static uint16_t test_seek_x;
static bool app_settings_book_top_bar(void) { return test_top_bar; }
static bool app_settings_book_bottom_bar(void) { return test_bottom_bar; }
static unsigned app_settings_book_margin(void) { return test_margin; }
static uint8_t app_settings_book_cbz_dir(void) { return 0; }
static void app_settings_set_book_cbz_dir(uint8_t v) { (void)v; }
static bool app_settings_book_cbz_dither(void) { return false; }
static void app_settings_set_book_cbz_dither(bool v) { (void)v; }
static uint8_t app_settings_book_gesture(void) { return 0; }
static void app_settings_set_book_gesture(uint8_t v) { (void)v; }
static uint8_t app_settings_book_refresh(void) { return 1; }
static void app_settings_set_book_refresh(uint8_t v) { (void)v; }
static uint8_t app_settings_book_rotation(void) { return 0; }
static void app_settings_set_book_rotation(uint8_t v) { (void)v; }
static unsigned side_margin(void);
static void lock_draw(void) {}
static void unlock_draw(void) {}
static void invalidate_prep(void) {}
static void save_progress(void) {}
static void sensor_set(app_ctx_t *ctx, bool on) { (void)ctx; (void)on; }
static void free_book(void) {}
static void scan_shelf(app_ctx_t *ctx) { (void)ctx; }
static void apply_layout_opts(void) {}
static unsigned line_height_percent(void) { return 150; }
static bool cbz_load_page_locked(size_t p) { (void)p; return true; }
static app_redraw_t close_image(app_ctx_t *ctx) { (void)ctx; return APP_REDRAW_PAGE; }
static app_redraw_t resize_text(app_ctx_t *ctx, int d) { (void)ctx; (void)d; return APP_REDRAW_PAGE; }
static void open_note_list(app_ctx_t *ctx, book_view_t v) { (void)ctx; (void)v; }
static app_redraw_t paint_reading(app_ctx_t *ctx, int m) { (void)ctx; (void)m; return APP_REDRAW_AREA; }
static app_redraw_t open_image(app_ctx_t *ctx, size_t b) { (void)ctx; (void)b; return APP_REDRAW_PAGE; }
static EpdRect sel_btn_rect(int i) { (void)i; return (EpdRect){0}; }
static size_t book_layout_page_start_offset(size_t p) { (void)p; return 0; }
static size_t book_layout_page_count(void) { return 2; }
static bool book_layout_expand_prev_sentence(const char *t, size_t l, size_t s, size_t ps, size_t *out) { (void)t;(void)l;(void)s;(void)ps;(void)out; return false; }
static bool book_layout_expand_next_sentence(const char *t, size_t l, size_t e, size_t pe, size_t *out) { (void)t;(void)l;(void)e;(void)pe;(void)out; return false; }
static bool book_layout_paragraph_bounds(const char *t, size_t l, size_t s, size_t *o0, size_t *o1) { (void)t;(void)l;(void)s;(void)o0;(void)o1; return false; }
static bool save_selected_excerpt(size_t a, size_t b) { (void)a; (void)b; return true; }
static bool book_layout_hit_text(size_t p, EpdRect r, uint16_t x, uint16_t y, size_t *o, int *ly) { (void)p;(void)r;(void)x;(void)y;(void)o;(void)ly; return false; }
static bool book_layout_sentence_bounds(const char *t, size_t l, size_t o, size_t a, size_t b, size_t *o0, size_t *o1) { (void)t;(void)l;(void)o;(void)a;(void)b;(void)o0;(void)o1; return false; }
static bool book_layout_build_blocks_metrics(const char *t, size_t l, void *b, size_t c, EpdRect r, int px, unsigned lh) { (void)t;(void)l;(void)b;(void)c;(void)r;(void)px;(void)lh; return true; }
static size_t book_layout_page_for_offset(size_t o) { (void)o; return 0; }
static size_t book_layout_image_at(size_t p, EpdRect r, uint16_t x, uint16_t y, EpdRect *out) { (void)p;(void)r;(void)x;(void)y;(void)out; return SIZE_MAX; }
static void copy_text(char *dst, size_t cap, const char *src) { snprintf(dst, cap, "%s", src); }
static app_redraw_t seek_progress(app_ctx_t *ctx, uint16_t x) {
    (void)ctx;
    ++test_seek_calls;
    test_seek_x = x;
    s_toolbar = false;
    return APP_REDRAW_FULL;
}
static app_redraw_t turn_page(app_ctx_t *ctx, int dir) {
    (void)ctx;
    ++test_turn_calls;
    test_turn_dir = dir;
    return APP_REDRAW_PAGE;
}
'''
    for name in ("reader_width", "reader_height", "body_rect", "bottom_bar_rect", "progress_rect", "tool_rect", "side_margin"):
        reader_bar_unit += function(name) + "\n"
    reader_bar_unit += (
        "static app_redraw_t reading_action(app_ctx_t *ctx, uint16_t x, uint16_t y) {\n"
        + reading_action_branch
        + "\n    return APP_REDRAW_NONE;\n}\n"
        + r'''
int main(void) {
    app_ctx_t ctx = {0};
    for (int landscape = 0; landscape <= 1; ++landscape) {
        s_reader_landscape = landscape != 0;
        test_bottom_bar = true;
        EpdRect body = body_rect();
        EpdRect bot = bottom_bar_rect();
        EpdRect track = progress_rect();
        assert(body.y + body.height == bot.y);
        assert(track.y >= bot.y && track.y + track.height <= bot.y + bot.height);

        // 1. 正文阅读态 + 底部栏显示：点按进度条左/中/右及底部栏边距均无动作，不触发跳转/翻页/工具栏。
        // Clean reading with bottom bar visible: taps across progress track and bottom margins do nothing.
        test_bottom_bar = true;
        s_toolbar = false;
        test_seek_calls = test_turn_calls = 0;
        uint16_t xs[5] = {
            (uint16_t)(bot.x + 4),
            (uint16_t)(track.x + 4),
            (uint16_t)(track.x + track.width / 2),
            (uint16_t)(track.x + track.width - 5),
            (uint16_t)(bot.x + bot.width - 4),
        };
        for (int i = 0; i < 5; ++i) {
            assert(reading_action(&ctx, xs[i], (uint16_t)(track.y + 12)) == APP_REDRAW_NONE);
            assert(reading_action(&ctx, xs[i], (uint16_t)(bot.y + bot.height - 1)) == APP_REDRAW_NONE);
        }
        assert(test_seek_calls == 0 && test_turn_calls == 0 && !s_toolbar);

        // 正文区仍正常翻页与呼出工具栏。/ Body taps still turn pages and toggle the toolbar.
        assert(reading_action(&ctx, (uint16_t)(reader_width() / 2), (uint16_t)(body.y + body.height / 2)) == APP_REDRAW_PAGE);
        assert(s_toolbar && test_seek_calls == 0 && test_turn_calls == 0);

        // 2. 工具栏打开时：点按进度条可跳转并收起工具栏；点按底部栏边距不误翻页。
        // With toolbar open: tapping progress track seeks; tapping bottom-bar margin does not turn pages.
        assert(reading_action(&ctx, (uint16_t)(bot.x + 2), (uint16_t)(track.y + 12)) == APP_REDRAW_NONE);
        assert(s_toolbar && test_seek_calls == 0 && test_turn_calls == 0);
        uint16_t seek_target = (uint16_t)(track.x + track.width / 2);
        assert(reading_action(&ctx, seek_target, (uint16_t)(track.y + 12)) == APP_REDRAW_FULL);
        assert(test_seek_calls == 1 && test_seek_x == seek_target && !s_toolbar && test_turn_calls == 0);

        // 3. 底部栏关闭时：正文态点按底部区走常规正文翻页，不跳转进度；打开工具栏后进度条仍可点按跳转。
        // With bottom bar hidden: clean reading bottom taps act as normal body taps; toolbar still allows seeking.
        test_bottom_bar = false;
        s_toolbar = false;
        test_seek_calls = test_turn_calls = 0;
        assert(reading_action(&ctx, (uint16_t)(reader_width() - 20), (uint16_t)(track.y + 12)) == APP_REDRAW_PAGE);
        assert(test_seek_calls == 0 && test_turn_calls == 1 && test_turn_dir == 1);
        s_toolbar = true;
        assert(reading_action(&ctx, seek_target, (uint16_t)(track.y + 12)) == APP_REDRAW_FULL);
        assert(test_seek_calls == 1 && !s_toolbar);
    }
    puts("book_reader_bottom_progress_guard_test: PASS");
}
'''
    )
    bar_c = Path(tmp) / "reader_bar_guard.c"
    bar_exe = Path(tmp) / "reader_bar_guard"
    bar_c.write_text(reader_bar_unit, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined", str(bar_c), "-o", str(bar_exe)], check=True)
    subprocess.run([str(bar_exe)], check=True)
