/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：将常见 EPUB 插图变为适合墨水屏的白底灰度图。
 * English: Convert common EPUB illustrations to white-backed grayscale for the panel.
 * 冻结：只在书源加载阶段串行调用；解码器分配总量有界，不在 render 中解码。
 * Frozen: Serialize calls during source loading; bound total decoder allocation and never decode inside render.
 */
#include "book_image.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>
#include "vendor/tjpgd.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define IMAGE_HEAP_MAX (4u * 1024u * 1024u)
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
typedef union { size_t size; max_align_t align; } allocation_t;
static size_t allocated;

static void* image_alloc(size_t n) {
    if (n > SIZE_MAX - sizeof(allocation_t) || n + sizeof(allocation_t) > IMAGE_HEAP_MAX - allocated) return NULL;
    allocation_t* p = heap_caps_malloc(sizeof(*p) + n, PSRAM_CAPS);
    if (!p) return NULL;
    p->size = n; allocated += n + sizeof(*p);
    return p + 1;
}
static void image_free(void* ptr) {
    if (!ptr) return;
    allocation_t* p = (allocation_t*)ptr - 1;
    allocated -= p->size + sizeof(*p);
    free(p);
}
static void* image_realloc(void* ptr, size_t n) {
    if (!ptr) return image_alloc(n);
    allocation_t* old = (allocation_t*)ptr - 1;
    size_t prior = old->size;
    if (n > SIZE_MAX - sizeof(*old) || n > IMAGE_HEAP_MAX - (allocated - prior)) return NULL;
    allocation_t* p = heap_caps_realloc(old, sizeof(*old) + n, PSRAM_CAPS);
    if (!p) return NULL;
    p->size = n; allocated = allocated - prior + n;
    return p + 1;
}

#define STBI_MALLOC(n) image_alloc(n)
#define STBI_REALLOC(p,n) image_realloc(p,n)
#define STBI_FREE(p) image_free(p)
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb_image.h"

static uint32_t png_u32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static unsigned png_u16(const uint8_t* p) { return (unsigned)p[0] << 8 | p[1]; }
static uint8_t png_paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

typedef struct {
    unsigned dst_w, dst_h;
    unsigned crop_x, crop_y, crop_w, crop_h;
} image_fit_t;

// 统一计算输出尺寸与源图裁切框；cover 模式等比铺满并居中裁切，否则在上限内等比缩放。
// Compute output dimensions and source crop box; cover mode aspect-fills and center-crops, otherwise fits within bounds.
static image_fit_t compute_fit(unsigned src_w, unsigned src_h,
                               unsigned max_w, unsigned max_h, bool cover) {
    image_fit_t fit = { .crop_w = src_w, .crop_h = src_h };
    if (cover) {
        fit.dst_w = max_w;
        fit.dst_h = max_h;
        uint64_t lhs = (uint64_t)src_w * max_h;
        uint64_t rhs = (uint64_t)max_w * src_h;
        if (lhs > rhs) {
            unsigned cw = (unsigned)(((uint64_t)src_h * max_w + max_h / 2) / max_h);
            if (cw < 1) cw = 1;
            if (cw > src_w) cw = src_w;
            fit.crop_w = cw;
            fit.crop_x = (src_w - cw) / 2;
        } else if (lhs < rhs) {
            unsigned ch = (unsigned)(((uint64_t)src_w * max_h + max_w / 2) / max_w);
            if (ch < 1) ch = 1;
            if (ch > src_h) ch = src_h;
            fit.crop_h = ch;
            fit.crop_y = (src_h - ch) / 2;
        }
        return fit;
    }
    unsigned dw = src_w, dh = src_h;
    if (dw > max_w) { dh = (unsigned)((uint64_t)dh * max_w / dw); dw = max_w; }
    if (dh > max_h) { dw = (unsigned)((uint64_t)dw * max_h / dh); dh = max_h; }
    if (!dw) dw = 1;
    if (!dh) dh = 1;
    fit.dst_w = dw;
    fit.dst_h = dh;
    return fit;
}

