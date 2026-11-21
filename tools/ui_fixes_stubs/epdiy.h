#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { int x, y, width, height; } EpdRect;
enum EpdFontFlags { EPD_DRAW_ALIGN_LEFT = 0, EPD_DRAW_ALIGN_RIGHT = 1, EPD_DRAW_ALIGN_CENTER = 2 };
enum EpdDrawError { EPD_DRAW_SUCCESS = 0 };
enum EpdDrawMode { MODE_DU = 1, MODE_GL16 = 2, MODE_GC16 = 3 };
typedef struct { int dummy; } EpdWaveform;
int epd_width(void);
int epd_height(void);
int epd_rotated_display_width(void);
int epd_rotated_display_height(void);
void epd_draw_pixel(int x, int y, uint8_t color, uint8_t *fb);
void epd_fill_rect(EpdRect r, uint8_t color, uint8_t *fb);
void epd_fill_triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t c, uint8_t *fb);
void epd_poweron(void);
