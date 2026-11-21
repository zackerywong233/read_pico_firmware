#pragma once
#include <stdint.h>
#include "epd_highlevel.h"
#include "esp_err.h"
static inline esp_err_t ambient_sync_run(EpdiyHighlevelState *hl, uint8_t *fb) { (void)hl; (void)fb; return ESP_OK; }
