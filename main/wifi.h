#pragma once

#include "esp_wifi_types_generic.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DISPLAY_WIFI_SSID CONFIG_DISPLAY_WIFI_STA_SSID
#define DISPLAY_WIFI_PASS CONFIG_DISPLAY_WIFI_STA_PASSWORD
#define DISPLAY_WIFI_MAX_RETRY CONFIG_DISPLAY_WIFI_MAXIMUM_RETRY

#if CONFIG_DISPLAY_WIFI_AUTH_OPEN
#define DISPLAY_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_DISPLAY_WIFI_AUTH_WEP
#define DISPLAY_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_DISPLAY_WIFI_AUTH_WPA_PSK
#define DISPLAY_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_DISPLAY_WIFI_AUTH_WPA2_PSK
#define DISPLAY_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_DISPLAY_WIFI_AUTH_WPA_WPA2_PSK
#define DISPLAY_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_DISPLAY_WIFI_AUTH_WPA2_WPA3_PSK
#define DISPLAY_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_DISPLAY_WIFI_AUTH_WPA3_PSK
#define DISPLAY_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#else
#define DISPLAY_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#endif

#include "esp_err.h"

esp_err_t wifi_station_init(void);

esp_err_t wifi_station_connect(char* ssid, char* password);

esp_err_t wifi_station_disconnect(void);

esp_err_t wifi_station_deinit(void);

esp_err_t wifi_station_get_ipv6(char* ipv6_addr, size_t addr_len);
esp_err_t wifi_station_wait_for_ipv6(uint32_t timeout_ms);
bool wifi_station_is_connected(void);