// 常见8位非交错PNG在解压缓冲内逐行还原，避免再分配一份完整RGB图。
// Unfilter common 8-bit noninterlaced PNG in place, avoiding a second full RGB image allocation.
static bool png_decode(const uint8_t* data, size_t len, unsigned max_w, unsigned max_h, bool cover,
                       size_t budget, uint8_t** pixels, uint16_t* width, uint16_t* height) {
    if (len < 33 || memcmp(data,"\x89PNG\r\n\x1a\n",8) || png_u32(data+8)!=13 || memcmp(data+12,"IHDR",4)) return false;
    unsigned w=png_u32(data+16), h=png_u32(data+20), type=data[25];
    unsigned channels=type==0 ? 1 : type==2 ? 3 : type==3 ? 1 : type==4 ? 2 : type==6 ? 4 : 0;
    if (!w || !h || w>4096 || h>4096 || (uint64_t)w*h>1024u*1024u || !channels ||
        data[24]!=8 || data[26] || data[27] || data[28]) return false;
    const uint8_t *palette=NULL,*trans=NULL; size_t palette_n=0,trans_n=0,packed_n=0;
    bool idat=false,ended=false;
    for (size_t at=33;at<len;) {
        if (len-at<12) return false;
        size_t n=png_u32(data+at);
        if (n>len-at-12) return false;
        const uint8_t* tag=data+at+4;
        if (!memcmp(tag,"IDAT",4)) { packed_n+=n; idat=true; }
        else if (!memcmp(tag,"PLTE",4)) {
            if (idat || palette || !n || n%3 || n>768) return false;
            palette=data+at+8;palette_n=n/3;
        } else if (!memcmp(tag,"tRNS",4)) {
            if (idat || trans || (type==0 ? n!=2 : type==2 ? n!=6 : type==3 ? !n || n>256 : true)) return false;
            trans=data+at+8;trans_n=n;
        } else if (!memcmp(tag,"IEND",4)) { if(n)return false; ended=true;break; }
        else if (!(tag[0]&32)) return false;
        at+=n+12;
    }
    if (!ended || !packed_n || (type==3 && (!palette || trans_n>palette_n))) return false;
    image_fit_t fit = compute_fit(w, h, max_w, max_h, cover);
    unsigned dw = fit.dst_w, dh = fit.dst_h;
    if((uint64_t)dw*dh>budget)return false;
    size_t row_n=(size_t)w*channels, raw_n=(row_n+1)*h;
    uint8_t* packed=image_alloc(packed_n);
    uint8_t* raw=image_alloc(raw_n);
    uint8_t* out=NULL;
    bool ok=false;
    if(!packed || !raw)goto done;
    size_t pos=0;
    for(size_t at=33;at+12<=len;) {
        size_t n=png_u32(data+at);
        if(!memcmp(data+at+4,"IDAT",4)){memcpy(packed+pos,data+at+8,n);pos+=n;}
        if(!memcmp(data+at+4,"IEND",4))break;
        at+=n+12;
    }
    if(stbi_zlib_decode_buffer((char*)raw,(int)raw_n,(const char*)packed,(int)packed_n)!=(int)raw_n)goto done;
    image_free(packed);packed=NULL;
    out=heap_caps_malloc((size_t)dw*dh,PSRAM_CAPS);
    if(!out)goto done;
    unsigned dy=0;
    for(unsigned y=0;y<h;++y) {
        uint8_t* row=raw+(row_n+1)*y+1;
        const uint8_t* prev=y ? row-row_n-1 : NULL;
        unsigned filter=row[-1];
        if(filter>4)goto done;
        for(size_t x=0;x<row_n;++x) {
            unsigned a=x>=channels ? row[x-channels] : 0,b=prev ? prev[x] : 0,c=prev && x>=channels ? prev[x-channels] : 0;
            unsigned add=filter==0 ? 0 : filter==1 ? a : filter==2 ? b : filter==3 ? (a+b)/2 : png_paeth(a,b,c);
            row[x]=(uint8_t)(row[x]+add);
        }
        bool row_rendered = false;
        unsigned rendered_dy = 0;
        while(dy<dh && fit.crop_y+(uint64_t)dy*fit.crop_h/dh==y) {
            if (row_rendered) {
                memcpy(out + (size_t)dy * dw, out + (size_t)rendered_dy * dw, dw);
            } else {
                for(unsigned x=0;x<dw;++x) {
                    const uint8_t* p=row+(size_t)(fit.crop_x+(uint64_t)x*fit.crop_w/dw)*channels;
                    unsigned gray=0,alpha=255;
                    if(type==0 || type==4) {
                        gray=p[0];
                        if(type==4)alpha=p[1];
                        else if(trans && p[0]==png_u16(trans))alpha=0;
                    } else if(type==3) {
                        unsigned index=p[0];
                        if(index>=palette_n)goto done;
                        const uint8_t* color=palette+index*3;
                        gray=(77u*color[0]+150u*color[1]+29u*color[2])>>8;
                        if(index<trans_n)alpha=trans[index];
                    } else {
                        gray=(77u*p[0]+150u*p[1]+29u*p[2])>>8;
                        if(type==6)alpha=p[3];
                        else if(trans && p[0]==png_u16(trans) && p[1]==png_u16(trans+2) && p[2]==png_u16(trans+4))alpha=0;
                    }
                    out[(size_t)dy*dw+x]=(uint8_t)((gray*alpha+255u*(255u-alpha)+127u)/255u);
                }
                row_rendered = true;
                rendered_dy = dy;
            }
            ++dy;
        }
#ifdef ESP_PLATFORM
        if(!(y%32))vTaskDelay(1);
#endif
    }
    if(dy!=dh)goto done;
    *pixels=out;*width=(uint16_t)dw;*height=(uint16_t)dh;out=NULL;ok=true;
done:
    image_free(packed);image_free(raw);free(out);return ok;
}

