/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include "cap_touch.h"
#include "esp_err.h"
#include "esp_log.h"
#include "lcd_ui.h"
#include "ble.h"
#include "rgb_lcd_panel.h"
#include "wifi_call.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "display";

// Optional manual weather location override.
// Set to 1 to bypass BLE location setting and use the location below.
// Set to 0 to use BLE location setting.
#define DISPLAY_USE_MANUAL_LOCATION 0
#define DISPLAY_MANUAL_LATITUDE 32.7157
#define DISPLAY_MANUAL_LONGITUDE -117.1611
#define DISPLAY_MANUAL_LOCATION "San Diego, CA"
#define DISPLAY_MANUAL_TIMEZONE "America/Los_Angeles"
#define DISPLAY_MANUAL_UTC_OFFSET_SECONDS (-7 * 3600)

#define USE_MANUAL_WIFI_CREDENTIALS 0
// For manual setting of SSID and password, go to sdkconfig.h and change
// CONFIG_DISPLAY_WIFI_STA_SSID 
// CONFIG_DISPLAY_WIFI_STA_PASSWORD

/*
* Main app entry point. Calls initialization functions for the LCD, UI, capacitive touch, and Wi-Fi call service.
*/
void app_main(void)
{
    //Initialize Non-Volatile Storage (NVS)
    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = ble_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BLE: %s", esp_err_to_name(ret));
        return;
    }

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
    ret = lcd_ui_init(panel_handle);
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

    wifi_call_set_use_manual_wifi_credentials(USE_MANUAL_WIFI_CREDENTIALS != 0);

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
