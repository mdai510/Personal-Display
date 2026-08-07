/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "cap_touch.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lcd_ui.h"
#include "rgb_lcd_panel.h"
#include "sdkconfig.h"
#include "wifi_call.h"

static const char *TAG = "display";

// Optional manual weather location override.
// Set to 1 to bypass IP geolocation and use the location below.
#define DISPLAY_USE_MANUAL_LOCATION 1
#define DISPLAY_MANUAL_LATITUDE 37.5517
#define DISPLAY_MANUAL_LONGITUDE -121.9519
#define DISPLAY_MANUAL_TIMEZONE "America/Los_Angeles"
#define DISPLAY_MANUAL_UTC_OFFSET_SECONDS (-7 * 3600)

/*
* Main app entry point. Calls initialization functions for the LCD, UI, capacitive touch, and Wi-Fi call service.
*/
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

    ret = lcd_ui_show_starting();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to render startup screen: %s", esp_err_to_name(ret));
        return;
    }

    ret = cap_touch_init();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to initialize capacitive touch: %s",
               esp_err_to_name(ret));
      return;
    }

#if DISPLAY_USE_MANUAL_LOCATION
    ret = wifi_call_set_manual_location(DISPLAY_MANUAL_LATITUDE,
                                        DISPLAY_MANUAL_LONGITUDE,
                                        DISPLAY_MANUAL_TIMEZONE,
                                        DISPLAY_MANUAL_UTC_OFFSET_SECONDS);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure manual location override: %s", esp_err_to_name(ret));
        return;
    }
#endif

    ret = wifi_call_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Wi-Fi call service: %s", esp_err_to_name(ret));
        return;
    }

    ret = lcd_ui_start_weather_coordinator();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start UI weather coordinator: %s", esp_err_to_name(ret));
        return;
    }
}
