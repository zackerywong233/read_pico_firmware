#pragma once
#include <stddef.h>
#include "esp_err.h"
esp_err_t read_pico_transfer_get_saved_wifi_list(char out[5][33], size_t *count);
