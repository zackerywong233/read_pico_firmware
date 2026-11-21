#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef struct { bool present; bool mounted; } read_pico_sd_info_t;
esp_err_t read_pico_sd_get_info(read_pico_sd_info_t *info);
esp_err_t read_pico_sd_start_probe(void);
