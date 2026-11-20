/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：在每个图书存储根下保存独立笔记文件，并迁移旧 NVS 记录。
 * English: Store sidecar notes under each book root and migrate legacy NVS records.
 *
 * 冻结：读写均校验完整路径与文件大小；替换通过临时文件和备份恢复，条目与摘录长度受可用存储限制。
 * Frozen: Validate full path and file size on every read/write; recover replacements through a temp file and backup, with item count and excerpt length bounded by available storage.
 */
#include "book_notes.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nvs.h"

#define NOTES_DIR_NAME ".pico_notes"
#define NOTES_HEADER_SIZE 12
#define NOTES_FORMAT_VERSION 2
#define NOTES_LEGACY_VERSION 1
#define NOTES_RECORD_BOOKMARK 1
#define NOTES_RECORD_EXCERPT 2
#define NOTES_BOOKMARK_SIZE 8

#define LEGACY_NS "rp_notes"
#define LEGACY_HEADER_SIZE 16
#define LEGACY_BOOKMARK_MAX 8
#define LEGACY_EXCERPT_MAX 6
#define LEGACY_EXCERPT_TEXT_MAX 176
#define LEGACY_BOOKMARK_SIZE 8
#define LEGACY_EXCERPT_HEADER_SIZE 8
#define LEGACY_BLOB_MAX (LEGACY_HEADER_SIZE + BOOK_STORE_PATH_MAX + LEGACY_BOOKMARK_MAX * LEGACY_BOOKMARK_SIZE + \
                         LEGACY_EXCERPT_MAX * (LEGACY_EXCERPT_HEADER_SIZE + LEGACY_EXCERPT_TEXT_MAX))

typedef struct {
    char directory[BOOK_STORE_PATH_MAX];
    char data[BOOK_STORE_PATH_MAX];
    char temporary[BOOK_STORE_PATH_MAX];
    char backup[BOOK_STORE_PATH_MAX];
} note_paths_t;

static bool valid_path(const char *path) {
    if (!path || !path[0] || strnlen(path, BOOK_STORE_PATH_MAX) >= BOOK_STORE_PATH_MAX) return false;
    return !strncmp(path, "/sdcard/books/", sizeof("/sdcard/books/") - 1) ||
           !strncmp(path, "/flash/books/", sizeof("/flash/books/") - 1);
}

static uint32_t path_hash(const char *path) {
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        hash = (hash ^ *p) * UINT32_C(16777619);
    return hash;
}

static bool make_paths(const char *path, note_paths_t *out) {
    const char *root;
    if (!strncmp(path, "/sdcard/books/", sizeof("/sdcard/books/") - 1)) root = "/sdcard/books/" NOTES_DIR_NAME;
    else if (!strncmp(path, "/flash/books/", sizeof("/flash/books/") - 1)) root = "/flash/books/" NOTES_DIR_NAME;
    else return false;
    uint32_t hash = path_hash(path);
    if (snprintf(out->directory, sizeof(out->directory), "%s", root) >= (int)sizeof(out->directory) ||
        snprintf(out->data, sizeof(out->data), "%s/n_%08" PRIx32 ".bin", root, hash) >= (int)sizeof(out->data) ||
        snprintf(out->temporary, sizeof(out->temporary), "%s/n_%08" PRIx32 ".tmp", root, hash) >= (int)sizeof(out->temporary) ||
        snprintf(out->backup, sizeof(out->backup), "%s/n_%08" PRIx32 ".bak", root, hash) >= (int)sizeof(out->backup))
        return false;
    return true;
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i)); }

