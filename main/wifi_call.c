#include "wifi_call.h"

#include "call_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "json_parse.h"
#include "time_sync.h"
#include "wifi.h"
#include <math.h>
#include <string.h>

#define WIFI_CALL_TASK_STACK_SIZE (6 * 1024)
#define WIFI_CALL_TASK_PRIORITY 1
#define WIFI_CALL_INTERVAL_MS (30 * 60 * 1000) //30 mins between each weather api call 
#define WIFI_CALL_IPV6_WAIT_MS 15000
#define WIFI_CALL_TZ_MAX_LEN 64

#define WEATHER_READY_BIT BIT0

static const char *TAG = "wifi_call";
static TaskHandle_t s_wifi_call_task_handle;
// Event group to signal when the weather information is ready after a refresh cycle.
static EventGroupHandle_t s_weather_event_group;
static ip_api_info_t s_ip_info;
static bool s_ip_info_ready = false;

typedef struct {
    bool enabled;
    double lat;
    double lon;
    char timezone[WIFI_CALL_TZ_MAX_LEN];
    int32_t utc_offset_seconds;
} manual_location_override_t;

static manual_location_override_t s_manual_location_override = {
    .enabled = false,
};

/*
* Call the weather API once, fetching both the 24-hour and 7-day forecasts. This function assumes that the Wi-Fi station is already connected and that the IP geolocation info has been obtained.
*/
esp_err_t wifi_call_fetch_weather_once(void){
    if (!wifi_station_is_connected()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!s_ip_info_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret;

    ESP_LOGI(TAG, "Calling Open-Meteo 24h forecast");
    ret = call_weather_api_24h(s_ip_info.lat, s_ip_info.lon, s_ip_info.timezone);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Open-Meteo 24h call failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "Calling Open-Meteo 7d forecast");
    ret = call_weather_api_7d(s_ip_info.lat, s_ip_info.lon, s_ip_info.timezone);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Open-Meteo 7d call failed: %s", esp_err_to_name(ret));
        return ret;
    }

    weather_forecast_t forecast = {0};
    ret = get_weather_forecast(&forecast);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Weather cached: hourly=%u points, daily=%u points, tz=%s",
                 (unsigned)forecast.hourly_count,
                 (unsigned)forecast.daily_count,
                 forecast.timezone[0] ? forecast.timezone : "N/A");

        if (s_weather_event_group != NULL) {
            xEventGroupSetBits(s_weather_event_group, WEATHER_READY_BIT);
        }
    } else {
        ESP_LOGE(TAG, "Failed to get weather forecast after parse");
    }
    return ret;
}

/*
 * Refresh the weather information once. This function handles Wi-Fi connection, IP geolocation, and time synchronization before fetching the weather.
 */
static esp_err_t wifi_call_refresh_once(void)
{
    if (s_weather_event_group != NULL) {
        xEventGroupClearBits(s_weather_event_group, WEATHER_READY_BIT);
    }

    esp_err_t ret = wifi_station_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = wifi_station_connect(DISPLAY_WIFI_SSID, DISPLAY_WIFI_PASS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi connect failed: %s", esp_err_to_name(ret));
        goto done;
    }

    if (!s_ip_info_ready){
        if (s_manual_location_override.enabled){
            memset(&s_ip_info, 0, sizeof(s_ip_info));
            s_ip_info.lat = s_manual_location_override.lat;
            s_ip_info.lon = s_manual_location_override.lon;
            s_ip_info.timezone = s_manual_location_override.timezone;
            s_ip_info.utc_offset_seconds = s_manual_location_override.utc_offset_seconds;
            s_ip_info_ready = true;

            ESP_LOGI(TAG,
                     "Using manual location override lat=%.6f lon=%.6f tz=%s offset=%ld",
                     s_ip_info.lat,
                     s_ip_info.lon,
                     s_ip_info.timezone,
                     (long)s_ip_info.utc_offset_seconds);
        } 
        else{
            ret = wifi_station_wait_for_ipv6(WIFI_CALL_IPV6_WAIT_MS);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "IPv6 was not ready within timeout: %s", esp_err_to_name(ret));
                goto done;
            }

            char ipv6[64] = {0};
            ret = wifi_station_get_ipv6(ipv6, sizeof(ipv6));
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to read station IPv6: %s", esp_err_to_name(ret));
                goto done;
            }

            ESP_LOGI(TAG, "Calling ip-api with IPv6: %s", ipv6);
            ret = call_ip_api_with_ipv6(ipv6);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "ip-api call failed: %s", esp_err_to_name(ret));
                goto done;
            }

            memset(&s_ip_info, 0, sizeof(s_ip_info));
            ret = get_ip_api_info(&s_ip_info);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to get IP geolocation info from parsed response");
                goto done;
            }

            s_ip_info_ready = true;
        }

        ESP_LOGI(TAG, "Sync time using UTC offset: %ld (timezone hint: %s)",
                 (long)s_ip_info.utc_offset_seconds,
                 s_ip_info.timezone ? s_ip_info.timezone : "N/A");
        ret = time_sync_once_with_utc_offset(s_ip_info.utc_offset_seconds, s_ip_info.timezone);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SNTP time sync failed: %s", esp_err_to_name(ret));
        }
    }

    ret = wifi_call_fetch_weather_once();

