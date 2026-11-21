#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define CST836U_MAX_POINTS 2
typedef struct cst836u_dev_t *cst836u_handle_t;
typedef struct { uint8_t fw; } cst836u_info_t;
typedef struct { bool active; uint8_t event; uint8_t id; uint16_t x; uint16_t y; } cst836u_point_t;
typedef struct {
    bool touched;
    uint8_t count;
    uint16_t x;
    uint16_t y;
    cst836u_point_t points[CST836U_MAX_POINTS];
    uint8_t raw[16];
} cst836u_touch_t;
esp_err_t cst836u_read(cst836u_handle_t h, cst836u_touch_t *t);
esp_err_t cst836u_get_info(cst836u_handle_t h, cst836u_info_t *i);