typedef struct {
    const uint8_t* data;
    size_t len, at;
    uint8_t* out;
    unsigned width, height, scaled_width, scaled_height;
    unsigned crop_x, crop_y, crop_w, crop_h;
    unsigned blocks;
} jpeg_io_t;

static size_t jpeg_read(JDEC* jd, uint8_t* out, size_t count) {
    jpeg_io_t* io = jd->device;
    if (count > io->len - io->at) count = io->len - io->at;
    if (out) memcpy(out, io->data + io->at, count);
    io->at += count;
    return count;
}
static int jpeg_write(JDEC* jd, void* bitmap, JRECT* r) {
    jpeg_io_t* io = jd->device;
    if (r->right >= io->scaled_width || r->bottom >= io->scaled_height ||
        r->left > r->right || r->top > r->bottom) return 0;
    if ((unsigned)r->right < io->crop_x || (unsigned)r->left >= io->crop_x + io->crop_w ||
        (unsigned)r->bottom < io->crop_y || (unsigned)r->top >= io->crop_y + io->crop_h) {
#ifdef ESP_PLATFORM
        if (!(++io->blocks % 32)) vTaskDelay(1);
#endif
        return 1;
    }
    unsigned rel_l = (unsigned)r->left > io->crop_x ? (unsigned)r->left - io->crop_x : 0;
    unsigned rel_r = (unsigned)r->right + 1 - io->crop_x;
    if (rel_r > io->crop_w) rel_r = io->crop_w;
    unsigned rel_t = (unsigned)r->top > io->crop_y ? (unsigned)r->top - io->crop_y : 0;
    unsigned rel_b = (unsigned)r->bottom + 1 - io->crop_y;
    if (rel_b > io->crop_h) rel_b = io->crop_h;
    unsigned x0 = (unsigned)(((uint64_t)rel_l * io->width + io->crop_w - 1) / io->crop_w);
    unsigned x1 = (unsigned)(((uint64_t)rel_r * io->width + io->crop_w - 1) / io->crop_w);
    unsigned y0 = (unsigned)(((uint64_t)rel_t * io->height + io->crop_h - 1) / io->crop_h);
    unsigned y1 = (unsigned)(((uint64_t)rel_b * io->height + io->crop_h - 1) / io->crop_h);
    const uint8_t* rgb = bitmap;
    unsigned stride = (unsigned)(r->right - r->left + 1);
    for (unsigned y = y0; y < y1; ++y) {
        unsigned sy = io->crop_y + (unsigned)((uint64_t)y * io->crop_h / io->height) - r->top;
        for (unsigned x = x0; x < x1; ++x) {
            unsigned sx = io->crop_x + (unsigned)((uint64_t)x * io->crop_w / io->width) - r->left;
            const uint8_t* color = rgb + (sy * stride + sx) * 3;
            io->out[(size_t)y * io->width + x] = (uint8_t)((77u*color[0]+150u*color[1]+29u*color[2])>>8);
        }
    }
#ifdef ESP_PLATFORM
    // 大图也定期让出 CPU，避免空闲任务看门狗饥饿。/ Yield during large images so idle watchdog tasks can run.
    if (!(++io->blocks % 32)) vTaskDelay(1);
#endif
    return 1;
}
static bool jpeg_decode(const uint8_t* data, size_t len, unsigned max_w, unsigned max_h, bool cover,
                        size_t budget, uint8_t** pixels, uint16_t* width, uint16_t* height) {
    void* work = heap_caps_malloc(8192, PSRAM_CAPS);
    if (!work) return false;
    JDEC jd;
    jpeg_io_t io = {.data=data, .len=len};
    bool ok = false;
    if (jd_prepare(&jd, jpeg_read, work, 8192, &io) != JDR_OK || !jd.width || !jd.height ||
        jd.width > 8192 || jd.height > 8192 || (uint64_t)jd.width * jd.height > 16u * 1024u * 1024u) goto done;
    image_fit_t fit = compute_fit(jd.width, jd.height, max_w, max_h, cover);
    io.width = fit.dst_w; io.height = fit.dst_h;
    if ((uint64_t)io.width * io.height > budget) goto done;
    unsigned scale = 0;
    if (cover) {
        // 铺满模式优先保清晰度：仅在裁切区仍不小于目标尺寸时才在 JPEG 解码阶段降采样。
        // Preserve full-screen sharpness in cover mode: only downscale in JPEG decode while the cropped region still covers target size.
        while (scale < 3 && (fit.crop_w >> (scale + 1)) >= io.width && (fit.crop_h >> (scale + 1)) >= io.height) ++scale;
    } else {
        // 阅读插图允许至多两倍放大，优先在 JPEG 解码阶段降采样。/ Allow at most 2x enlargement to reduce work during JPEG decoding.
        while (scale < 3 && ((unsigned)jd.width >> (scale + 1)) >= (io.width + 1) / 2 && ((unsigned)jd.height >> (scale + 1)) >= (io.height + 1) / 2) ++scale;
    }
    io.scaled_width = jd.width >> scale; io.scaled_height = jd.height >> scale;
    if (!io.scaled_width || !io.scaled_height) goto done;
    if (cover) {
        image_fit_t scaled_fit = compute_fit(io.scaled_width, io.scaled_height, io.width, io.height, true);
        io.crop_x = scaled_fit.crop_x; io.crop_y = scaled_fit.crop_y;
        io.crop_w = scaled_fit.crop_w; io.crop_h = scaled_fit.crop_h;
    } else {
        io.crop_x = 0; io.crop_y = 0;
        io.crop_w = io.scaled_width; io.crop_h = io.scaled_height;
    }
    io.out = heap_caps_malloc((size_t)io.width * io.height, PSRAM_CAPS);
    if (!io.out) goto done;
    memset(io.out, 255, (size_t)io.width * io.height);
    ok = jd_decomp(&jd, jpeg_write, (uint8_t)scale) == JDR_OK;
    if (ok) { *pixels = io.out; *width = (uint16_t)io.width; *height = (uint16_t)io.height; }
    else free(io.out);
done:
    free(work);
    return ok;
}

