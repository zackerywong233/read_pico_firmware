/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TF 卡文件管理核心文件系统操作实现（无界面依赖，可供主机测试直接编译验证）。
 * English: TF card file manager core filesystem ops (UI-free for direct host unit testing).
 */

#include "files_ops.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define TREE_STACK_MAX 64

bool files_is_hidden_name(const char *name) {
    if (!name || !name[0]) return true;
    if (name[0] == '.') return true;
    if (strcasecmp(name, "System Volume Information") == 0) return true;
    if (strcasecmp(name, "$RECYCLE.BIN") == 0) return true;
    return false;
}

files_kind_t files_kind_for_name(const char *name) {
    if (!name) return FILES_KIND_OTHER;
    const char *ext = strrchr(name, '.');
    if (!ext || !ext[1]) return FILES_KIND_OTHER;
    if (!strcasecmp(ext, ".txt") || !strcasecmp(ext, ".epub") ||
        !strcasecmp(ext, ".md") || !strcasecmp(ext, ".cbz")) {
        return FILES_KIND_BOOK;
    }
    if (!strcasecmp(ext, ".ttf") || !strcasecmp(ext, ".otf")) {
        return FILES_KIND_FONT;
    }
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg") || !strcasecmp(ext, ".png")) {
        return FILES_KIND_IMAGE;
    }
    if (!strcasecmp(ext, ".log") || !strcasecmp(ext, ".json") || !strcasecmp(ext, ".csv") ||
        !strcasecmp(ext, ".ini") || !strcasecmp(ext, ".xml") || !strcasecmp(ext, ".conf")) {
        return FILES_KIND_TEXT;
    }
    return FILES_KIND_OTHER;
}

static size_t trim_trailing_slash(const char *s, size_t len) {
    while (len > 1 && s[len - 1] == '/') --len;
    return len;
}

bool files_path_covers(const char *target, const char *candidate) {
    if (!target || !target[0] || !candidate || !candidate[0]) return false;
    size_t tlen = trim_trailing_slash(target, strlen(target));
    size_t clen = trim_trailing_slash(candidate, strlen(candidate));
    if (clen < tlen) return false;
    if (memcmp(target, candidate, tlen) != 0) return false;
    return clen == tlen || candidate[tlen] == '/';
}

bool files_is_under_books(const char *root, const char *path) {
    if (!root || !root[0] || !path || !path[0]) return false;
    char books_dir[FILES_PATH_MAX];
    size_t rlen = trim_trailing_slash(root, strlen(root));
    if (rlen + 7 >= sizeof(books_dir)) return false;
    memcpy(books_dir, root, rlen);
    strcpy(books_dir + rlen, "/books");
    return files_path_covers(books_dir, path) && strlen(path) > strlen(books_dir);
}

bool files_path_join_safe(const char *root, const char *cur, const char *rel,
                          char *out, size_t cap) {
    if (!root || !root[0] || !out || cap < 2) return false;
    size_t rlen = trim_trailing_slash(root, strlen(root));
    if (rlen == 0 || root[0] != '/' || rlen >= cap) return false;

    // 拼接原始候选串。/ Assemble raw candidate path.
    char raw[FILES_PATH_MAX * 2];
    if (rel && rel[0] == '/') {
        if (strlen(rel) >= sizeof(raw)) return false;
        strcpy(raw, rel);
    } else {
        const char *base = (cur && cur[0]) ? cur : root;
        size_t blen = trim_trailing_slash(base, strlen(base));
        if (rel && rel[0]) {
            if (blen + 1 + strlen(rel) >= sizeof(raw)) return false;
            memcpy(raw, base, blen);
            raw[blen] = '/';
            strcpy(raw + blen + 1, rel);
        } else {
            if (blen >= sizeof(raw)) return false;
            memcpy(raw, base, blen);
            raw[blen] = '\0';
        }
    }

    // 拒绝反斜杠与控制字符。/ Reject backslashes and control characters.
    for (const unsigned char *p = (const unsigned char *)raw; *p; ++p) {
        if (*p < 32 || *p == '\\') return false;
    }

    // 逐段归一化。/ Normalize path segments.
    char norm[FILES_PATH_MAX];
    size_t used = 0;
    const char *p = raw;
    while (*p) {
        while (*p == '/') ++p;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') ++p;
        size_t slen = (size_t)(p - seg);
        if (slen == 1 && seg[0] == '.') continue;
        if (slen == 2 && seg[0] == '.' && seg[1] == '.') {
            while (used > 0 && norm[used - 1] != '/') --used;
            if (used > 0) --used; // 去掉斜杠。/ Drop slash.
            continue;
        }
        if (used + 1 + slen >= sizeof(norm)) return false;
        norm[used++] = '/';
        memcpy(norm + used, seg, slen);
        used += slen;
    }
    if (used == 0) {
        norm[used++] = '/';
    }
    norm[used] = '\0';

    // 强制停留在 root 之内，严防逃逸。/ Enforce containment strictly within root.
    if (used < rlen || memcmp(norm, root, rlen) != 0) return false;
    if (used > rlen && norm[rlen] != '/') return false;
    if (used >= cap) return false;
    memcpy(out, norm, used + 1);
    return true;
}

