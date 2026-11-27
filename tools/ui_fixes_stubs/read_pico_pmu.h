#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define PMU_STATUS_OK 0
#define PMU_STATUS_ACCEPTED 1
#define PMU_CHARGE_NOT_CHARGING 0
#define PMU_CHARGE_CHARGING 1
#define PMU_CMD_TIME_GET 0x30

typedef struct {
    bool present;
    bool identity_ok;
    bool status_ok;
    bool time_ok;
    uint8_t fw_major, fw_minor, fw_patch;
    uint8_t charge_state;
    uint16_t battery_mv;
    uint16_t soc_permille;
    uint32_t unix_sec;
    uint8_t time_synced;
    uint16_t last_op_status;
} pmu_snapshot_t;

bool read_pico_pmu_ready(void);
const pmu_snapshot_t *read_pico_pmu_get(void);
esp_err_t read_pico_pmu_refresh(void);
esp_err_t read_pico_pmu_cmd(uint16_t code, const uint8_t *payload, uint8_t plen);
esp_err_t read_pico_pmu_time_sync(uint32_t unix_seconds);
bool read_pico_pmu_take_key_short(void);
