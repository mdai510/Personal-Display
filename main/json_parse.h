#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#define WEATHER_HOURLY_POINTS_MAX 24
#define WEATHER_DAILY_POINTS_MAX 7

typedef struct {
  int64_t time;
  float temperature_2m;
  int precipitation_probability;
  float precipitation;
  int relative_humidity_2m;
  float rain;
  float showers;
  float snowfall;
  int cloud_cover;
  int weather_code;
  int is_day;
} weather_hourly_point_t;

typedef struct {
  int64_t time;
  int weather_code;
  float temperature_2m_max;
  float temperature_2m_min;
  int precipitation_probability_max;
  int64_t sunrise;
  int64_t sunset;
  int daylight_duration;
  float rain_sum;
  float snowfall_sum;
  float showers_sum;
  float wind_speed_10m_max;
} weather_daily_point_t;

typedef struct {
  int64_t time;
  float temperature_2m;
  int relative_humidity_2m;
  int is_day;
  float showers;
  float snowfall;
  float rain;
  float precipitation;
  int weather_code;
  float wind_speed_10m;
  int cloud_cover;
} weather_current_t;

typedef struct {
  char timezone[64];
  double latitude;
  double longitude;
  weather_current_t current;
  weather_hourly_point_t hourly[WEATHER_HOURLY_POINTS_MAX];
  size_t hourly_count;
  weather_daily_point_t daily[WEATHER_DAILY_POINTS_MAX];
  size_t daily_count;
} weather_forecast_t;

esp_err_t process_weather_hourly_response(const char* response, size_t response_len);
esp_err_t process_weather_daily_response(const char* response, size_t response_len);
esp_err_t get_weather_forecast(weather_forecast_t *out);

