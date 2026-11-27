/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：封面提取、.covers 4bpp 缩略图缓存、删书清理、锁屏封面缓存与 Bayer 抖动主机单测。
 * English: Host unit tests for cover extraction, .covers 4bpp cache, forget cleanup, lockscreen cache, and Bayer dithering.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "book_cover.h"
#include "book_epub.h"
#include "book_image.h"
#include "book_source_internal.h"

int image_fail_after = -1;

// 最小有效 4x4 灰度 PNG（由标准 zlib/CRC32 生成）。/ Minimal valid 4x4 grayscale PNG.
static const uint8_t k_test_png[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04,
    0x08, 0x00, 0x00, 0x00, 0x00, 0x8C, 0x9A, 0xC1, 0xA2, 0x00, 0x00, 0x00,
    0x0E, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x60, 0xF8, 0x0F, 0x84,
    0xA8, 0x04, 0x00, 0x4B, 0xC8, 0x07, 0xF9, 0x6B, 0xDE, 0x2F, 0x7C, 0x00,
    0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82
};

static void write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(data, 1, len, f) == len);
    fclose(f);
}

static uint32_t test_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static void put_u16(FILE *f, uint16_t v) { uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; assert(fwrite(b, 1, 2, f) == 2); }
static void put_u32(FILE *f, uint32_t v) { uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)}; assert(fwrite(b, 1, 4, f) == 4); }

static uint32_t write_stored_epub(const char *path) {
    static const char container_xml[] =
        "<?xml version=\"1.0\"?>"
        "<container version=\"1.0\" xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">"
        "<rootfiles><rootfile full-path=\"OEBPS/content.opf\"/></rootfiles></container>";
    static const char opf_xml[] =
        "<?xml version=\"1.0\"?>"
        "<package xmlns=\"http://www.idpf.org/2007/opf\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\">"
        "<metadata><dc:title>Cover Test</dc:title><dc:creator>Pico Author</dc:creator>"
        "<meta name=\"cover\" content=\"cov-id\"/></metadata>"
        "<manifest><item id=\"cov-id\" href=\"cover.png\" media-type=\"image/png\"/></manifest>"
        "</package>";
    struct { const char *name; const uint8_t *data; size_t len; uint32_t off; uint32_t crc; } ents[3] = {
        { "META-INF/container.xml", (const uint8_t *)container_xml, sizeof(container_xml) - 1, 0, 0 },
        { "OEBPS/content.opf", (const uint8_t *)opf_xml, sizeof(opf_xml) - 1, 0, 0 },
        { "OEBPS/cover.png", k_test_png, sizeof(k_test_png), 0, 0 },
    };
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    for (int i = 0; i < 3; ++i) {
        ents[i].off = (uint32_t)ftell(f);
        ents[i].crc = test_crc32(ents[i].data, ents[i].len);
        uint16_t nlen = (uint16_t)strlen(ents[i].name);
        put_u32(f, 0x04034B50u); put_u16(f, 20); put_u16(f, 0); put_u16(f, 0);
        put_u16(f, 0); put_u16(f, 0); put_u32(f, ents[i].crc);
        put_u32(f, (uint32_t)ents[i].len); put_u32(f, (uint32_t)ents[i].len);
        put_u16(f, nlen); put_u16(f, 0);
        assert(fwrite(ents[i].name, 1, nlen, f) == nlen);
        assert(fwrite(ents[i].data, 1, ents[i].len, f) == ents[i].len);
    }
    uint32_t cd_off = (uint32_t)ftell(f);
    for (int i = 0; i < 3; ++i) {
        uint16_t nlen = (uint16_t)strlen(ents[i].name);
        put_u32(f, 0x02014B50u); put_u16(f, 20); put_u16(f, 20); put_u16(f, 0); put_u16(f, 0);
        put_u16(f, 0); put_u16(f, 0); put_u32(f, ents[i].crc);
        put_u32(f, (uint32_t)ents[i].len); put_u32(f, (uint32_t)ents[i].len);
        put_u16(f, nlen); put_u16(f, 0); put_u16(f, 0); put_u16(f, 0); put_u16(f, 0);
        put_u32(f, 0); put_u32(f, ents[i].off);
        assert(fwrite(ents[i].name, 1, nlen, f) == nlen);
    }
    uint32_t cd_size = (uint32_t)ftell(f) - cd_off;
    put_u32(f, 0x06054B50u); put_u16(f, 0); put_u16(f, 0); put_u16(f, 3); put_u16(f, 3);
    put_u32(f, cd_size); put_u32(f, cd_off); put_u16(f, 0);
    uint32_t total = (uint32_t)ftell(f);
    fclose(f);
    return total;
}

