/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：索引 CBZ 图片页并按自然文件名排序，按 20 页生成章节。
 * English: Index CBZ image pages, sort naturally and group twenty pages per chapter.
 *
 * 冻结：只解压当前页；单页输入沿用 ZIP 的 4 MiB 上限。
 * Frozen: Extract only the current page; each ZIP entry keeps the 4 MiB cap.
 */
#include "book_source_internal.h"
#include "book_image.h"
#include "zip_reader.h"
#include "esp_heap_caps.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#define CBZ_GROUP 20
#define CBZ_PAGE_MAX 32768
#define PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
typedef struct { int zip; char *name; } page_t;
struct book_cbz { zip_reader_t *zip; page_t *pages; size_t count; };
static bool image_name(const char*n){const char*e=strrchr(n,'.');return e&&(!strcasecmp(e,".jpg")||!strcasecmp(e,".jpeg")||!strcasecmp(e,".png"));}
static int natural(const char*a,const char*b){while(*a&&*b){if(isdigit((unsigned char)*a)&&isdigit((unsigned char)*b)){const char*x=a,*y=b;while(*x=='0')x++;while(*y=='0')y++;const char*xe=x,*ye=y;while(isdigit((unsigned char)*xe))xe++;while(isdigit((unsigned char)*ye))ye++;size_t nx=xe-x,ny=ye-y;if(nx!=ny)return nx<ny?-1:1;int c=memcmp(x,y,nx);if(c)return c<0?-1:1;while(isdigit((unsigned char)*a))a++;while(isdigit((unsigned char)*b))b++;continue;}int x=tolower((unsigned char)*a),y=tolower((unsigned char)*b);if(x!=y)return x<y?-1:1;a++;b++;}return (*a!=0)-(*b!=0);}
static int page_cmp(const void*a,const void*b){return natural(((const page_t*)a)->name,((const page_t*)b)->name);}
esp_err_t book_cbz_open(const char*path,book_cbz_t**out){if(!out||!path)return ESP_ERR_INVALID_ARG;*out=NULL;book_cbz_t*b=calloc(1,sizeof(*b));if(!b)return ESP_ERR_NO_MEM;esp_err_t e=zip_open(path,&b->zip);if(e!=ESP_OK){free(b);return e;}size_t n=zip_entry_count(b->zip);b->pages=heap_caps_calloc(n?n:1,sizeof(*b->pages),PSRAM);if(!b->pages){book_cbz_close(b);return ESP_ERR_NO_MEM;}for(size_t i=0;i<n;i++){const char*name=zip_entry_name(b->zip,(int)i);if(!name||!image_name(name)||!zip_entry_size(b->zip,(int)i))continue;if(b->count==CBZ_PAGE_MAX)break;size_t len=strlen(name);char*copy=heap_caps_malloc(len+1,PSRAM);if(!copy){book_cbz_close(b);return ESP_ERR_NO_MEM;}memcpy(copy,name,len+1);b->pages[b->count++]=(page_t){(int)i,copy};}if(!b->count){book_cbz_close(b);return ESP_ERR_NOT_FOUND;}qsort(b->pages,b->count,sizeof(*b->pages),page_cmp);*out=b;return ESP_OK;}
void book_cbz_close(book_cbz_t*b){if(!b)return;for(size_t i=0;i<b->count;i++)free(b->pages[i].name);free(b->pages);zip_close(b->zip);free(b);}
size_t book_cbz_chapter_count(const book_cbz_t*b){return b?(b->count+CBZ_GROUP-1)/CBZ_GROUP:0;}
esp_err_t book_cbz_chapter_title(const book_cbz_t*b,size_t i,char*out,size_t cap){if(!b||!out||!cap||i>=book_cbz_chapter_count(b))return ESP_ERR_INVALID_ARG;int n=snprintf(out,cap,"第 %u 组 · %u-%u",(unsigned)i+1,(unsigned)(i*CBZ_GROUP+1),(unsigned)((i+1)*CBZ_GROUP<b->count?(i+1)*CBZ_GROUP:b->count));return n<0||(size_t)n>=cap?ESP_ERR_INVALID_SIZE:ESP_OK;}
uint32_t book_cbz_total_bytes(const book_cbz_t*b){uint64_t sum=0;if(b)for(size_t i=0;i<b->count;i++)sum+=zip_entry_size(b->zip,b->pages[i].zip);return sum>UINT32_MAX?UINT32_MAX:(uint32_t)sum;}
uint32_t book_cbz_chapter_offset(const book_cbz_t*b,size_t i){uint64_t sum=0;if(!b||i>=book_cbz_chapter_count(b))return 0;for(size_t p=0;p<i*CBZ_GROUP;p++)sum+=zip_entry_size(b->zip,b->pages[p].zip);return sum>UINT32_MAX?UINT32_MAX:(uint32_t)sum;}
esp_err_t book_cbz_load(const book_cbz_t*b,size_t i,html_text_t*out){if(!out)return ESP_ERR_INVALID_ARG;memset(out,0,sizeof(*out));if(!b||i>=book_cbz_chapter_count(b))return ESP_ERR_INVALID_ARG;size_t first=i*CBZ_GROUP,n=b->count-first;if(n>CBZ_GROUP)n=CBZ_GROUP;char*text=heap_caps_calloc(n,2,PSRAM);blk_t*blocks=heap_caps_calloc(n,sizeof(*blocks),PSRAM);if(!text||!blocks){free(text);free(blocks);return ESP_ERR_NO_MEM;}for(size_t j=0;j<n;j++){blocks[j].offset=j;blocks[j].len=1;blocks[j].image_src=heap_caps_malloc(12,PSRAM);if(!blocks[j].image_src){html_blocks_free(blocks,n);free(text);return ESP_ERR_NO_MEM;}snprintf(blocks[j].image_src,12,"%u",(unsigned)(first+j));text[j]=' ';}text[n]=0;out->utf8=text;out->len=n;out->blocks=blocks;out->count=n;return ESP_OK;}
esp_err_t book_cbz_load_image(book_cbz_t*b,size_t chapter,const char*ref,uint8_t**pixels,uint16_t*w,uint16_t*h){if(!pixels||!w||!h)return ESP_ERR_INVALID_ARG;*pixels=NULL;*w=*h=0;if(!b||!ref)return ESP_ERR_INVALID_ARG;char*end=NULL;unsigned long p=strtoul(ref,&end,10);if(!*ref||*end||p>=b->count||p/CBZ_GROUP!=chapter)return ESP_ERR_INVALID_ARG;size_t n=zip_entry_size(b->zip,b->pages[p].zip);if(!n||n>ZIP_OUTPUT_MAX)return ESP_ERR_INVALID_SIZE;uint8_t*encoded=heap_caps_malloc(n,PSRAM);if(!encoded)return ESP_ERR_NO_MEM;esp_err_t e=zip_extract(b->zip,b->pages[p].zip,encoded,n);if(e==ESP_OK&&!book_image_decode(encoded,n,ZIP_OUTPUT_MAX,pixels,w,h))e=ESP_ERR_INVALID_SIZE;free(encoded);return e;}
