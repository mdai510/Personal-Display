#include <string.h>
#include <stdlib.h>
#include "json_parse.h"
#include "esp_err.h"
#include "cJSON.h"

static uint8_t has_ip_api_been_processed = 0;
static ip_api_info_t s_ip_api_info;
static weather_forecast_t s_weather_forecast;
static uint8_t has_weather_hourly_been_processed = 0;
static uint8_t has_weather_daily_been_processed = 0;

static void replace_heap_string(char **target, const char *value)
{
    if (*target) {
        free(*target);
        *target = NULL;
    }
    if (value) {
        *target = strdup(value);
    }
}

static int64_t json_array_get_i64(cJSON *arr, size_t idx)
{
    cJSON *item = cJSON_GetArrayItem(arr, (int)idx);
    if (item && cJSON_IsNumber(item)) {
        return (int64_t)item->valuedouble;
    }
    return 0;
}

static int json_array_get_i32(cJSON *arr, size_t idx)
{
    cJSON *item = cJSON_GetArrayItem(arr, (int)idx);
    if (item && cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return 0;
}

static float json_array_get_f32(cJSON *arr, size_t idx)
{
    cJSON *item = cJSON_GetArrayItem(arr, (int)idx);
    if (item && cJSON_IsNumber(item)) {
        return (float)item->valuedouble;
    }
    return 0.0f;
}

static size_t min_size(size_t a, size_t b)
{
    return (a < b) ? a : b;
}

esp_err_t process_ip_api_response(const char *response, size_t response_len){
    cJSON *root = cJSON_ParseWithLength(response, response_len);
    if (root == NULL) {
        return ESP_FAIL;
    }
    
    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (status == NULL || cJSON_IsString(status) == 0 || strcmp(status->valuestring, "success") != 0) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    cJSON *country = cJSON_GetObjectItemCaseSensitive(root, "country");
    if(cJSON_IsString(country) && (country->valuestring != NULL)) {
        replace_heap_string(&s_ip_api_info.country, country->valuestring);
    }
    cJSON *regionName = cJSON_GetObjectItem(root, "regionName");
    if(cJSON_IsString(regionName) && (regionName->valuestring != NULL)) {
        replace_heap_string(&s_ip_api_info.regionName, regionName->valuestring);
    }
    cJSON *city = cJSON_GetObjectItem(root, "city");
    if(cJSON_IsString(city) && (city->valuestring != NULL)) {
        replace_heap_string(&s_ip_api_info.city, city->valuestring);
    }
    cJSON *lat = cJSON_GetObjectItem(root, "lat");
    if(cJSON_IsNumber(lat)) {
        s_ip_api_info.lat = lat->valuedouble;
    }
    cJSON *lon = cJSON_GetObjectItem(root, "lon");
    if(cJSON_IsNumber(lon)) {
        s_ip_api_info.lon = lon->valuedouble;
    }
    cJSON *timezone = cJSON_GetObjectItem(root, "timezone");
    if(cJSON_IsString(timezone) && (timezone->valuestring != NULL)) {
        replace_heap_string(&s_ip_api_info.timezone, timezone->valuestring);
    }

    cJSON *offset = cJSON_GetObjectItem(root, "offset");
    if (cJSON_IsNumber(offset)) {
        s_ip_api_info.utc_offset_seconds = (int32_t)offset->valueint;
    }

    has_ip_api_been_processed = 1;
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t get_ip_api_info(ip_api_info_t *info){
    if (info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!has_ip_api_been_processed) {
        return ESP_FAIL;
    }
    memcpy(info, &s_ip_api_info, sizeof(ip_api_info_t));
    return ESP_OK;
}

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
    }
    s_weather_forecast.hourly_count = count;

    has_weather_hourly_been_processed = 1;
    cJSON_Delete(root);
    return ESP_OK;
}

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