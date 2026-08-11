#include <string.h>
#include <stdlib.h>
#include "json_parse.h"
#include "esp_err.h"
#include "cJSON.h"

static weather_forecast_t s_weather_forecast;
static uint8_t has_weather_hourly_been_processed = 0;
static uint8_t has_weather_daily_been_processed = 0;

/*
 * Get a 64-bit integer from a JSON array.
 */
static int64_t json_array_get_i64(cJSON *arr, size_t idx)
{
    cJSON *item = cJSON_GetArrayItem(arr, (int)idx);
    if (item && cJSON_IsNumber(item)) {
        return (int64_t)item->valuedouble;
    }
    return 0;
}

/*
 * Get a 32-bit integer from a JSON array.
 */
static int json_array_get_i32(cJSON *arr, size_t idx)
{
    cJSON *item = cJSON_GetArrayItem(arr, (int)idx);
    if (item && cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return 0;
}

/*
 * Get a 32-bit float from a JSON array.
 */
static float json_array_get_f32(cJSON *arr, size_t idx)
{
    cJSON *item = cJSON_GetArrayItem(arr, (int)idx);
    if (item && cJSON_IsNumber(item)) {
        return (float)item->valuedouble;
    }
    return 0.0f;
}

/*
 * Get smaller size
 */
static size_t min_size(size_t a, size_t b)
{
    return (a < b) ? a : b;
}

/*
 * Process the weather hourly response.
 */
esp_err_t process_weather_hourly_response(const char *response, size_t response_len)
{
    cJSON *root = cJSON_ParseWithLength(response, response_len);
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *timezone = cJSON_GetObjectItemCaseSensitive(root, "timezone");
    if (cJSON_IsString(timezone) && timezone->valuestring) {
        strncpy(s_weather_forecast.timezone, timezone->valuestring, sizeof(s_weather_forecast.timezone) - 1);
        s_weather_forecast.timezone[sizeof(s_weather_forecast.timezone) - 1] = '\0';
    }

    cJSON *latitude = cJSON_GetObjectItemCaseSensitive(root, "latitude");
    cJSON *longitude = cJSON_GetObjectItemCaseSensitive(root, "longitude");
    if (cJSON_IsNumber(latitude)) {
        s_weather_forecast.latitude = latitude->valuedouble;
    }
    if (cJSON_IsNumber(longitude)) {
        s_weather_forecast.longitude = longitude->valuedouble;
    }

    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    if (cJSON_IsObject(current)) {
        cJSON *time = cJSON_GetObjectItemCaseSensitive(current, "time");
        cJSON *temperature_2m = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
        cJSON *relative_humidity_2m = cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
        cJSON *is_day = cJSON_GetObjectItemCaseSensitive(current, "is_day");
        cJSON *showers = cJSON_GetObjectItemCaseSensitive(current, "showers");
        cJSON *snowfall = cJSON_GetObjectItemCaseSensitive(current, "snowfall");
        cJSON *rain = cJSON_GetObjectItemCaseSensitive(current, "rain");
        cJSON *precipitation = cJSON_GetObjectItemCaseSensitive(current, "precipitation");
        cJSON *weather_code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
        cJSON *wind_speed_10m = cJSON_GetObjectItemCaseSensitive(current, "wind_speed_10m");
        cJSON *cloud_cover = cJSON_GetObjectItemCaseSensitive(current, "cloud_cover");

        if (cJSON_IsNumber(time)) s_weather_forecast.current.time = (int64_t)time->valuedouble;
        if (cJSON_IsNumber(temperature_2m)) s_weather_forecast.current.temperature_2m = (float)temperature_2m->valuedouble;
        if (cJSON_IsNumber(relative_humidity_2m)) s_weather_forecast.current.relative_humidity_2m = relative_humidity_2m->valueint;
        if (cJSON_IsNumber(is_day)) s_weather_forecast.current.is_day = is_day->valueint;
        if (cJSON_IsNumber(showers)) s_weather_forecast.current.showers = (float)showers->valuedouble;
        if (cJSON_IsNumber(snowfall)) s_weather_forecast.current.snowfall = (float)snowfall->valuedouble;
        if (cJSON_IsNumber(rain)) s_weather_forecast.current.rain = (float)rain->valuedouble;
        if (cJSON_IsNumber(precipitation)) s_weather_forecast.current.precipitation = (float)precipitation->valuedouble;
        if (cJSON_IsNumber(weather_code)) s_weather_forecast.current.weather_code = weather_code->valueint;
        if (cJSON_IsNumber(wind_speed_10m)) s_weather_forecast.current.wind_speed_10m = (float)wind_speed_10m->valuedouble;
        if (cJSON_IsNumber(cloud_cover)) s_weather_forecast.current.cloud_cover = cloud_cover->valueint;
    }

    cJSON *hourly = cJSON_GetObjectItemCaseSensitive(root, "hourly");
    if (!cJSON_IsObject(hourly)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *arr_time = cJSON_GetObjectItemCaseSensitive(hourly, "time");
    cJSON *arr_temp = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
    cJSON *arr_pop = cJSON_GetObjectItemCaseSensitive(hourly, "precipitation_probability");
    cJSON *arr_prec = cJSON_GetObjectItemCaseSensitive(hourly, "precipitation");
    cJSON *arr_hum = cJSON_GetObjectItemCaseSensitive(hourly, "relative_humidity_2m");
    cJSON *arr_rain = cJSON_GetObjectItemCaseSensitive(hourly, "rain");
    cJSON *arr_showers = cJSON_GetObjectItemCaseSensitive(hourly, "showers");
    cJSON *arr_snow = cJSON_GetObjectItemCaseSensitive(hourly, "snowfall");
    cJSON *arr_cloud = cJSON_GetObjectItemCaseSensitive(hourly, "cloud_cover");
    cJSON *arr_code = cJSON_GetObjectItemCaseSensitive(hourly, "weather_code");
    cJSON *arr_is_day = cJSON_GetObjectItemCaseSensitive(hourly, "is_day");

    if (!cJSON_IsArray(arr_time) || !cJSON_IsArray(arr_temp) || !cJSON_IsArray(arr_code)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t count = cJSON_GetArraySize(arr_time);
    count = min_size(count, (size_t)cJSON_GetArraySize(arr_temp));
    count = min_size(count, (size_t)cJSON_GetArraySize(arr_code));
    if (arr_pop) count = min_size(count, (size_t)cJSON_GetArraySize(arr_pop));
    if (arr_prec) count = min_size(count, (size_t)cJSON_GetArraySize(arr_prec));
    if (arr_hum) count = min_size(count, (size_t)cJSON_GetArraySize(arr_hum));
    if (arr_rain) count = min_size(count, (size_t)cJSON_GetArraySize(arr_rain));
    if (arr_showers) count = min_size(count, (size_t)cJSON_GetArraySize(arr_showers));
    if (arr_snow) count = min_size(count, (size_t)cJSON_GetArraySize(arr_snow));
    if (arr_cloud) count = min_size(count, (size_t)cJSON_GetArraySize(arr_cloud));
    if (arr_is_day) count = min_size(count, (size_t)cJSON_GetArraySize(arr_is_day));
    if (count > WEATHER_HOURLY_POINTS_MAX) {
        count = WEATHER_HOURLY_POINTS_MAX;
    }

    for (size_t i = 0; i < count; i++) {
        weather_hourly_point_t *p = &s_weather_forecast.hourly[i];
        p->time = json_array_get_i64(arr_time, i);
        p->temperature_2m = json_array_get_f32(arr_temp, i);
        p->weather_code = json_array_get_i32(arr_code, i);
        p->precipitation_probability = arr_pop ? json_array_get_i32(arr_pop, i) : 0;
        p->precipitation = arr_prec ? json_array_get_f32(arr_prec, i) : 0.0f;
        p->relative_humidity_2m = arr_hum ? json_array_get_i32(arr_hum, i) : 0;
        p->rain = arr_rain ? json_array_get_f32(arr_rain, i) : 0.0f;
        p->showers = arr_showers ? json_array_get_f32(arr_showers, i) : 0.0f;
        p->snowfall = arr_snow ? json_array_get_f32(arr_snow, i) : 0.0f;
        p->cloud_cover = arr_cloud ? json_array_get_i32(arr_cloud, i) : 0;
        p->is_day = arr_is_day ? json_array_get_i32(arr_is_day, i) : 0;
    }
    s_weather_forecast.hourly_count = count;

    has_weather_hourly_been_processed = 1;
    cJSON_Delete(root);
    return ESP_OK;
}

/*
 * Process the weather daily response.
 */
esp_err_t process_weather_daily_response(const char *response, size_t response_len)
{
    cJSON *root = cJSON_ParseWithLength(response, response_len);
    if (root == NULL) {
        return ESP_FAIL;
    }

    cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    if (!cJSON_IsObject(daily)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *arr_time = cJSON_GetObjectItemCaseSensitive(daily, "time");
    cJSON *arr_code = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
    cJSON *arr_tmax = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    cJSON *arr_tmin = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
    cJSON *arr_pop_max = cJSON_GetObjectItemCaseSensitive(daily, "precipitation_probability_max");
    cJSON *arr_sunrise = cJSON_GetObjectItemCaseSensitive(daily, "sunrise");
    cJSON *arr_sunset = cJSON_GetObjectItemCaseSensitive(daily, "sunset");
    cJSON *arr_daylight = cJSON_GetObjectItemCaseSensitive(daily, "daylight_duration");
    cJSON *arr_rain = cJSON_GetObjectItemCaseSensitive(daily, "rain_sum");
    cJSON *arr_snow = cJSON_GetObjectItemCaseSensitive(daily, "snowfall_sum");
    cJSON *arr_showers = cJSON_GetObjectItemCaseSensitive(daily, "showers_sum");
    cJSON *arr_wind = cJSON_GetObjectItemCaseSensitive(daily, "wind_speed_10m_max");

    if (!cJSON_IsArray(arr_time) || !cJSON_IsArray(arr_code) || !cJSON_IsArray(arr_tmax) || !cJSON_IsArray(arr_tmin)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    size_t count = cJSON_GetArraySize(arr_time);
    count = min_size(count, (size_t)cJSON_GetArraySize(arr_code));
    count = min_size(count, (size_t)cJSON_GetArraySize(arr_tmax));
    count = min_size(count, (size_t)cJSON_GetArraySize(arr_tmin));
    if (arr_pop_max) count = min_size(count, (size_t)cJSON_GetArraySize(arr_pop_max));
    if (arr_sunrise) count = min_size(count, (size_t)cJSON_GetArraySize(arr_sunrise));
    if (arr_sunset) count = min_size(count, (size_t)cJSON_GetArraySize(arr_sunset));
    if (arr_daylight) count = min_size(count, (size_t)cJSON_GetArraySize(arr_daylight));
    if (arr_rain) count = min_size(count, (size_t)cJSON_GetArraySize(arr_rain));
    if (arr_snow) count = min_size(count, (size_t)cJSON_GetArraySize(arr_snow));
    if (arr_showers) count = min_size(count, (size_t)cJSON_GetArraySize(arr_showers));
    if (arr_wind) count = min_size(count, (size_t)cJSON_GetArraySize(arr_wind));
    if (count > WEATHER_DAILY_POINTS_MAX) {
        count = WEATHER_DAILY_POINTS_MAX;
    }

    for (size_t i = 0; i < count; i++) {
        weather_daily_point_t *d = &s_weather_forecast.daily[i];
        d->time = json_array_get_i64(arr_time, i);
        d->weather_code = json_array_get_i32(arr_code, i);
        d->temperature_2m_max = json_array_get_f32(arr_tmax, i);
        d->temperature_2m_min = json_array_get_f32(arr_tmin, i);
        d->precipitation_probability_max = arr_pop_max ? json_array_get_i32(arr_pop_max, i) : 0;
        d->sunrise = arr_sunrise ? json_array_get_i64(arr_sunrise, i) : 0;
        d->sunset = arr_sunset ? json_array_get_i64(arr_sunset, i) : 0;
        d->daylight_duration = arr_daylight ? json_array_get_i32(arr_daylight, i) : 0;
        d->rain_sum = arr_rain ? json_array_get_f32(arr_rain, i) : 0.0f;
        d->snowfall_sum = arr_snow ? json_array_get_f32(arr_snow, i) : 0.0f;
        d->showers_sum = arr_showers ? json_array_get_f32(arr_showers, i) : 0.0f;
        d->wind_speed_10m_max = arr_wind ? json_array_get_f32(arr_wind, i) : 0.0f;
    }
    s_weather_forecast.daily_count = count;

    has_weather_daily_been_processed = 1;
    cJSON_Delete(root);
    return ESP_OK;
}

/*
 * Get the cached weather forecast.
 */
esp_err_t get_weather_forecast(weather_forecast_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!has_weather_hourly_been_processed && !has_weather_daily_been_processed) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(out, &s_weather_forecast, sizeof(weather_forecast_t));
    return ESP_OK;
}