#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "epd_highlevel.h"
#include "sc7a20h_lab.h"
bool app_lock_powers_down(void);
void enter_lock_and_sleep(EpdiyHighlevelState *hl, int64_t *ignore_until, sc7a20h_handle_t acc);
