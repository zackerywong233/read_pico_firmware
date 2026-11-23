/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TF 卡文件管理核心文件系统操作与安全护栏（路径归一化防穿越、512 项上限、
 * 迭代显式栈删目录、生效字体回退保护、书架联动清理）。
 * English: TF card file manager core operations and safety guardrails (path normalization,
 * 512-entry cap, iterative non-recursive tree delete, active-font fallback, bookshelf sync).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FILES_MAX_ENTRIES 512
#define FILES_PATH_MAX 320
#define FILES_NAME_MAX 128
#define FILES_TEXT_PREVIEW_MAX (256u * 1024u)
#define FILES_WALLPAPER_MAX (2u * 1024u * 1024u)

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FILES_KIND_DIR = 0,
    FILES_KIND_BOOK,
    FILES_KIND_FONT,
    FILES_KIND_IMAGE,
    FILES_KIND_TEXT,
    FILES_KIND_OTHER,
} files_kind_t;

typedef struct {
    char name[FILES_NAME_MAX];
    uint32_t size;
    files_kind_t kind;
    bool is_dir;
    bool selected;
} files_entry_t;

typedef void (*files_font_fallback_cb_t)(void);
typedef void (*files_book_changed_cb_t)(const char *book_path);

/// 判断是否为默认隐藏的系统/内部条目（.covers、.notes、System Volume Information、._* 及点文件）。
/// Check if an entry is hidden by default (.covers, .notes, System Volume Information, ._*, dotfiles).
bool files_is_hidden_name(const char *name);

/// 根据文件名扩展名判定文件类别。/ Classify a regular file by extension.
files_kind_t files_kind_for_name(const char *name);

/// 严格路径归一化并校验结果停留在 root 之内，防 .. 穿越与非法字符。
/// Normalize and join cur + rel strictly inside root, rejecting traversal and invalid characters.
bool files_path_join_safe(const char *root, const char *cur, const char *rel,
                          char *out, size_t cap);

/// 计算上级目录；已在 root 时返回 false。/ Compute parent directory; false when already at root.
bool files_path_parent(const char *root, const char *cur, char *out, size_t cap);

/// 检查 target 是否等于 candidate 或是其祖先目录。/ Check whether target equals or contains candidate.
bool files_path_covers(const char *target, const char *candidate);

/// 检查路径是否位于 <root>/books/ 下。/ Check whether path lies under <root>/books/.
bool files_is_under_books(const char *root, const char *path);

/// 枚举目录（最多 max_items <= FILES_MAX_ENTRIES 项），目录在前，支持按名称或大小排序。
/// List directory entries bounded by max_items (<= 512), directories first, sorted by name or size.
bool files_list_dir(const char *dir_path, bool show_hidden, bool sort_by_size,
                    files_entry_t *out_items, size_t max_items,
                    size_t *out_count, bool *out_truncated);

/// 使用迭代显式栈统计目标（含自身与全部子项）条目数，防栈溢出。
/// Count items in a file or directory tree using an explicit iterative stack.
bool files_count_tree(const char *root, const char *target, size_t *out_count);

/// 安全删除文件或非空目录（迭代显式栈）；触发当前字体保护与书架联动清理回调。
/// Safely delete a file or non-empty directory iteratively, running font fallback and book cleanup callbacks.
bool files_delete_tree_safe(const char *root, const char *target,
                            const char *active_font_path,
                            files_font_fallback_cb_t on_font_fallback,
                            files_book_changed_cb_t on_book_changed);

/// 同卷零拷贝移动（rename）；目标不可覆盖已存在项，同步执行字体保护与书架联动。
/// Same-volume zero-copy move (rename) without overwrite, with font and bookshelf guards.
bool files_move_safe(const char *root, const char *src_path, const char *dst_dir,
                     const char *dst_name, const char *active_font_path,
                     files_font_fallback_cb_t on_font_fallback,
                     files_book_changed_cb_t on_book_changed,
                     char *out_dst_path, size_t out_cap);

/// 在当前目录自动创建不重名的新建文件夹（新建文件夹 / 新建文件夹_2 ...）。
/// Create a unique new folder inside cur_dir.
bool files_mkdir_auto(const char *root, const char *cur_dir, char *out_path, size_t cap);

/// 将图片（<= 2 MiB）复制为锁屏壁纸 <root>/screensaver.<ext>。
/// Copy an image (<= 2 MiB) to <root>/screensaver.<ext> as the lockscreen wallpaper.
bool files_set_wallpaper_from(const char *root, const char *src_path);

#ifdef __cplusplus
}
#endif
