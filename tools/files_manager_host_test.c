/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TF 卡文件管理核心模块（files_ops）宿主机回归测试：验证路径穿越拦截、
 * 隐藏项过滤、512 项截断、迭代删目录、当前生效字体回退护栏、书架联动清理与壁纸限制。
 * English: Host regression tests for TF card file manager operations (files_ops):
 * path traversal guard, hidden-name filter, 512-entry cap, iterative tree delete,
 * active-font fallback guardrail, bookshelf cleanup hook, and wallpaper limits.
 */
#include "files_ops.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int s_font_fallback_calls = 0;
static int s_book_changed_calls = 0;
static char s_last_book_changed[FILES_PATH_MAX] = {0};

static void on_font_fallback(void) { s_font_fallback_calls++; }

static void on_book_changed(const char *path) {
    s_book_changed_calls++;
    if (path) {
        strncpy(s_last_book_changed, path, sizeof(s_last_book_changed) - 1);
        s_last_book_changed[sizeof(s_last_book_changed) - 1] = '\0';
    }
}

static void write_bytes(const char *path, size_t n, uint8_t fill) {
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    uint8_t buf[1024];
    memset(buf, fill, sizeof(buf));
    size_t rem = n;
    while (rem > 0) {
        size_t chunk = rem > sizeof(buf) ? sizeof(buf) : rem;
        assert(fwrite(buf, 1, chunk, fp) == chunk);
        rem -= chunk;
    }
    fclose(fp);
}

static bool path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void test_path_guards(void) {
    char out[FILES_PATH_MAX];
    assert(files_path_join_safe("/sdcard", "/sdcard", "books/test.epub", out, sizeof(out)));
    assert(strcmp(out, "/sdcard/books/test.epub") == 0);

    assert(files_path_join_safe("/sdcard", "/sdcard/books", "../fonts/a.ttf", out, sizeof(out)));
    assert(strcmp(out, "/sdcard/fonts/a.ttf") == 0);

    // Must block traversal outside root.
    assert(!files_path_join_safe("/sdcard", "/sdcard/books", "../../etc/passwd", out, sizeof(out)));
    assert(!files_path_join_safe("/sdcard", "/sdcard", "..", out, sizeof(out)));
    assert(!files_path_join_safe("/sdcard", "/sdcard", "a\\b", out, sizeof(out)));
    assert(!files_path_join_safe("/sdcard", "/sdcard", "bad\nname", out, sizeof(out)));

    // Parent computation.
    assert(!files_path_parent("/sdcard", "/sdcard", out, sizeof(out)));
    assert(files_path_parent("/sdcard", "/sdcard/books", out, sizeof(out)));
    assert(strcmp(out, "/sdcard") == 0);
    assert(files_path_parent("/sdcard", "/sdcard/books/sub", out, sizeof(out)));
    assert(strcmp(out, "/sdcard/books") == 0);

    // Covers & books helpers.
    assert(files_path_covers("/sdcard/fonts", "/sdcard/fonts/a.ttf"));
    assert(files_path_covers("/sdcard/fonts/a.ttf", "/sdcard/fonts/a.ttf"));
    assert(!files_path_covers("/sdcard/font", "/sdcard/fonts/a.ttf"));
    assert(files_is_under_books("/sdcard", "/sdcard/books/demo.txt"));
    assert(!files_is_under_books("/sdcard", "/sdcard/bookshelf/demo.txt"));
}

static void test_hidden_and_kinds(void) {
    assert(files_is_hidden_name(".covers"));
    assert(files_is_hidden_name(".notes"));
    assert(files_is_hidden_name("._appledouble"));
    assert(files_is_hidden_name(".DS_Store"));
    assert(files_is_hidden_name("System Volume Information"));
    assert(!files_is_hidden_name("books"));
    assert(!files_is_hidden_name("notes.txt"));

    assert(files_kind_for_name("demo.EPUB") == FILES_KIND_BOOK);
    assert(files_kind_for_name("comic.cbz") == FILES_KIND_BOOK);
    assert(files_kind_for_name("song.ttf") == FILES_KIND_FONT);
    assert(files_kind_for_name("pic.JPEG") == FILES_KIND_IMAGE);
    assert(files_kind_for_name("log.json") == FILES_KIND_TEXT);
    assert(files_kind_for_name("bin.dat") == FILES_KIND_OTHER);
}