int main(void) {
    char root[128], cache_dir[160], txt_path[192], png_path[192], plain_txt[192];
    snprintf(root, sizeof(root), "/tmp/book-cover-test-%d", (int)getpid());
    assert(mkdir(root, 0700) == 0);
    snprintf(cache_dir, sizeof(cache_dir), "%s/.covers", root);
    book_cover_set_cache_dir(cache_dir);

    snprintf(plain_txt, sizeof(plain_txt), "%s/plain.txt", root);
    write_file(plain_txt, "hello", 5);
    uint8_t thumb[BOOK_COVER_THUMB_BYTES];
    assert(!book_cover_load_thumb(plain_txt, 5, thumb));

    snprintf(txt_path, sizeof(txt_path), "%s/novel.txt", root);
    snprintf(png_path, sizeof(png_path), "%s/novel.png", root);
    write_file(txt_path, "story", 5);
    write_file(png_path, k_test_png, sizeof(k_test_png));
    memset(thumb, 0xAA, sizeof(thumb));
    assert(book_cover_load_thumb(txt_path, 5, thumb));
    assert(unlink(png_path) == 0);
    book_cover_clear_lru();
    uint8_t thumb2[BOOK_COVER_THUMB_BYTES];
    assert(book_cover_load_thumb(txt_path, 5, thumb2));
    assert(memcmp(thumb, thumb2, sizeof(thumb)) == 0);

    book_cover_clear_lru();
    assert(!book_cover_load_thumb(txt_path, 999, thumb2));

    book_cover_forget(txt_path);
    assert(!book_cover_load_thumb(txt_path, 5, thumb2));

    write_file(png_path, k_test_png, sizeof(k_test_png));
    assert(book_cover_save_lock_cache(txt_path, 5));
    size_t fb_sz = 684 * 1216 / 2;
    uint8_t *fb = malloc(fb_sz);
    assert(fb != NULL);
    memset(fb, 0x55, fb_sz);
    assert(book_cover_load_lock_cache(fb, fb_sz));
    free(fb);

    assert(!book_cover_save_lock_cache(plain_txt, 5));
    uint8_t dummy[16];
    assert(!book_cover_load_lock_cache(dummy, sizeof(dummy)));

    // 4x4 Bayer 有序抖动与全屏适配解码验证。/ 4x4 Bayer dithering and fit-decode check.
    uint8_t grad[16 * 16];
    memset(grad, 127, sizeof(grad));
    book_image_dither_bayer4(grad, 16, 16);
    bool saw_low = false, saw_high = false;
    for (size_t i = 0; i < sizeof(grad); ++i) {
        assert(grad[i] % 17 == 0);
        if (grad[i] < 127) saw_low = true;
        if (grad[i] > 127) saw_high = true;
    }
    assert(saw_low && saw_high);

    uint8_t *fit_pix = NULL;
    uint16_t fw = 0, fh = 0;
    assert(book_image_decode_fit(k_test_png, sizeof(k_test_png), 684, 1216, 2u * 1024u * 1024u, true, &fit_pix, &fw, &fh));
    assert(fit_pix != NULL && fw == 4 && fh == 4);
    for (size_t i = 0; i < (size_t)fw * fh; ++i) assert(fit_pix[i] % 17 == 0);
    free(fit_pix);

    book_cover_clear_lru();
    // EPUB OPF 封面提取与书架缩略图缓存往返。/ EPUB OPF cover extraction and thumbnail round-trip.
    char epub_path[192];
    snprintf(epub_path, sizeof(epub_path), "%s/sample.epub", root);
    uint32_t epub_size = write_stored_epub(epub_path);
    char epub_title[64] = {0}, epub_author[64] = {0};
    uint8_t *epub_raw = NULL;
    size_t epub_raw_len = 0;
    assert(book_epub_extract_cover(epub_path, epub_title, sizeof(epub_title),
                                   epub_author, sizeof(epub_author),
                                   &epub_raw, &epub_raw_len) == ESP_OK);
    assert(strcmp(epub_title, "Cover Test") == 0);
    assert(strcmp(epub_author, "Pico Author") == 0);
    assert(epub_raw != NULL && epub_raw_len == sizeof(k_test_png));
    assert(memcmp(epub_raw, k_test_png, sizeof(k_test_png)) == 0);
    free(epub_raw);
    assert(book_cover_load_thumb(epub_path, epub_size, thumb));
    book_cover_forget(epub_path);
    unlink(epub_path);

    book_cover_clear_lru();
    book_cover_set_cache_dir(NULL);
    unlink(png_path);
    unlink(txt_path);
    unlink(plain_txt);
    rmdir(cache_dir);
    rmdir(root);
    puts("book_cover_host_test: PASS");
    return 0;
}