bool files_path_parent(const char *root, const char *cur, char *out, size_t cap) {
    if (!root || !cur || !out || cap < 2) return false;
    char norm_cur[FILES_PATH_MAX];
    if (!files_path_join_safe(root, cur, NULL, norm_cur, sizeof(norm_cur))) return false;
    size_t rlen = trim_trailing_slash(root, strlen(root));
    if (strlen(norm_cur) <= rlen) return false;
    return files_path_join_safe(root, norm_cur, "..", out, cap);
}

static bool s_sort_by_size;
static int compare_entries(const void *a, const void *b) {
    const files_entry_t *x = (const files_entry_t *)a;
    const files_entry_t *y = (const files_entry_t *)b;
    if (x->is_dir != y->is_dir) return x->is_dir ? -1 : 1;
    if (s_sort_by_size && !x->is_dir && x->size != y->size) {
        return x->size > y->size ? -1 : 1;
    }
    return strcasecmp(x->name, y->name);
}

bool files_list_dir(const char *dir_path, bool show_hidden, bool sort_by_size,
                    files_entry_t *out_items, size_t max_items,
                    size_t *out_count, bool *out_truncated) {
    if (out_count) *out_count = 0;
    if (out_truncated) *out_truncated = false;
    if (!dir_path || !out_items || !max_items || !out_count) return false;
    if (max_items > FILES_MAX_ENTRIES) max_items = FILES_MAX_ENTRIES;

    DIR *d = opendir(dir_path);
    if (!d) return false;

    size_t count = 0;
    bool truncated = false;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (!show_hidden && files_is_hidden_name(name)) continue;
        if (count >= max_items) {
            truncated = true;
            break;
        }
        char full[FILES_PATH_MAX];
        if (!files_path_join_safe(dir_path, dir_path, name, full, sizeof(full))) continue;
        struct stat st;
        if (stat(full, &st) != 0) continue;
        files_entry_t *item = &out_items[count++];
        memset(item, 0, sizeof(*item));
        snprintf(item->name, sizeof(item->name), "%.*s", (int)(sizeof(item->name) - 1), name);
        item->is_dir = S_ISDIR(st.st_mode);
        item->size = item->is_dir ? 0 : (st.st_size > 0 ? (uint32_t)st.st_size : 0);
        item->kind = item->is_dir ? FILES_KIND_DIR : files_kind_for_name(name);
    }
    closedir(d);

    if (count > 1) {
        s_sort_by_size = sort_by_size;
        qsort(out_items, count, sizeof(*out_items), compare_entries);
    }
    *out_count = count;
    if (out_truncated) *out_truncated = truncated;
    return true;
}

typedef struct {
    char path[FILES_PATH_MAX];
    bool expanded;
} tree_frame_t;

