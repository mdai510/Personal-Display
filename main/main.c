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
