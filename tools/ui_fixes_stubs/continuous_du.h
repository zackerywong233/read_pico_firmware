#pragma once
#include <stdbool.h>
#include "epd_highlevel.h"
#include "epdiy.h"
int continuous_du_dark_phases(void);
int continuous_du_light_phases(void);
bool continuous_du_init(void);
void continuous_du_reset(void);
bool continuous_du_busy(void);
void continuous_du_from_logical(int lx, int ly, int *px, int *py);
EpdRect continuous_du_rect_from_logical(EpdRect logical);
void continuous_du_mark_circle(int cx, int cy, int radius, int phases);
enum EpdDrawError continuous_du_scan(EpdiyHighlevelState *hl, EpdRect area);