bool files_count_tree(const char *root, const char *target, size_t *out_count) {
    if (out_count) *out_count = 0;
    if (!root || !target || !out_count) return false;
    char norm[FILES_PATH_MAX];
    if (!files_path_join_safe(root, target, NULL, norm, sizeof(norm))) return false;
    struct stat st;
    if (stat(norm, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) { *out_count = 1; return true; }

    tree_frame_t *stack = calloc(TREE_STACK_MAX, sizeof(*stack));
    if (!stack) return false;
    int top = 0;
    snprintf(stack[top++].path, FILES_PATH_MAX, "%s", norm);
    size_t total = 0;

    while (top > 0) {
        tree_frame_t cur = stack[--top];
        total++;
        DIR *d = opendir(cur.path);
        if (!d) continue;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            char child[FILES_PATH_MAX];
            if (!files_path_join_safe(root, cur.path, ent->d_name, child, sizeof(child))) continue;
            struct stat cst;
            if (stat(child, &cst) != 0) continue;
            if (S_ISDIR(cst.st_mode)) {
                if (top < TREE_STACK_MAX) {
                    snprintf(stack[top].path, FILES_PATH_MAX, "%s", child);
                    stack[top].expanded = false;
                    top++;
                } else {
                    total++;
                }
            } else {
                total++;
            }
        }
        closedir(d);
    }
    free(stack);
    *out_count = total;
    return true;
}

bool files_delete_tree_safe(const char *root, const char *target,
                            const char *active_font_path,
                            files_font_fallback_cb_t on_font_fallback,
                            files_book_changed_cb_t on_book_changed) {
    if (!root || !target) return false;
    char norm[FILES_PATH_MAX];
    if (!files_path_join_safe(root, target, NULL, norm, sizeof(norm))) return false;
    // 禁止删除根目录本身。/ Never delete root itself.
    if (strlen(norm) <= trim_trailing_slash(root, strlen(root))) return false;

    // 安全护栏 1：若删除目标覆盖当前生效的 TF 字体，先回退内置字体再删。
    // Guardrail 1: fall back to built-in font before deleting the active TF font.
    if (active_font_path && active_font_path[0] && files_path_covers(norm, active_font_path)) {
        if (on_font_fallback) on_font_fallback();
    }

    struct stat st;
    if (stat(norm, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(norm) != 0) return false;
        if (on_book_changed && files_is_under_books(root, norm)) on_book_changed(norm);
        return true;
    }

    // 迭代显式栈后序删除目录树（零递归，防止 ESP32 栈溢出）。
    // Iterative explicit-stack post-order directory removal (zero recursion).
    tree_frame_t *stack = calloc(TREE_STACK_MAX, sizeof(*stack));
    if (!stack) return false;
    int top = 0;
    snprintf(stack[top].path, FILES_PATH_MAX, "%s", norm);
    stack[top].expanded = false;
    top++;
    bool ok = true;

    while (top > 0) {
        tree_frame_t *cur = &stack[top - 1];
        if (cur->expanded) {
            if (rmdir(cur->path) != 0) ok = false;
            top--;
            continue;
        }
        cur->expanded = true;
        DIR *d = opendir(cur->path);
        if (!d) { ok = false; top--; continue; }
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
            char child[FILES_PATH_MAX];
            if (!files_path_join_safe(root, cur->path, ent->d_name, child, sizeof(child))) {
                ok = false;
                continue;
            }
            struct stat cst;
            if (stat(child, &cst) != 0) { ok = false; continue; }
            if (S_ISDIR(cst.st_mode)) {
                if (top < TREE_STACK_MAX) {
                    snprintf(stack[top].path, FILES_PATH_MAX, "%s", child);
                    stack[top].expanded = false;
                    top++;
                } else {
                    ok = false;
                }
            } else {
                if (unlink(child) != 0) ok = false;
                else if (on_book_changed && files_is_under_books(root, child)) on_book_changed(child);
            }
        }
        closedir(d);
    }
    free(stack);
    return ok;
}

