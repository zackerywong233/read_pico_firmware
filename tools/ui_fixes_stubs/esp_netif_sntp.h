#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct { const char *server; } esp_sntp_config_t;
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(s) ((esp_sntp_config_t){ .server = (s) })
static inline esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *c) { (void)c; return ESP_OK; }
static inline void esp_netif_sntp_deinit(void) {}
