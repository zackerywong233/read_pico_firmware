#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
static inline esp_err_t read_pico_buzzer_tone(uint32_t hz, uint32_t ms) { (void)hz; (void)ms; return ESP_OK; }