bool files_move_safe(const char *root, const char *src_path, const char *dst_dir,
                     const char *dst_name, const char *active_font_path,
                     files_font_fallback_cb_t on_font_fallback,
                     files_book_changed_cb_t on_book_changed,
                     char *out_dst_path, size_t out_cap) {
    if (!root || !src_path || !dst_dir || !dst_name || !dst_name[0]) return false;
    char norm_src[FILES_PATH_MAX], norm_dir[FILES_PATH_MAX], norm_dst[FILES_PATH_MAX];
    if (!files_path_join_safe(root, src_path, NULL, norm_src, sizeof(norm_src))) return false;
    if (!files_path_join_safe(root, dst_dir, NULL, norm_dir, sizeof(norm_dir))) return false;
    if (!files_path_join_safe(root, norm_dir, dst_name, norm_dst, sizeof(norm_dst))) return false;
    if (strcmp(norm_src, norm_dst) == 0) return false;
    // 禁止把目录移入自身内部。/ Reject moving a directory into itself.
    if (files_path_covers(norm_src, norm_dst)) return false;

    // 拒绝覆盖已存在目标。/ Refuse to overwrite an existing destination.
    struct stat st;
    if (stat(norm_dst, &st) == 0) return false;
    (void)mkdir(norm_dir, 0775);

    if (active_font_path && active_font_path[0] && files_path_covers(norm_src, active_font_path)) {
        if (on_font_fallback) on_font_fallback();
    }
    if (rename(norm_src, norm_dst) != 0) return false;

    if (on_book_changed) {
        if (files_is_under_books(root, norm_src)) on_book_changed(norm_src);
        else if (files_is_under_books(root, norm_dst)) on_book_changed(norm_dst);
    }
    if (out_dst_path && out_cap) {
        snprintf(out_dst_path, out_cap, "%s", norm_dst);
    }
    return true;
}

bool files_mkdir_auto(const char *root, const char *cur_dir, char *out_path, size_t cap) {
    if (!root || !cur_dir) return false;
    for (int i = 1; i <= 99; ++i) {
        char name[48];
        if (i == 1) snprintf(name, sizeof(name), "新建文件夹");
        else snprintf(name, sizeof(name), "新建文件夹_%d", i);
        char candidate[FILES_PATH_MAX];
        if (!files_path_join_safe(root, cur_dir, name, candidate, sizeof(candidate))) return false;
        struct stat st;
        if (stat(candidate, &st) == 0) continue;
        if (mkdir(candidate, 0775) == 0) {
            if (out_path && cap) snprintf(out_path, cap, "%s", candidate);
            return true;
        }
        return false;
    }
    return false;
}

bool files_set_wallpaper_from(const char *root, const char *src_path) {
    if (!root || !src_path) return false;
    char norm_src[FILES_PATH_MAX];
    if (!files_path_join_safe(root, src_path, NULL, norm_src, sizeof(norm_src))) return false;
    if (files_kind_for_name(norm_src) != FILES_KIND_IMAGE) return false;

    struct stat st;
    if (stat(norm_src, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || (size_t)st.st_size > FILES_WALLPAPER_MAX) {
        return false;
    }
    const char *ext = strrchr(norm_src, '.');
    const char *dst_name = (ext && !strcasecmp(ext, ".png")) ? "screensaver.png" : "screensaver.jpg";
    char dst[FILES_PATH_MAX], tmp[FILES_PATH_MAX + 8];
    if (!files_path_join_safe(root, root, dst_name, dst, sizeof(dst))) return false;
    if (strcmp(norm_src, dst) == 0) return true;
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);

    FILE *in = fopen(norm_src, "rb");
    if (!in) return false;
    FILE *out = fopen(tmp, "wb");
    if (!out) { fclose(in); return false; }

    uint8_t buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    if (ferror(in)) ok = false;
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) { (void)unlink(tmp); return false; }

    // 清理另一种扩展名的旧壁纸避免冲突。/ Remove other wallpaper extension if present.
    const char *other = !strcmp(dst_name, "screensaver.png") ? "screensaver.jpg" : "screensaver.png";
    char other_path[FILES_PATH_MAX];
    if (files_path_join_safe(root, root, other, other_path, sizeof(other_path))) {
        (void)unlink(other_path);
    }
    if (rename(tmp, dst) != 0) { (void)unlink(tmp); return false; }
    return true;
}
