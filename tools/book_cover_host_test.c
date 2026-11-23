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
    book_cover_set_cache_dir(NULL);
    unlink(png_path);
    unlink(txt_path);
    unlink(plain_txt);
    rmdir(cache_dir);
    rmdir(root);
    puts("book_cover_host_test: PASS");
    return 0;
}
