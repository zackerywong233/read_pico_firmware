/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：图书封面提取、144x196 4bpp 缩略图磁盘/内存 LRU 缓存与 684x1216 锁屏封面缓存。
 * English: Book cover extraction, 144x196 4bpp disk+PSRAM LRU thumbnail cache and 684x1216 lockscreen cover cache.
 */

#include "book_cover.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "book_epub.h"
#include "book_image.h"
#include "book_source_internal.h"
#include "esp_heap_caps.h"

#define COVER_MAGIC "RCOV"
#define COVER_VERSION 1u
#define COVER_LRU_CAP 12
#define COVER_LOCK_W 684
#define COVER_LOCK_H 1216
#define COVER_LOCK_BYTES (COVER_LOCK_W * COVER_LOCK_H / 2)
#define PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

typedef struct {
    char magic[4];
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint16_t reserved;
    uint32_t file_size;
} cover_disk_hdr_t;

typedef struct {
    bool valid;
    bool has_cover;
    uint64_t hash;
    uint32_t file_size;
    uint32_t tick;
    uint8_t *data;
} cover_lru_slot_t;

static cover_lru_slot_t s_lru[COVER_LRU_CAP];
static uint32_t s_lru_tick;
static char s_cache_dir[160];

static const char *cache_dir(void) {
    return s_cache_dir[0] ? s_cache_dir : BOOK_COVER_DIR_DEFAULT;
}

void book_cover_set_cache_dir(const char *dir) {
    if (!dir || !dir[0]) s_cache_dir[0] = '\0';
    else snprintf(s_cache_dir, sizeof(s_cache_dir), "%s", dir);
}

static uint64_t fnv1a_64(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)s; p && *p; ++p) {
        h ^= *p;
        h *= 1099511628211ULL;
    }
    return h;
}

static void thumb_cache_path(const char *book_path, char *out, size_t cap) {
    uint64_t h = fnv1a_64(book_path);
    snprintf(out, cap, "%s/%016llx_%dx%d.cov",
             cache_dir(), (unsigned long long)h,
             BOOK_COVER_THUMB_W, BOOK_COVER_THUMB_H);
}

static void lock_cache_path(char *out, size_t cap) {
    snprintf(out, cap, "%s/%s", cache_dir(), BOOK_COVER_LOCK_FILENAME);
}

static bool ensure_cache_dir(void) {
    struct stat st;
    const char *dir = cache_dir();
    if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) return true;
    return mkdir(dir, 0775) == 0 || errno == EEXIST;
}

static void pack_gray8_to_4bpp(const uint8_t *gray, size_t count, uint8_t *out_4bpp) {
    for (size_t i = 0; i < count; i += 2) {
        uint8_t lo = gray[i] >> 4;
        uint8_t hi = (i + 1 < count ? gray[i + 1] : 0xFF) >> 4;
        out_4bpp[i / 2] = (uint8_t)((hi << 4) | lo);
    }
}

static bool read_file_bounded(const char *path, size_t max_bytes, uint8_t **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || (size_t)st.st_size > max_bytes) {
        return false;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = (size_t)st.st_size;
    uint8_t *buf = heap_caps_malloc(n, PSRAM_CAPS);
    if (!buf) { fclose(f); return false; }
    bool ok = fread(buf, 1, n, f) == n;
    fclose(f);
    if (!ok) { free(buf); return false; }
    *out = buf;
    *out_len = n;
    return true;
}

static bool extract_cover_bytes(const char *book_path, uint8_t **encoded, size_t *encoded_len) {
    *encoded = NULL;
    *encoded_len = 0;
    if (!book_path || !book_path[0]) return false;

    const char *ext = strrchr(book_path, '.');
    if (ext && strcasecmp(ext, ".epub") == 0) {
        if (book_epub_extract_cover(book_path, NULL, 0, NULL, 0, encoded, encoded_len) == ESP_OK &&
            *encoded && *encoded_len > 0) {
            return true;
        }
    } else if (ext && strcasecmp(ext, ".cbz") == 0) {
        if (book_cbz_extract_cover(book_path, encoded, encoded_len) == ESP_OK &&
            *encoded && *encoded_len > 0) {
            return true;
        }
    }

    // 同名 .jpg / .jpeg / .png 封面文件。/ Sibling .jpg / .jpeg / .png cover file.
    if (ext && ext > book_path) {
        static const char *const exts[3] = { ".jpg", ".jpeg", ".png" };
        size_t stem_len = (size_t)(ext - book_path);
        for (int i = 0; i < 3; ++i) {
            if (stem_len + strlen(exts[i]) + 1 > 320) continue;
            char sibling[320];
            memcpy(sibling, book_path, stem_len);
            strcpy(sibling + stem_len, exts[i]);
            if (read_file_bounded(sibling, 2u * 1024u * 1024u, encoded, encoded_len)) {
                return true;
            }
        }
    }
    return false;
}

