#pragma once
#include <stdbool.h>
bool ttf_font_ready(void);
bool ttf_font_is_builtin(void);
bool ttf_font_path_is_builtin(const char *path);
const char *ttf_font_path(void);
int ttf_font_open(const char *path);
int ttf_font_open_builtin(void);
int ttf_text_width_px(int px, const char *text);