done:
    {
        esp_err_t deinit_ret = wifi_station_deinit();
        if (deinit_ret != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi deinit failed: %s", esp_err_to_name(deinit_ret));
        }
    }
    return ret;
}

/*
 * Task that periodically refreshes the weather information.
 * It waits for either a manual refresh request or the configured interval before performing the refresh.
 */
static void wifi_call_task(void *arg)
{
    (void)arg;
    const TickType_t interval_ticks = pdMS_TO_TICKS(WIFI_CALL_INTERVAL_MS);

    while (1) {
        // On startup, perform an immediate refresh.
        ESP_LOGI(TAG, "Starting weather refresh cycle");
        esp_err_t ret = wifi_call_refresh_once();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Weather cycle completed with errors: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "Weather cycle completed successfully");
        }

        // Blocks until either the interval elapses or a manual refresh is requested.
        uint32_t notify_count = ulTaskNotifyTake(pdTRUE, interval_ticks);
        if (notify_count > 0) {
            ESP_LOGI(TAG, "Manual weather refresh requested");
        }
    }
}

/*
 * Start the Wi-Fi call service, creating the necessary task and event group if they do not already exist.
 */
esp_err_t wifi_call_start(void)
{
    if (s_wifi_call_task_handle != NULL) {
        return ESP_OK;
    }

    if (s_weather_event_group == NULL) {
        s_weather_event_group = xEventGroupCreate();
        if (s_weather_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xEventGroupClearBits(s_weather_event_group, WEATHER_READY_BIT);

    BaseType_t created = xTaskCreate(
        wifi_call_task,
        "wifi_call",
        WIFI_CALL_TASK_STACK_SIZE,
        NULL,
        WIFI_CALL_TASK_PRIORITY,
        &s_wifi_call_task_handle);

    if (created != pdPASS) {
        s_wifi_call_task_handle = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

/*
 * Check weather event group to see if weather info is ready.
 */
bool wifi_call_is_weather_ready(void)
{
    if (s_weather_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_weather_event_group) & WEATHER_READY_BIT) != 0;
}

/*
 * Wait until the weather information is ready or the specified timeout elapses.
 */
esp_err_t wifi_call_wait_weather_ready(uint32_t timeout_ms)
{
    if (s_weather_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t wait_ticks = (timeout_ms == portMAX_DELAY)
                                ? portMAX_DELAY
                                : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_weather_event_group,
                                           WEATHER_READY_BIT,
                                           pdTRUE,
                                           pdFALSE,
                                           wait_ticks);
    return (bits & WEATHER_READY_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/*
 * Request a manual refresh of the weather information.
 */
esp_err_t wifi_call_request_refresh(void)
{
    if (s_wifi_call_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    // Notify wifi call task to perform a refresh immediately.
    xTaskNotifyGive(s_wifi_call_task_handle);
    return ESP_OK;
}

esp_err_t wifi_call_set_manual_location(double latitude,
                                        double longitude,
                                        const char *timezone,
                                        int32_t utc_offset_seconds)
{
    if (isnan(latitude) || isnan(longitude) || latitude < -90.0 || latitude > 90.0 ||
        longitude < -180.0 || longitude > 180.0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (timezone == NULL || timezone[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    size_t tz_len = strnlen(timezone, WIFI_CALL_TZ_MAX_LEN);
    if (tz_len == WIFI_CALL_TZ_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_manual_location_override.lat = latitude;
    s_manual_location_override.lon = longitude;
    s_manual_location_override.utc_offset_seconds = utc_offset_seconds;
    memcpy(s_manual_location_override.timezone, timezone, tz_len + 1);
    s_manual_location_override.enabled = true;

    // Force a refresh cycle to rebuild location/time settings from this override.
    s_ip_info_ready = false;

    ESP_LOGI(TAG,
             "Manual location override configured lat=%.6f lon=%.6f tz=%s offset=%ld",
             latitude,
             longitude,
             s_manual_location_override.timezone,
             (long)utc_offset_seconds);

    return ESP_OK;
}
