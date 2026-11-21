# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
# 独立 BLE 配套控制通道组件（NimBLE 外设）。/ Standalone BLE companion control channel component (NimBLE peripheral).
idf_component_register(SRCS "read_pico_ble.c"
    INCLUDE_DIRS "include"
    REQUIRES bt heap nvs_flash esp_timer esp_hw_support esp_app_format espressif__cjson read_pico_transfer)