static cover_lru_slot_t *lru_find(uint64_t hash, uint32_t file_size) {
    for (int i = 0; i < COVER_LRU_CAP; ++i) {
        if (s_lru[i].valid && s_lru[i].hash == hash && s_lru[i].file_size == file_size) {
            s_lru[i].tick = ++s_lru_tick;
            return &s_lru[i];
        }
    }
    return NULL;
}

static void lru_store(uint64_t hash, uint32_t file_size, bool has_cover, const uint8_t *data) {
    int victim = 0;
    for (int i = 0; i < COVER_LRU_CAP; ++i) {
        if (!s_lru[i].valid) { victim = i; break; }
        if (s_lru[i].hash == hash) { victim = i; break; }
        if (s_lru[i].tick < s_lru[victim].tick) victim = i;
    }
    cover_lru_slot_t *slot = &s_lru[victim];
    if (has_cover && data) {
        if (!slot->data) {
            slot->data = heap_caps_malloc(BOOK_COVER_THUMB_BYTES, PSRAM_CAPS);
            if (!slot->data) return;
        }
        memcpy(slot->data, data, BOOK_COVER_THUMB_BYTES);
    }
    slot->valid = true;
    slot->has_cover = has_cover;
    slot->hash = hash;
    slot->file_size = file_size;
    slot->tick = ++s_lru_tick;
}

