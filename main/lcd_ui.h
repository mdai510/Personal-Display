#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "lvgl.h"

typedef void (*lcd_ui_render_fn_t)(lv_display_t *disp, void *user_ctx);

esp_err_t lcd_ui_init(esp_lcd_panel_handle_t panel_handle);
esp_err_t lcd_ui_render(lcd_ui_render_fn_t render_fn, void *user_ctx);
esp_err_t lcd_ui_show_demo(void);
esp_err_t lcd_ui_show_time_band(void);
bool lcd_ui_is_initialized(void);
esp_err_t lcd_ui_wait_until_ready(uint32_t timeout_ms);