static esp_err_t ensure_directory(const char *directory) {
    if (mkdir(directory, 0777) == 0 || errno == EEXIST) {
        struct stat st;
        if (stat(directory, &st) == 0 && S_ISDIR(st.st_mode)) return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t sidecar_owner(const char *file_path, const char *expected_path, bool *exists) {
    struct stat st;
    if (stat(file_path, &st) != 0) {
        if (errno == ENOENT) { *exists = false; return ESP_OK; }
        return ESP_FAIL;
    }
    *exists = true;
    FILE *file = fopen(file_path, "rb");
    if (!file) return ESP_FAIL;
    uint8_t header[NOTES_HEADER_SIZE];
    bool valid = fread(header, 1, sizeof(header), file) == sizeof(header) &&
                 !memcmp(header, "RBNF", 4) &&
                 (header[4] == NOTES_FORMAT_VERSION || header[4] == NOTES_LEGACY_VERSION);
    size_t path_len = valid ? get16(header + 5) : 0;
    char stored[BOOK_STORE_PATH_MAX];
    valid = valid && path_len >= 2 && path_len <= sizeof(stored) &&
            fread(stored, 1, path_len, file) == path_len && stored[path_len - 1] == 0 &&
            strnlen(stored, path_len) == path_len - 1;
    fclose(file);
    if (!valid) return ESP_ERR_INVALID_RESPONSE;
    return strcmp(stored, expected_path) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static esp_err_t recover_backup(const note_paths_t *paths, const char *path) {
    bool has_data = false, has_backup = false;
    esp_err_t err = sidecar_owner(paths->data, path, &has_data);
    if (err != ESP_OK) return err;
    err = sidecar_owner(paths->backup, path, &has_backup);
    if (err != ESP_OK) return err;
    if (has_data) {
        if (has_backup) (void)unlink(paths->backup);
        (void)unlink(paths->temporary);
        return ESP_OK;
    }
    if (has_backup && rename(paths->backup, paths->data) != 0) return ESP_FAIL;
    (void)unlink(paths->temporary);
    return ESP_OK;
}

static void make_legacy_key(const char *path, char key[11]) {
    snprintf(key, 11, "n_%08" PRIx32, path_hash(path));
}

static bool legacy_position_valid(const uint8_t *p) { return p[6] <= 100; }

static esp_err_t legacy_parse(const char *path, uint32_t file_size, book_notes_t *out,
                              const uint8_t *blob, size_t size) {
    if (size < LEGACY_HEADER_SIZE + 2 || size > LEGACY_BLOB_MAX || memcmp(blob, "RBN", 3) || blob[3] != 1)
        return ESP_ERR_INVALID_RESPONSE;
    size_t path_len = get16(blob + 4);
    uint8_t bookmarks = blob[6], excerpts = blob[7];
    if (path_len < 2 || path_len > BOOK_STORE_PATH_MAX || bookmarks > LEGACY_BOOKMARK_MAX ||
        excerpts > LEGACY_EXCERPT_MAX || path_len > size - LEGACY_HEADER_SIZE ||
        strnlen((const char *)blob + LEGACY_HEADER_SIZE, path_len) != path_len - 1)
        return ESP_ERR_INVALID_RESPONSE;
    if (strcmp((const char *)blob + LEGACY_HEADER_SIZE, path)) return ESP_ERR_INVALID_STATE;
    if (get32(blob + 8) != file_size) return ESP_ERR_INVALID_STATE;

    book_notes_t result = {0};
    if (bookmarks) result.bookmarks = calloc(bookmarks, sizeof(*result.bookmarks));
    if (excerpts) result.excerpts = calloc(excerpts, sizeof(*result.excerpts));
    if ((bookmarks && !result.bookmarks) || (excerpts && !result.excerpts)) {
        book_notes_free(&result);
        return ESP_ERR_NO_MEM;
    }
    size_t at = LEGACY_HEADER_SIZE + path_len;
    for (unsigned i = 0; i < bookmarks; ++i) {
        if (size - at < LEGACY_BOOKMARK_SIZE || !legacy_position_valid(blob + at)) {
            book_notes_free(&result);
            return ESP_ERR_INVALID_RESPONSE;
        }
        result.bookmarks[i] = (book_note_position_t){get16(blob + at), get32(blob + at + 2), blob[at + 6]};
        at += LEGACY_BOOKMARK_SIZE;
    }
    for (unsigned i = 0; i < excerpts; ++i) {
        if (size - at < LEGACY_EXCERPT_HEADER_SIZE || !legacy_position_valid(blob + at)) {
            book_notes_free(&result);
            return ESP_ERR_INVALID_RESPONSE;
        }
        size_t len = blob[at + 7];
        if (!len || len > LEGACY_EXCERPT_TEXT_MAX || len > size - at - LEGACY_EXCERPT_HEADER_SIZE ||
            memchr(blob + at + LEGACY_EXCERPT_HEADER_SIZE, 0, len)) {
            book_notes_free(&result);
            return ESP_ERR_INVALID_RESPONSE;
        }
        result.excerpts[i].position = (book_note_position_t){get16(blob + at), get32(blob + at + 2), blob[at + 6]};
        ++result.excerpt_count;
        result.excerpts[i].text = malloc(len + 1);
        if (!result.excerpts[i].text) {
            book_notes_free(&result);
            return ESP_ERR_NO_MEM;
        }
        memcpy(result.excerpts[i].text, blob + at + LEGACY_EXCERPT_HEADER_SIZE, len);
        result.excerpts[i].text[len] = 0;
        result.excerpts[i].text_len = len;
        at += LEGACY_EXCERPT_HEADER_SIZE + len;
    }
    if (at != size) {
        book_notes_free(&result);
        return ESP_ERR_INVALID_RESPONSE;
    }
    result.bookmark_count = bookmarks;
    *out = result;
    return ESP_OK;
}

static esp_err_t legacy_load(const char *path, uint32_t file_size, book_notes_t *out) {
    char key[11];
    make_legacy_key(path, key);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LEGACY_NS, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    size_t size = LEGACY_BLOB_MAX;
    uint8_t *blob = malloc(size);
    if (!blob) { nvs_close(handle); return ESP_ERR_NO_MEM; }
    err = nvs_get_blob(handle, key, blob, &size);
    nvs_close(handle);
    if (err == ESP_OK) err = legacy_parse(path, file_size, out, blob, size);
    else if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_ERR_NOT_FOUND;
    free(blob);
    return err;
}

static esp_err_t legacy_forget(const char *path) {
    char key[11];
    make_legacy_key(path, key);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LEGACY_NS, NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    size_t size = LEGACY_BLOB_MAX;
    uint8_t *blob = malloc(size);
    if (!blob) { nvs_close(handle); return ESP_ERR_NO_MEM; }
    err = nvs_get_blob(handle, key, blob, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    else if (err == ESP_OK) {
        if (size < LEGACY_HEADER_SIZE + 2 || size > LEGACY_BLOB_MAX || memcmp(blob, "RBN", 3) || blob[3] != 1)
            err = ESP_ERR_INVALID_STATE;
        else {
            size_t path_len = get16(blob + 4);
            if (path_len < 2 || path_len > BOOK_STORE_PATH_MAX || path_len > size - LEGACY_HEADER_SIZE ||
                strnlen((const char *)blob + LEGACY_HEADER_SIZE, path_len) != path_len - 1 ||
                strcmp((const char *)blob + LEGACY_HEADER_SIZE, path)) err = ESP_ERR_INVALID_STATE;
            else err = nvs_erase_key(handle, key);
        }
    }
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    free(blob);
    return err;
}

void book_notes_free(book_notes_t *notes) {
    if (!notes) return;
    for (size_t i = 0; i < notes->excerpt_count; ++i) free(notes->excerpts[i].text);
    free(notes->bookmarks);
    free(notes->excerpts);
    memset(notes, 0, sizeof(*notes));
}

esp_err_t book_notes_load(const char *path, uint32_t file_size, book_notes_t *out) {
    if (!valid_path(path) || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    note_paths_t *paths = calloc(1, sizeof(*paths));
    if (!paths) return ESP_ERR_NO_MEM;
    if (!make_paths(path, paths)) { free(paths); return ESP_ERR_INVALID_ARG; }
    esp_err_t err = recover_backup(paths, path);
    if (err != ESP_OK) { free(paths); return err; }
    FILE *file = fopen(paths->data, "rb");
    if (!file) {
        free(paths);
        if (errno != ENOENT) return ESP_FAIL;
        book_notes_t legacy = {0};
        err = legacy_load(path, file_size, &legacy);
        if (err == ESP_ERR_NOT_FOUND) return err;
        if (err != ESP_OK) return err;
        err = book_notes_save(path, file_size, &legacy);
        if (err == ESP_OK) (void)legacy_forget(path);
        if (err != ESP_OK) book_notes_free(&legacy);
        else *out = legacy;
        return err;
    }
    free(paths);

    uint8_t header[NOTES_HEADER_SIZE];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "RBNF", 4) ||
        (header[4] != NOTES_FORMAT_VERSION && header[4] != NOTES_LEGACY_VERSION)) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint8_t format_version = header[4];
    size_t path_len = get16(header + 5);
    if (path_len < 2 || path_len > BOOK_STORE_PATH_MAX || get32(header + 8) != file_size) {
        fclose(file);
        return get32(header + 8) != file_size ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_RESPONSE;
    }
    char stored_path[BOOK_STORE_PATH_MAX];
    if (fread(stored_path, 1, path_len, file) != path_len || stored_path[path_len - 1] != 0 ||
        strnlen(stored_path, path_len) != path_len - 1) {
        fclose(file);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (strcmp(stored_path, path)) {
        fclose(file);
        return ESP_ERR_INVALID_STATE;
    }

    book_notes_t result = {0};
    esp_err_t read_err = ESP_OK;
    for (;;) {
        int kind = fgetc(file);
        if (kind == EOF) break;
        uint8_t meta[NOTES_BOOKMARK_SIZE - 1];
        size_t meta_len = kind == NOTES_RECORD_BOOKMARK || kind == NOTES_RECORD_EXCERPT
            ? NOTES_BOOKMARK_SIZE - 1 : 0;
        if (!meta_len || fread(meta, 1, meta_len, file) != meta_len || meta[6] > 100) {
            read_err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        book_note_position_t position = {get16(meta), get32(meta + 2), meta[6]};
        if (kind == NOTES_RECORD_BOOKMARK) {
            if (result.bookmark_count >= SIZE_MAX / sizeof(*result.bookmarks)) { read_err = ESP_ERR_INVALID_SIZE; break; }
            book_note_position_t *items = realloc(result.bookmarks, (result.bookmark_count + 1) * sizeof(*items));
            if (!items) { read_err = ESP_ERR_NO_MEM; break; }
            result.bookmarks = items;
            result.bookmarks[result.bookmark_count++] = position;
        } else {
            uint8_t len_data[4];
            size_t length_bytes = format_version == NOTES_LEGACY_VERSION ? 2 : 4;
            if (fread(len_data, 1, length_bytes, file) != length_bytes) { read_err = ESP_ERR_INVALID_RESPONSE; break; }
            uint32_t stored_len = format_version == NOTES_LEGACY_VERSION ? get16(len_data) : get32(len_data);
            size_t len = stored_len;
            if (!len || len + 1 < len || result.excerpt_count >= SIZE_MAX / sizeof(*result.excerpts)) {
                read_err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            book_note_excerpt_t *items = realloc(result.excerpts, (result.excerpt_count + 1) * sizeof(*items));
            if (!items) { read_err = ESP_ERR_NO_MEM; break; }
            result.excerpts = items;
            book_note_excerpt_t *excerpt = &result.excerpts[result.excerpt_count];
            excerpt->position = position;
            excerpt->text = malloc(len + 1);
            if (!excerpt->text) { read_err = ESP_ERR_NO_MEM; break; }
            excerpt->text_len = len;
            ++result.excerpt_count;
            if (fread(excerpt->text, 1, len, file) != len || memchr(excerpt->text, 0, len)) {
                read_err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            excerpt->text[len] = 0;
        }
    }
    if (ferror(file)) read_err = ESP_FAIL;
    fclose(file);
    if (read_err != ESP_OK) {
        book_notes_free(&result);
        return read_err;
    }
    *out = result;
    return ESP_OK;
}

static esp_err_t write_position(FILE *file, uint8_t kind, const book_note_position_t *position) {
    uint8_t record[NOTES_BOOKMARK_SIZE] = {kind};
    put16(record + 1, position->chapter);
    put32(record + 3, position->byte_off);
    record[7] = position->percent;
    return fwrite(record, 1, sizeof(record), file) == sizeof(record) ? ESP_OK : ESP_FAIL;
}

static esp_err_t save_with_paths(const char *path, uint32_t file_size,
                                 const book_notes_t *notes, note_paths_t *paths) {
    esp_err_t err = ensure_directory(paths->directory);
    if (err != ESP_OK) return err;
    err = recover_backup(paths, path);
    if (err != ESP_OK) return err;
    uint8_t header[NOTES_HEADER_SIZE] = {'R', 'B', 'N', 'F', NOTES_FORMAT_VERSION, 0, 0, 0, 0, 0, 0, 0};
    size_t path_len = strlen(path) + 1;
    put16(header + 5, (uint16_t)path_len);
    put32(header + 8, file_size);
    FILE *file = fopen(paths->temporary, "wb");
    if (!file) return ESP_FAIL;
    bool ok = fwrite(header, 1, sizeof(header), file) == sizeof(header) &&
              fwrite(path, 1, path_len, file) == path_len;
    for (size_t i = 0; ok && i < notes->bookmark_count; ++i) {
        const book_note_position_t *p = &notes->bookmarks[i];
        if (p->percent > 100) { ok = false; break; }
        ok = write_position(file, NOTES_RECORD_BOOKMARK, p) == ESP_OK;
    }
    for (size_t i = 0; ok && i < notes->excerpt_count; ++i) {
        const book_note_excerpt_t *excerpt = &notes->excerpts[i];
        size_t len = excerpt->text_len;
        uint64_t len64 = len;
        if (!excerpt->text || !len || len64 > UINT32_MAX || excerpt->position.percent > 100 ||
            memchr(excerpt->text, 0, len)) { ok = false; break; }
        uint8_t len_data[4];
        put32(len_data, (uint32_t)len64);
        ok = write_position(file, NOTES_RECORD_EXCERPT, &excerpt->position) == ESP_OK &&
             fwrite(len_data, 1, sizeof(len_data), file) == sizeof(len_data) &&
             fwrite(excerpt->text, 1, len, file) == len;
    }
    if (ok && fflush(file) != 0) ok = false;
    if (ok && fsync(fileno(file)) != 0) ok = false;
    if (fclose(file) != 0) ok = false;
    if (!ok) { unlink(paths->temporary); return ESP_FAIL; }

    struct stat st;
    if (stat(paths->data, &st) == 0) {
        if (unlink(paths->backup) != 0 && errno != ENOENT) { unlink(paths->temporary); return ESP_FAIL; }
        if (rename(paths->data, paths->backup) != 0) { unlink(paths->temporary); return ESP_FAIL; }
    } else if (errno != ENOENT) {
        unlink(paths->temporary);
        return ESP_FAIL;
    }
    if (rename(paths->temporary, paths->data) != 0) {
        (void)rename(paths->backup, paths->data);
        unlink(paths->temporary);
        return ESP_FAIL;
    }
    (void)unlink(paths->backup);
    (void)legacy_forget(path);
    return ESP_OK;
}

esp_err_t book_notes_save(const char *path, uint32_t file_size, const book_notes_t *notes) {
    if (!valid_path(path) || !notes || (notes->bookmark_count && !notes->bookmarks) ||
        (notes->excerpt_count && !notes->excerpts)) return ESP_ERR_INVALID_ARG;
    note_paths_t *paths = calloc(1, sizeof(*paths));
    if (!paths) return ESP_ERR_NO_MEM;
    if (!make_paths(path, paths)) { free(paths); return ESP_ERR_INVALID_ARG; }
    esp_err_t err = save_with_paths(path, file_size, notes, paths);
    free(paths);
    return err;
}

static esp_err_t forget_with_paths(const char *path, note_paths_t *paths) {
    esp_err_t result = ESP_OK;
    const char *items[] = {paths->data, paths->backup};
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        bool exists = false;
        esp_err_t owner_err = sidecar_owner(items[i], path, &exists);
        if (owner_err != ESP_OK) { result = owner_err; continue; }
        if (exists && unlink(items[i]) != 0) result = ESP_FAIL;
    }
    if (unlink(paths->temporary) != 0 && errno != ENOENT) result = ESP_FAIL;
    esp_err_t legacy_err = legacy_forget(path);
    return result != ESP_OK ? result : legacy_err;
}

esp_err_t book_notes_forget(const char *path) {
    if (!valid_path(path)) return ESP_ERR_INVALID_ARG;
    note_paths_t *paths = calloc(1, sizeof(*paths));
    if (!paths) return ESP_ERR_NO_MEM;
    if (!make_paths(path, paths)) { free(paths); return ESP_ERR_INVALID_ARG; }
    esp_err_t err = forget_with_paths(path, paths);
    free(paths);
    return err;
}
