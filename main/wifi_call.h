#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t wifi_call_start(void);
esp_err_t wifi_call_fetch_weather_once(void);
esp_err_t wifi_call_request_refresh(void);
esp_err_t wifi_call_set_manual_location(double latitude,
										double longitude,
										const char *timezone,
										int32_t utc_offset_seconds);
bool wifi_call_is_weather_ready(void);
esp_err_t wifi_call_wait_weather_ready(uint32_t timeout_ms);
