/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：主机验证 Markdown 导出内容与安全文件名。/ English: Verify Markdown export and safe filenames on host.
 * 冻结：仅使用临时目录，不覆盖产品数据。/ Frozen: Use a temporary directory only; never touch product data.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "book_notes.h"
#include "nvs.h"

esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *handle) { (void)ns; (void)mode; (void)handle; return ESP_ERR_NVS_NOT_FOUND; }
void nvs_close(nvs_handle_t handle) { (void)handle; }
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) { (void)handle; (void)key; (void)out; (void)size; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size) { (void)handle; (void)key; (void)data; (void)size; return ESP_OK; }
esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *size) { (void)handle; (void)key; (void)out; (void)size; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value) { (void)handle; (void)key; (void)value; return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) { (void)handle; (void)key; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle) { (void)handle; return ESP_OK; }
esp_err_t nvs_get_u32(nvs_handle_t handle, const char *key, uint32_t *out) { (void)handle; (void)key; (void)out; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_u32(nvs_handle_t handle, const char *key, uint32_t value) { (void)handle; (void)key; (void)value; return ESP_OK; }

int main(void) {
    (void)mkdir("/tmp/pico-book-notes-export-test", 0777);
    book_note_position_t bookmark = {.chapter = 1, .byte_off = 42, .percent = 18};
    char excerpt_text[] = "第一行\n第二行";
    book_note_excerpt_t excerpt = {.position = {.chapter = 2, .byte_off = 90, .percent = 51},
                                   .text = excerpt_text, .text_len = strlen(excerpt_text)};
    book_notes_t notes = {.bookmark_count = 1, .excerpt_count = 1,
                          .bookmarks = &bookmark, .excerpts = &excerpt};
    char path[288];
    assert(book_notes_export_markdown("/sdcard/books/test.txt", "A/B《书》", &notes,
                                      path, sizeof(path)) == ESP_OK);
    assert(strstr(path, "/A_B《书》.md") != NULL);
    FILE *file = fopen(path, "rb");
    assert(file);
    char content[512] = {0};
    size_t len = fread(content, 1, sizeof(content) - 1, file);
    fclose(file);
    assert(len > 0);
    assert(strstr(content, "# A/B《书》") != NULL);
    assert(strstr(content, "## 书签 · 第 2 章 · 18%") != NULL);
    assert(strstr(content, "> 位置：42 字节") != NULL);
    assert(strstr(content, "## 摘录 · 第 3 章 · 51%") != NULL);
    assert(strstr(content, "> 第一行\n> 第二行") != NULL);
    remove(path);
    puts("book_notes_export_host_test: PASS");
    return 0;
}
