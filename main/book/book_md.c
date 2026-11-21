/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：有界 Markdown 章节索引、行内清洗与标题块生成。
 * English: Bounded Markdown chapter indexing, inline cleanup and heading blocks.
 *
 * 冻结：只读本地文件；源文件与输出均限制为 2 MiB。
 * Frozen: Read local files only; source and output are capped at 2 MiB.
 */
#include "book_source_internal.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MD_MAX (2u * 1024u * 1024u)
#define MD_CHAPTERS 2048u
#define MD_BLOCKS 8192u
#define PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
typedef struct { uint32_t start; char title[96]; } md_entry_t;
struct book_md { char *data; uint32_t len; md_entry_t *entries; size_t count; };

static bool heading(const char *p, size_t n, size_t *skip) {
    size_t i=0; while(i<n && i<3 && p[i]=='#') ++i;
    if (!i || i>3 || (i<n && p[i]!=' ' && p[i]!='\t')) return false;
    while(i<n && (p[i]==' '||p[i]=='\t')) ++i;
    *skip=i;
    return true;
}
static void title_copy(char *out, size_t cap, const char *p, size_t n) {
    if (!cap) return;
    if (n>=cap) n=cap-1;
    memcpy(out,p,n);
    out[n]=0;
}
esp_err_t book_md_open(const char *path, book_md_t **out) {
    if (!out || !path) return ESP_ERR_INVALID_ARG;
    *out=NULL;
    FILE *f=fopen(path,"rb"); if(!f) return ESP_ERR_NOT_FOUND;
    if(fseek(f,0,SEEK_END)){fclose(f);return ESP_FAIL;} long n=ftell(f);
    if(n<0 || (unsigned long)n>MD_MAX){fclose(f);return ESP_ERR_INVALID_SIZE;} rewind(f);
    book_md_t *b=calloc(1,sizeof(*b)); if(!b){fclose(f);return ESP_ERR_NO_MEM;}
    b->data=heap_caps_malloc((size_t)n+1,PSRAM); b->entries=heap_caps_calloc(MD_CHAPTERS,sizeof(*b->entries),PSRAM);
    if(!b->data||!b->entries){fclose(f);book_md_close(b);return ESP_ERR_NO_MEM;}
    b->len=(uint32_t)n; if(fread(b->data,1,n,f)!=(size_t)n){fclose(f);book_md_close(b);return ESP_FAIL;} fclose(f); b->data[n]=0;
    size_t pos=0; while(pos<(size_t)n && b->count<MD_CHAPTERS){size_t end=pos;while(end<(size_t)n&&b->data[end]!='\n')++end;size_t skip=0;
        if(heading(b->data+pos,end-pos,&skip)){md_entry_t *e=&b->entries[b->count++];e->start=(uint32_t)pos;size_t z=end-pos-skip;if(z&&b->data[pos+end-pos-1]=='\r')--z;title_copy(e->title,sizeof(e->title),b->data+pos+skip,z);}
        pos=end+1; }
    if(!b->count){b->count=1;b->entries[0].start=0;title_copy(b->entries[0].title,sizeof(b->entries[0].title),path,strlen(path));}
    *out=b;return ESP_OK;
}
void book_md_close(book_md_t *b){if(b){free(b->data);free(b->entries);free(b);}}
size_t book_md_chapter_count(const book_md_t *b){return b?b->count:0;}
esp_err_t book_md_chapter_title(const book_md_t*b,size_t i,char*out,size_t cap){if(!b||!out||!cap||i>=b->count)return ESP_ERR_INVALID_ARG;size_t n=strlen(b->entries[i].title);if(n>=cap)return ESP_ERR_INVALID_SIZE;memcpy(out,b->entries[i].title,n+1);return ESP_OK;}
uint32_t book_md_total_bytes(const book_md_t*b){return b?b->len:0;}
uint32_t book_md_chapter_offset(const book_md_t*b,size_t i){return b&&i<b->count?b->entries[i].start:0;}
static void append(char *o,size_t *n,char c){if(*n<MD_MAX)o[(*n)++]=c;}
static size_t inline_clean(const char*p,size_t n,char*out,size_t cap){size_t w=0;for(size_t i=0;i<n&&w+4<cap;){
    if(p[i]=='['){size_t close=i+1;while(close<n&&p[close]!=']')++close;if(close+1<n&&p[close+1]=='('){size_t end=close+2;while(end<n&&p[end]!=')')++end;if(end<n){for(size_t k=i+1;k<close&&w+1<cap;++k)out[w++]=p[k];i=end+1;continue;}}}
    if(p[i]=='*'||p[i]=='_'||p[i]=='`'){char c=p[i];size_t run=(c=='*'&&i+1<n&&p[i+1]=='*')?2:1; if(run==2||i==0||p[i-1]!='\\'){i+=run;continue;}}
    out[w++]=p[i++];}return w;}
esp_err_t book_md_load(const book_md_t*b,size_t i,html_text_t*out){if(!out)return ESP_ERR_INVALID_ARG;memset(out,0,sizeof(*out));if(!b||i>=b->count)return ESP_ERR_INVALID_ARG;
    uint32_t start=b->entries[i].start,end=i+1<b->count?b->entries[i+1].start:b->len;char *text=heap_caps_malloc(MD_MAX+1,PSRAM);blk_t *blocks=heap_caps_calloc(MD_BLOCKS,sizeof(*blocks),PSRAM);char *line=heap_caps_malloc(MD_MAX+1,PSRAM);if(!text||!blocks||!line){free(text);free(blocks);free(line);return ESP_ERR_NO_MEM;}size_t used=0,count=0,pos=start;
    while(pos<end&&count<MD_BLOCKS){size_t le=pos;while(le<end&&b->data[le]!='\n')++le;size_t n=le-pos;if(n&&b->data[le-1]=='\r')--n;size_t skip=0;bool h=heading(b->data+pos,n,&skip);const char*p=b->data+pos+skip;size_t pn=n-skip;if(pn){char prefix[4];size_t pre=0;if(!h&&pn>=2&&p[0]=='-'&&p[1]==' '){prefix[0]=0xE2;prefix[1]=0x80;prefix[2]=0xA2;prefix[3]=' ';pre=4;p+=2;pn-=2;}else if(!h&&pn>=2&&p[0]=='>'&&p[1]==' '){prefix[0]='>';prefix[1]=' ';pre=2;p+=2;pn-=2;}else if(!h&&pn>=3&&p[0]>='0'&&p[0]<='9'&&p[1]=='.'&&p[2]==' '){prefix[0]=p[0];prefix[1]='.';prefix[2]=' ';pre=3;p+=3;pn-=3;}
        if(count)append(text,&used,'\n');
        size_t off=used;
        if(pre>MD_MAX-used)pre=0;
        if(pre){memcpy(text+used,prefix,pre);used+=pre;}
        size_t clean=inline_clean(p,pn,line,MD_MAX-used);
        memcpy(text+used,line,clean);used+=clean;
        blocks[count]=(blk_t){.offset=off,.len=used-off,.heading=h};++count;}
        pos=le+1;}
    text[used]=0;free(line);if(!count){free(blocks);blocks=NULL;}out->utf8=text;out->len=used;out->blocks=blocks;out->count=count;return ESP_OK;}