static bool book_image_decode_impl(const uint8_t* data, size_t len,
                                   unsigned max_w, unsigned max_h, bool cover, size_t budget,
                                   uint8_t** pixels, uint16_t* width, uint16_t* height) {
    if (!pixels || !width || !height) return false;
    *pixels = NULL; *width = *height = 0;
    if (!data || !len || len > 2u * 1024u * 1024u || !max_w || !max_h || allocated) return false;
    if (png_decode(data, len, max_w, max_h, cover, budget, pixels, width, height)) return true;
    // 基线 JPEG 优先使用 MCU 分块缩小；小型渐进 JPEG 仍可走有界完整解码。
    // Prefer MCU-scaled baseline JPEG; bounded full decoding still handles small progressive JPEGs.
    if (len >= 2 && data[0] == 0xff && data[1] == 0xd8 &&
        jpeg_decode(data, len, max_w, max_h, cover, budget, pixels, width, height)) return true;
    int w, h, channels;
    if (!stbi_info_from_memory(data, (int)len, &w, &h, &channels) ||
        w <= 0 || h <= 0 || (uint64_t)w * h > 1024u * 1024u) return false;
    image_fit_t fit = compute_fit((unsigned)w, (unsigned)h, max_w, max_h, cover);
    if ((uint64_t)fit.dst_w * fit.dst_h > budget) return false;
    size_t size = (size_t)fit.dst_w * fit.dst_h;
    uint8_t* gray = stbi_load_from_memory(data, (int)len, &w, &h, &channels, 2);
    if (!gray) return false;
    if (w <= 0 || h <= 0) { stbi_image_free(gray); return false; }
    fit = compute_fit((unsigned)w, (unsigned)h, max_w, max_h, cover);
    if ((uint64_t)fit.dst_w * fit.dst_h != size) { stbi_image_free(gray); return false; }
    uint8_t* out = heap_caps_malloc(size, PSRAM_CAPS);
    if (out) {
        for (unsigned y = 0; y < fit.dst_h; ++y) {
            size_t sy = fit.crop_y + (size_t)((uint64_t)y * fit.crop_h / fit.dst_h);
            for (unsigned x = 0; x < fit.dst_w; ++x) {
                size_t sx = fit.crop_x + (size_t)((uint64_t)x * fit.crop_w / fit.dst_w);
                size_t at = (sy * (size_t)w + sx) * 2;
                unsigned alpha = gray[at + 1];
                out[(size_t)y * fit.dst_w + x] = (uint8_t)((gray[at] * alpha + 255u * (255u - alpha) + 127u) / 255u);
            }
        }
    }
    stbi_image_free(gray);
    if (!out) return false;
    *pixels = out; *width = (uint16_t)fit.dst_w; *height = (uint16_t)fit.dst_h;
    return true;
}

bool book_image_decode(const uint8_t* data, size_t len, size_t budget,
                       uint8_t** pixels, uint16_t* width, uint16_t* height) {
    return book_image_decode_impl(data, len, 648, 1000, false, budget, pixels, width, height);
}

bool book_image_decode_cover(const uint8_t* data, size_t len,
                             uint16_t target_w, uint16_t target_h, size_t budget,
                             uint8_t** pixels, uint16_t* width, uint16_t* height) {
    return book_image_decode_impl(data, len, target_w, target_h, true, budget, pixels, width, height);
}
