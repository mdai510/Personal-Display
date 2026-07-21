/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_check.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "rgb_lcd_panel.h"

static const char *TAG = "display";

esp_err_t rgb_lcd_backlight_init(void){
#if DISPLAY_PIN_NUM_BK_LIGHT >= 0
    // The RGB panel data interface usually does not control the backlight itself,
    // so drive the dedicated backlight GPIO separately when the board provides one.
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << DISPLAY_PIN_NUM_BK_LIGHT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&bk_gpio_config), TAG, "configure backlight gpio failed");
#endif

    return ESP_OK;
}

void rgb_lcd_backlight_set(bool on){
    if (DISPLAY_PIN_NUM_BK_LIGHT < 0) {
        return;
    }

    gpio_set_level(DISPLAY_PIN_NUM_BK_LIGHT, on ? DISPLAY_LCD_BK_LIGHT_ON_LEVEL : DISPLAY_LCD_BK_LIGHT_OFF_LEVEL);
}

esp_err_t rgb_lcd_panel_new(esp_lcd_panel_handle_t *panel_handle){
    ESP_RETURN_ON_FALSE(panel_handle, ESP_ERR_INVALID_ARG, TAG, "panel handle is null");

    // Fill one esp_lcd_rgb_panel_config_t in one place: bus width, GPIO routing, timing, and frame buffers.
    esp_lcd_rgb_panel_config_t panel_config = {
        .data_width = CONFIG_DISPLAY_LCD_DATA_LINES,
        .dma_burst_size = 64,
        .num_fbs = DISPLAY_RGB_PANEL_NUM_FBS,
    #if CONFIG_DISPLAY_USE_BOUNCE_BUFFER || !CONFIG_DISPLAY_USE_DOUBLE_FB
        // Single-frame-buffer + PSRAM is sensitive to bandwidth stalls (e.g. Wi-Fi).
        // Keep a DRAM bounce buffer enabled to avoid occasional horizontal wrap/shift.
        .bounce_buffer_size_px = 20 * DISPLAY_LCD_H_RES,
#endif
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .disp_gpio_num = DISPLAY_PIN_NUM_DISP_EN,
        .pclk_gpio_num = DISPLAY_PIN_NUM_PCLK,
        .vsync_gpio_num = DISPLAY_PIN_NUM_VSYNC,
        .hsync_gpio_num = DISPLAY_PIN_NUM_HSYNC,
        .de_gpio_num = DISPLAY_PIN_NUM_DE,
        .data_gpio_nums = {
            DISPLAY_PIN_NUM_DATA0,
            DISPLAY_PIN_NUM_DATA1,
            DISPLAY_PIN_NUM_DATA2,
            DISPLAY_PIN_NUM_DATA3,
            DISPLAY_PIN_NUM_DATA4,
            DISPLAY_PIN_NUM_DATA5,
            DISPLAY_PIN_NUM_DATA6,
            DISPLAY_PIN_NUM_DATA7,
            DISPLAY_PIN_NUM_DATA8,
            DISPLAY_PIN_NUM_DATA9,
            DISPLAY_PIN_NUM_DATA10,
            DISPLAY_PIN_NUM_DATA11,
            DISPLAY_PIN_NUM_DATA12,
            DISPLAY_PIN_NUM_DATA13,
            DISPLAY_PIN_NUM_DATA14,
            DISPLAY_PIN_NUM_DATA15,
    #if CONFIG_DISPLAY_LCD_DATA_LINES > 16
            DISPLAY_PIN_NUM_DATA16,
            DISPLAY_PIN_NUM_DATA17,
            DISPLAY_PIN_NUM_DATA18,
            DISPLAY_PIN_NUM_DATA19,
            DISPLAY_PIN_NUM_DATA20,
            DISPLAY_PIN_NUM_DATA21,
            DISPLAY_PIN_NUM_DATA22,
            DISPLAY_PIN_NUM_DATA23,
#endif
        },
        .timings = {
            .pclk_hz = DISPLAY_LCD_PIXEL_CLOCK_HZ,
            .h_res = DISPLAY_LCD_H_RES,
            .v_res = DISPLAY_LCD_V_RES,
            .hsync_back_porch = DISPLAY_LCD_HBP,
            .hsync_front_porch = DISPLAY_LCD_HFP,
            .hsync_pulse_width = DISPLAY_LCD_HSYNC,
            .vsync_back_porch = DISPLAY_LCD_VBP,
            .vsync_front_porch = DISPLAY_LCD_VFP,
            .vsync_pulse_width = DISPLAY_LCD_VSYNC,
            .flags = {
                .pclk_active_neg = true,
                .pclk_idle_high = false,
                .de_idle_high = false,
                .vsync_idle_low = false,
                .hsync_idle_low = false,
            },
        },
        .flags.fb_in_psram = true,
    };
    return esp_lcd_new_rgb_panel(&panel_config, panel_handle);
}

esp_err_t rgb_lcd_panel_init(esp_lcd_panel_handle_t panel_handle){
    ESP_RETURN_ON_FALSE(panel_handle, ESP_ERR_INVALID_ARG, TAG, "panel handle is null");
    // reset() applies the panel reset sequence, while init() starts the RGB output engine.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_handle), TAG, "reset RGB panel failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_handle), TAG, "init RGB panel failed");
    return ESP_OK;
}