bool book_cover_load_thumb(const char *book_path, uint32_t file_size,
                           uint8_t out_4bpp[BOOK_COVER_THUMB_BYTES]) {
    if (!book_path || !book_path[0] || !out_4bpp) return false;
    uint64_t hash = fnv1a_64(book_path);

    // 1. 内存 LRU 命中。/ In-memory LRU hit.
    cover_lru_slot_t *cached = lru_find(hash, file_size);
    if (cached) {
        if (cached->has_cover && cached->data) {
            memcpy(out_4bpp, cached->data, BOOK_COVER_THUMB_BYTES);
            return true;
        }
        return false;
    }

    // 2. 磁盘 .covers 缓存命中。/ Disk .covers cache hit.
    char cpath[256];
    thumb_cache_path(book_path, cpath, sizeof(cpath));
    FILE *f = fopen(cpath, "rb");
    if (f) {
        cover_disk_hdr_t hdr = {0};
        bool ok = fread(&hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
                  memcmp(hdr.magic, COVER_MAGIC, 4) == 0 &&
                  hdr.version == COVER_VERSION &&
                  hdr.width == BOOK_COVER_THUMB_W &&
                  hdr.height == BOOK_COVER_THUMB_H &&
                  hdr.file_size == file_size &&
                  fread(out_4bpp, 1, BOOK_COVER_THUMB_BYTES, f) == BOOK_COVER_THUMB_BYTES;
        fclose(f);
        if (ok) {
            lru_store(hash, file_size, true, out_4bpp);
            return true;
        }
    }

    // 3. 提取封面图并解码为 144x196 4bpp。/ Extract and decode to 144x196 4bpp.
    uint8_t *encoded = NULL;
    size_t encoded_len = 0;
    if (!extract_cover_bytes(book_path, &encoded, &encoded_len)) {
        lru_store(hash, file_size, false, NULL);
        return false;
    }

    uint8_t *gray = NULL;
    uint16_t gw = 0, gh = 0;
    bool decoded = book_image_decode_cover(encoded, encoded_len,
                                           BOOK_COVER_THUMB_W, BOOK_COVER_THUMB_H,
                                           2u * 1024u * 1024u, &gray, &gw, &gh);
    free(encoded);
    if (!decoded || !gray || gw != BOOK_COVER_THUMB_W || gh != BOOK_COVER_THUMB_H) {
        free(gray);
        lru_store(hash, file_size, false, NULL);
        return false;
    }

    pack_gray8_to_4bpp(gray, (size_t)gw * gh, out_4bpp);
    free(gray);
    lru_store(hash, file_size, true, out_4bpp);

    // 写入磁盘缓存（TF 卡不可写时仅保留内存 LRU）。/ Persist to disk cache when writable.
    if (ensure_cache_dir()) {
        char tmp_path[272];
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", cpath);
        FILE *wf = fopen(tmp_path, "wb");
        if (wf) {
            cover_disk_hdr_t hdr = {
                .magic = {'R', 'C', 'O', 'V'},
                .version = COVER_VERSION,
                .width = BOOK_COVER_THUMB_W,
                .height = BOOK_COVER_THUMB_H,
                .reserved = 0,
                .file_size = file_size,
            };
            bool wrote = fwrite(&hdr, 1, sizeof(hdr), wf) == sizeof(hdr) &&
                         fwrite(out_4bpp, 1, BOOK_COVER_THUMB_BYTES, wf) == BOOK_COVER_THUMB_BYTES;
            fclose(wf);
            if (wrote) (void)rename(tmp_path, cpath);
            else (void)unlink(tmp_path);
        }
    }
    return true;
}

void book_cover_forget(const char *book_path) {
    if (!book_path || !book_path[0]) return;
    uint64_t hash = fnv1a_64(book_path);
    for (int i = 0; i < COVER_LRU_CAP; ++i) {
        if (s_lru[i].valid && s_lru[i].hash == hash) {
            s_lru[i].valid = false;
            s_lru[i].has_cover = false;
        }
    }
    char cpath[256];
    thumb_cache_path(book_path, cpath, sizeof(cpath));
    (void)unlink(cpath);
}

void book_cover_clear_lru(void) {
    for (int i = 0; i < COVER_LRU_CAP; ++i) {
        free(s_lru[i].data);
        memset(&s_lru[i], 0, sizeof(s_lru[i]));
    }
}

bool book_cover_save_lock_cache(const char *book_path, uint32_t file_size) {
    (void)file_size;
    char lpath[256];
    lock_cache_path(lpath, sizeof(lpath));

    uint8_t *encoded = NULL;
    size_t encoded_len = 0;
    if (!extract_cover_bytes(book_path, &encoded, &encoded_len)) {
        (void)unlink(lpath);
        return false;
    }

    uint8_t *gray = NULL;
    uint16_t gw = 0, gh = 0;
    bool decoded = book_image_decode_cover(encoded, encoded_len,
                                           COVER_LOCK_W, COVER_LOCK_H,
                                           2u * 1024u * 1024u, &gray, &gw, &gh);
    free(encoded);
    if (!decoded || !gray || gw != COVER_LOCK_W || gh != COVER_LOCK_H) {
        free(gray);
        (void)unlink(lpath);
        return false;
    }

    // 旋转并打包为面板物理 1216x684 4bpp 缓冲，锁屏时直接 memcpy 出图（< 40ms）。
    // Rotate and pack into physical 1216x684 4bpp layout so lockscreen memcpy renders in < 40ms.
    uint8_t *packed = heap_caps_malloc(COVER_LOCK_BYTES, PSRAM_CAPS);
    if (!packed) { free(gray); return false; }
    memset(packed, 0xFF, COVER_LOCK_BYTES);
    const int phys_w = COVER_LOCK_H; // 1216
    const int phys_h = COVER_LOCK_W; // 684
    for (int ly = 0; ly < COVER_LOCK_H; ++ly) {
        const uint8_t *row = gray + (size_t)ly * COVER_LOCK_W;
        for (int lx = 0; lx < COVER_LOCK_W; ++lx) {
            int px = ly;
            int py = phys_h - lx - 1;
            uint8_t nib = row[lx] >> 4;
            uint8_t *dst = &packed[(size_t)py * (phys_w / 2) + (size_t)(px / 2)];
            if (px & 1) *dst = (uint8_t)((*dst & 0x0F) | (nib << 4));
            else *dst = (uint8_t)((*dst & 0xF0) | nib);
        }
    }
    free(gray);

    bool ok = false;
    if (ensure_cache_dir()) {
        char tmp[272];
        snprintf(tmp, sizeof(tmp), "%s.tmp", lpath);
        FILE *wf = fopen(tmp, "wb");
        if (wf) {
            ok = fwrite(packed, 1, COVER_LOCK_BYTES, wf) == COVER_LOCK_BYTES;
            fclose(wf);
            if (ok) ok = (rename(tmp, lpath) == 0);
            else (void)unlink(tmp);
        }
    }
    free(packed);
    return ok;
}

bool book_cover_load_lock_cache(uint8_t *fb, size_t fb_bytes) {
    if (!fb || fb_bytes != COVER_LOCK_BYTES) return false;
    char lpath[256];
    lock_cache_path(lpath, sizeof(lpath));
    FILE *f = fopen(lpath, "rb");
    if (!f) return false;
    bool ok = fread(fb, 1, COVER_LOCK_BYTES, f) == COVER_LOCK_BYTES;
    fclose(f);
    return ok;
}
