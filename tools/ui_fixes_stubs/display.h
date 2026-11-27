#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "epd_highlevel.h"
#include "epdiy.h"
void guard_draw_result(EpdiyHighlevelState *hl, enum EpdDrawError err);
enum EpdDrawError update_display_full(EpdiyHighlevelState *hl);
enum EpdDrawError update_display_mode(EpdiyHighlevelState *hl, enum EpdDrawMode mode);
enum EpdDrawError update_display_white(EpdiyHighlevelState *hl);
enum EpdDrawError update_display_area_with(EpdiyHighlevelState *hl, const EpdWaveform *wf, enum EpdDrawMode mode, EpdRect area);
bool display_take_white_exit(void);
void rails_idle_check(int64_t now_ms);
void rails_keepalive(void);
