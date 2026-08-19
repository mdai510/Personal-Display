#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "lvgl.h"

typedef void (*lcd_ui_render_fn_t)(lv_display_t *disp, void *user_ctx);

esp_err_t lcd_ui_init(esp_lcd_panel_handle_t panel_handle);
esp_err_t lcd_ui_render(lcd_ui_render_fn_t render_fn, void *user_ctx);
esp_err_t lcd_ui_show_time_band(void);
esp_err_t lcd_ui_show_weather_current(void);
esp_err_t lcd_ui_show_starting(void);
esp_err_t lcd_ui_set_location_name(const char *location_name);
esp_err_t lcd_ui_show_message_screen(const char *message);
esp_err_t lcd_ui_show_location_retry_screen(const char *message);
esp_err_t lcd_ui_show_location_change_screen(const char *message);
esp_err_t lcd_ui_set_message_retry_in_progress(bool in_progress);
esp_err_t lcd_ui_start_weather_coordinator(void);
bool lcd_ui_is_initialized(void);
esp_err_t lcd_ui_wait_until_ready(uint32_t timeout_ms);
