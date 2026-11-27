#pragma once
#include <stddef.h>
#include "esp_err.h"
static inline esp_err_t mdns_init(void) { return ESP_OK; }
static inline void mdns_free(void) {}
static inline esp_err_t mdns_hostname_set(const char *name) { (void)name; return ESP_OK; }
static inline esp_err_t mdns_instance_name_set(const char *name) { (void)name; return ESP_OK; }
static inline esp_err_t mdns_service_add(const char *name, const char *service, const char *proto, uint16_t port, const void *txt, size_t n) { (void)name;(void)service;(void)proto;(void)port;(void)txt;(void)n;return ESP_OK; }
