#pragma once

#include "esp_err.h"

esp_err_t call_ip_api_with_ipv6(const char *ipv6);
esp_err_t call_weather_api_24h(double latitude, double longitude, const char *timezone);
esp_err_t call_weather_api_7d(double latitude, double longitude, const char *timezone);
