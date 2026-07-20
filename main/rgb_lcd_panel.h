/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_lcd_panel_rgb.h"

#ifdef __cplusplus
extern "C" {
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your
///LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Example timing:
// Refresh rate = 18 MHz / (HSYNC + HBP + H_RES + HFP) / (VSYNC + VBP + V_RES +
// VFP) = about 42 Hz CrowPanel DIS08070H V3: 800x480 RGB565 panel
#define DISPLAY_LCD_PIXEL_CLOCK_HZ (15 * 1000 * 1000)

#define DISPLAY_LCD_H_RES 800
#define DISPLAY_LCD_V_RES 480

#define DISPLAY_LCD_HSYNC 48
#define DISPLAY_LCD_HBP 40
#define DISPLAY_LCD_HFP 40

#define DISPLAY_LCD_VSYNC 31
#define DISPLAY_LCD_VBP 13
#define DISPLAY_LCD_VFP 1

#define DISPLAY_LCD_BK_LIGHT_ON_LEVEL 1
#define DISPLAY_LCD_BK_LIGHT_OFF_LEVEL 0

// Set these two GPIOs to -1 when the panel backlight or display-enable pin is
// fixed on the board.
#define DISPLAY_PIN_NUM_BK_LIGHT 2
#define DISPLAY_PIN_NUM_DISP_EN -1

#define DISPLAY_PIN_NUM_HSYNC                           CONFIG_DISPLAY_LCD_HSYNC_GPIO
#define DISPLAY_PIN_NUM_VSYNC                           CONFIG_DISPLAY_LCD_VSYNC_GPIO
#define DISPLAY_PIN_NUM_DE                              CONFIG_DISPLAY_LCD_DE_GPIO
#define DISPLAY_PIN_NUM_PCLK                            CONFIG_DISPLAY_LCD_PCLK_GPIO

#define DISPLAY_PIN_NUM_DATA0                           CONFIG_DISPLAY_LCD_DATA0_GPIO
#define DISPLAY_PIN_NUM_DATA1                           CONFIG_DISPLAY_LCD_DATA1_GPIO
#define DISPLAY_PIN_NUM_DATA2                           CONFIG_DISPLAY_LCD_DATA2_GPIO
#define DISPLAY_PIN_NUM_DATA3                           CONFIG_DISPLAY_LCD_DATA3_GPIO
#define DISPLAY_PIN_NUM_DATA4                           CONFIG_DISPLAY_LCD_DATA4_GPIO
#define DISPLAY_PIN_NUM_DATA5                           CONFIG_DISPLAY_LCD_DATA5_GPIO
#define DISPLAY_PIN_NUM_DATA6                           CONFIG_DISPLAY_LCD_DATA6_GPIO
#define DISPLAY_PIN_NUM_DATA7                           CONFIG_DISPLAY_LCD_DATA7_GPIO
#define DISPLAY_PIN_NUM_DATA8                           CONFIG_DISPLAY_LCD_DATA8_GPIO
#define DISPLAY_PIN_NUM_DATA9                           CONFIG_DISPLAY_LCD_DATA9_GPIO
#define DISPLAY_PIN_NUM_DATA10                          CONFIG_DISPLAY_LCD_DATA10_GPIO
#define DISPLAY_PIN_NUM_DATA11                          CONFIG_DISPLAY_LCD_DATA11_GPIO
#define DISPLAY_PIN_NUM_DATA12                          CONFIG_DISPLAY_LCD_DATA12_GPIO
#define DISPLAY_PIN_NUM_DATA13                          CONFIG_DISPLAY_LCD_DATA13_GPIO
#define DISPLAY_PIN_NUM_DATA14                          CONFIG_DISPLAY_LCD_DATA14_GPIO
#define DISPLAY_PIN_NUM_DATA15                          CONFIG_DISPLAY_LCD_DATA15_GPIO
#if CONFIG_DISPLAY_LCD_DATA_LINES > 16
#define DISPLAY_PIN_NUM_DATA16                          CONFIG_DISPLAY_LCD_DATA16_GPIO
#define DISPLAY_PIN_NUM_DATA17                          CONFIG_DISPLAY_LCD_DATA17_GPIO
#define DISPLAY_PIN_NUM_DATA18                          CONFIG_DISPLAY_LCD_DATA18_GPIO
#define DISPLAY_PIN_NUM_DATA19                          CONFIG_DISPLAY_LCD_DATA19_GPIO
#define DISPLAY_PIN_NUM_DATA20                          CONFIG_DISPLAY_LCD_DATA20_GPIO
#define DISPLAY_PIN_NUM_DATA21                          CONFIG_DISPLAY_LCD_DATA21_GPIO
#define DISPLAY_PIN_NUM_DATA22                          CONFIG_DISPLAY_LCD_DATA22_GPIO
#define DISPLAY_PIN_NUM_DATA23                          CONFIG_DISPLAY_LCD_DATA23_GPIO
#endif

#if CONFIG_DISPLAY_USE_DOUBLE_FB
#define DISPLAY_RGB_PANEL_NUM_FBS                       2
#else
#define DISPLAY_RGB_PANEL_NUM_FBS                       1
#endif

// One RGB565 pixel uses 2 bytes, one RGB888 pixel uses 3 bytes.
#if (defined(CONFIG_DISPLAY_LCD_DATA_LINES_16) && CONFIG_DISPLAY_LCD_DATA_LINES_16) || \
	(defined(CONFIG_DISPLAY_LCD_DATA_LINES) && (CONFIG_DISPLAY_LCD_DATA_LINES == 16))
#define DISPLAY_PIXEL_SIZE                              2
#elif (defined(CONFIG_DISPLAY_LCD_DATA_LINES_24) && CONFIG_DISPLAY_LCD_DATA_LINES_24) || \
	  (defined(CONFIG_DISPLAY_LCD_DATA_LINES) && (CONFIG_DISPLAY_LCD_DATA_LINES == 24))
#define DISPLAY_PIXEL_SIZE                              3
#else
#define DISPLAY_PIXEL_SIZE                              2
#endif

esp_err_t rgb_lcd_backlight_init(void);
void rgb_lcd_backlight_set(bool on);
esp_err_t rgb_lcd_panel_new(esp_lcd_panel_handle_t *panel_handle);
esp_err_t rgb_lcd_panel_init(esp_lcd_panel_handle_t panel_handle);

#ifdef __cplusplus
}
#endif
