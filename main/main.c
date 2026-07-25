/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "rgb_lcd_panel.h"
#include "lcd_ui.h"
#include "wifi.h"
#include "call_api.h"
#include "json_parse.h"
#include "time_sync.h"

static const char *TAG = "display";
static weather_forecast_t s_forecast;

void app_main(void)
{
    ESP_ERROR_CHECK(rgb_lcd_backlight_init());
    ESP_LOGI(TAG, "Initialize LCD backlight");
    ESP_LOGI(TAG, "Turn off LCD backlight");
    rgb_lcd_backlight_set(false);

    ESP_LOGI(TAG, "Create RGB LCD panel");
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(rgb_lcd_panel_new(&panel_handle));

    ESP_LOGI(TAG, "Initialize RGB LCD panel");
    ESP_ERROR_CHECK(rgb_lcd_panel_init(panel_handle));

    ESP_LOGI(TAG, "Turn on LCD backlight");
    rgb_lcd_backlight_set(true);

    ESP_LOGI(TAG, "Initialize UI");
    esp_err_t ret = lcd_ui_init(panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize UI: %s", esp_err_to_name(ret));
        return;
    }

    ret = lcd_ui_wait_until_ready(5000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "UI was not ready in time: %s", esp_err_to_name(ret));
        return;
    }

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
        esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
    }

    ESP_LOGI(TAG, "Initialize WiFi");
    ret = wifi_station_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi");
        return;
    }

    ESP_LOGI(TAG, "Connecting to WiFi");
    ret = wifi_station_connect(DISPLAY_WIFI_SSID, DISPLAY_WIFI_PASS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
        return;
    }

    ret = wifi_station_wait_for_ipv6(15000);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "IPv6 was not ready within timeout: %s", esp_err_to_name(ret));
        return;
    }

    char ipv6[64] = {0};
    ret = wifi_station_get_ipv6(ipv6, sizeof(ipv6));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read station IPv6: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Calling ip-api with IPv6: %s", ipv6);
    ret = call_ip_api_with_ipv6(ipv6);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ip-api call failed: %s", esp_err_to_name(ret));
        return;
    }

    ip_api_info_t ip_info = {0};
    ret = get_ip_api_info(&ip_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get IP geolocation info from parsed response");
        return;
    }

    ESP_LOGI(TAG, "Sync time using UTC offset: %ld (timezone hint: %s)",
             (long)ip_info.utc_offset_seconds,
             ip_info.timezone ? ip_info.timezone : "N/A");
    ret = time_sync_once_with_utc_offset(ip_info.utc_offset_seconds, ip_info.timezone);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SNTP time sync failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Display top time band");
        esp_err_t ui_ret = lcd_ui_show_time_band();
        if (ui_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to render top time band: %s", esp_err_to_name(ui_ret));
        }
    }

    ESP_LOGI(TAG, "Calling Open-Meteo 24h forecast");
    ret = call_weather_api_24h(ip_info.lat, ip_info.lon, ip_info.timezone);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Open-Meteo 24h call failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Calling Open-Meteo 7d forecast");
    ret = call_weather_api_7d(ip_info.lat, ip_info.lon, ip_info.timezone);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Open-Meteo 7d call failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = get_weather_forecast(&s_forecast);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Weather cached: hourly=%u points, daily=%u points, tz=%s",
                 (unsigned)s_forecast.hourly_count,
                 (unsigned)s_forecast.daily_count,
                 s_forecast.timezone[0] ? s_forecast.timezone : "N/A");
    }
}