static void test_listing_delete_move_guardrails(void) {
    char tmpl[] = "/tmp/pico_files_test_XXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root != NULL);

    char p[FILES_PATH_MAX];
    snprintf(p, sizeof(p), "%s/books", root);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/fonts", root);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/.covers", root);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/nested", root);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/nested/sub1", root);
    assert(mkdir(p, 0755) == 0);
    snprintf(p, sizeof(p), "%s/nested/sub1/deep.txt", root);
    write_bytes(p, 42, 'A');

    char book_a[FILES_PATH_MAX];
    snprintf(book_a, sizeof(book_a), "%s/books/novel.epub", root);
    write_bytes(book_a, 128, 'B');

    char font_a[FILES_PATH_MAX];
    snprintf(font_a, sizeof(font_a), "%s/fonts/myfont.ttf", root);
    write_bytes(font_a, 256, 'F');

    char hidden_f[FILES_PATH_MAX];
    snprintf(hidden_f, sizeof(hidden_f), "%s/._ghost", root);
    write_bytes(hidden_f, 16, 'H');

    // Default listing hides dotfiles/.covers; directories come first.
    files_entry_t entries[16];
    size_t count = 0;
    bool truncated = false;
    assert(files_list_dir(root, false, false, entries, 16, &count, &truncated));
    assert(!truncated);
    assert(count == 3); // books, fonts, nested
    assert(entries[0].is_dir && entries[1].is_dir && entries[2].is_dir);

    // Show hidden includes .covers and ._ghost.
    assert(files_list_dir(root, true, false, entries, 16, &count, &truncated));
    assert(count == 5);

    // Auto mkdir produces non-colliding directory names.
    char created1[FILES_PATH_MAX];
    char created2[FILES_PATH_MAX];
    assert(files_mkdir_auto(root, root, created1, sizeof(created1)));
    assert(files_mkdir_auto(root, root, created2, sizeof(created2)));
    assert(strcmp(created1, created2) != 0);
    assert(path_exists(created1) && path_exists(created2));

    // Tree count and iterative delete of nested folder.
    char nested[FILES_PATH_MAX];
    snprintf(nested, sizeof(nested), "%s/nested", root);
    size_t tree_count = 0;
    assert(files_count_tree(root, nested, &tree_count));
    assert(tree_count == 3); // nested, sub1, deep.txt
    assert(!files_delete_tree_safe(root, root, NULL, NULL, NULL)); // must refuse root
    assert(files_delete_tree_safe(root, nested, NULL, NULL, NULL));
    assert(!path_exists(nested));

    // Guardrail 1: moving or deleting active font triggers font fallback callback.
    s_font_fallback_calls = 0;
    char moved_font[FILES_PATH_MAX];
    assert(files_move_safe(root, font_a, created1, "myfont.ttf", font_a,
                           on_font_fallback, on_book_changed,
                           moved_font, sizeof(moved_font)));
    assert(s_font_fallback_calls == 1);
    assert(!path_exists(font_a) && path_exists(moved_font));

    // Moving without overwrite fails when target exists.
    assert(!files_move_safe(root, moved_font, created1, "myfont.ttf", NULL,
                            NULL, NULL, NULL, 0));

    // Deleting parent directory of active font also triggers font fallback.
    s_font_fallback_calls = 0;
    assert(files_delete_tree_safe(root, created1, moved_font, on_font_fallback, on_book_changed));
    assert(s_font_fallback_calls == 1);
    assert(!path_exists(created1));

    // Guardrail 2: deleting a book under <root>/books/ notifies bookshelf cleanup.
    s_book_changed_calls = 0;
    assert(files_delete_tree_safe(root, book_a, NULL, on_font_fallback, on_book_changed));
    assert(s_book_changed_calls == 1);
    assert(strcmp(s_last_book_changed, book_a) == 0);

    // Wallpaper copy: rejects non-image and > 2 MiB, accepts valid <= 2 MiB image.
    char small_png[FILES_PATH_MAX];
    snprintf(small_png, sizeof(small_png), "%s/wall.png", root);
    write_bytes(small_png, 512, 0x89);
    assert(files_set_wallpaper_from(root, small_png));
    snprintf(p, sizeof(p), "%s/screensaver.png", root);
    assert(path_exists(p));

    char big_jpg[FILES_PATH_MAX];
    snprintf(big_jpg, sizeof(big_jpg), "%s/huge.jpg", root);
    write_bytes(big_jpg, FILES_WALLPAPER_MAX + 16, 0xFF);
    assert(!files_set_wallpaper_from(root, big_jpg));

    char bad_txt[FILES_PATH_MAX];
    snprintf(bad_txt, sizeof(bad_txt), "%s/notimg.txt", root);
    write_bytes(bad_txt, 64, 'T');
    assert(!files_set_wallpaper_from(root, bad_txt));

    // 512-entry cap & truncation flag.
    char many_dir[FILES_PATH_MAX];
    snprintf(many_dir, sizeof(many_dir), "%s/many", root);
    assert(mkdir(many_dir, 0755) == 0);
    for (int i = 0; i < 520; i++) {
        snprintf(p, sizeof(p), "%s/f_%03d.txt", many_dir, i);
        write_bytes(p, (size_t)(i + 1), 'x');
    }
    files_entry_t *big_list = calloc(FILES_MAX_ENTRIES, sizeof(files_entry_t));
    assert(big_list != NULL);
    assert(files_list_dir(many_dir, false, true, big_list, FILES_MAX_ENTRIES,
                          &count, &truncated));
    assert(count == FILES_MAX_ENTRIES);
    assert(truncated == true);
    // Verify size-descending sort order.
    assert(big_list[0].size >= big_list[1].size);
    free(big_list);

    // Clean up temp tree.
    assert(files_delete_tree_safe(root, many_dir, NULL, NULL, NULL));
    assert(files_delete_tree_safe(root, created2, NULL, NULL, NULL));
    snprintf(p, sizeof(p), "%s/books", root);
    assert(files_delete_tree_safe(root, p, NULL, NULL, NULL));
    snprintf(p, sizeof(p), "%s/fonts", root);
    assert(files_delete_tree_safe(root, p, NULL, NULL, NULL));
    snprintf(p, sizeof(p), "%s/.covers", root);
    assert(files_delete_tree_safe(root, p, NULL, NULL, NULL));
    unlink(hidden_f);
    unlink(small_png);
    unlink(big_jpg);
    unlink(bad_txt);
    snprintf(p, sizeof(p), "%s/screensaver.png", root);
    unlink(p);
    assert(rmdir(root) == 0);
}

int main(void) {
    test_path_guards();
    test_hidden_and_kinds();
    test_listing_delete_move_guardrails();
    printf("files_manager_host_test: OK\n");
    return 0;
}
