/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <time.h>
#include <sys/lock.h>
#include <sys/param.h>
#include "freertos/idf_additions.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "rgb_lcd_panel.h"
#include "json_parse.h"
#include "lcd_ui.h"
#include "cap_touch.h"
#include "wifi_call.h"

LV_FONT_DECLARE(weather_icons_48);
LV_FONT_DECLARE(weather_icons_24);

static const char *TAG = "lcd_ui";

// LVGL draw buffers must use the same pixel format as the RGB panel output.
#if (defined(CONFIG_DISPLAY_LCD_DATA_LINES_16) && CONFIG_DISPLAY_LCD_DATA_LINES_16) || \
    (defined(CONFIG_DISPLAY_LCD_DATA_LINES) && (CONFIG_DISPLAY_LCD_DATA_LINES == 16))
#define DISPLAY_LV_COLOR_FORMAT      LV_COLOR_FORMAT_RGB565
#elif (defined(CONFIG_DISPLAY_LCD_DATA_LINES_24) && CONFIG_DISPLAY_LCD_DATA_LINES_24) || \
      (defined(CONFIG_DISPLAY_LCD_DATA_LINES) && (CONFIG_DISPLAY_LCD_DATA_LINES == 24))
#define DISPLAY_LV_COLOR_FORMAT      LV_COLOR_FORMAT_RGB888
#else
#define DISPLAY_LV_COLOR_FORMAT      LV_COLOR_FORMAT_RGB565
#endif

#define DISPLAY_LVGL_DRAW_BUF_LINES    30
#define DISPLAY_LVGL_TICK_PERIOD_MS    2
#define DISPLAY_LVGL_TASK_STACK_SIZE   (16 * 1024)
#define DISPLAY_LVGL_TASK_PRIORITY     2
#define DISPLAY_LVGL_TASK_MAX_DELAY_MS 500
#define DISPLAY_LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ
#define UI_WEATHER_COORD_TASK_STACK_SIZE (4 * 1024)
#define UI_WEATHER_COORD_TASK_PRIORITY 2

#define UI_READY_BIT BIT0
#define WEATHER_PANEL_TOP_OFFSET 44
#define FORECAST_DAYS_VISIBLE 3
#define FORECAST_TOGGLE_PANEL_W 32
#define TIME_BAND_REFRESH_TOUCH_W 36
#define HOURLY_FORECAST_PER_PAGE 8
#define HOURLY_FORECAST_PAGE_COUNT 3

#define UI_COLOR_PRIMARY_HEX 0xFFFFFF
#define UI_COLOR_HUMIDITY_PRECIP_HEX 0x33A1FF

#define WEATHER_PANEL_HALF_H ((DISPLAY_LCD_V_RES - WEATHER_PANEL_TOP_OFFSET) / 2)
#define WEATHER_LEFT_PANEL_W ((DISPLAY_LCD_H_RES * 50) / 100)
#define WEATHER_RIGHT_AVAILABLE_W (DISPLAY_LCD_H_RES - WEATHER_LEFT_PANEL_W)
#define WEATHER_TOGGLE_PANEL_ACTUAL_W ((WEATHER_RIGHT_AVAILABLE_W <= FORECAST_TOGGLE_PANEL_W + 1) ? 1 : FORECAST_TOGGLE_PANEL_W)
#define WEATHER_FORECAST_PANEL_W (WEATHER_RIGHT_AVAILABLE_W - WEATHER_TOGGLE_PANEL_ACTUAL_W)

#define PANEL_X1(panel) ((panel).x)
#define PANEL_Y1(panel) ((panel).y)
#define PANEL_X2(panel) ((panel).x + (panel).w - 1)
#define PANEL_Y2(panel) ((panel).y + (panel).h - 1)

typedef struct {
    lv_obj_t *root;
    int32_t x;
    int32_t y;
    lv_coord_t w;
    lv_coord_t h;
    bool visible;
} lcd_ui_panel_t;

typedef enum {
    WEATHER_ICON_SUN = 0,
    WEATHER_ICON_MOON,
    WEATHER_ICON_SUN_PARTLY_CLOUDY,
    WEATHER_ICON_MOON_PARTLY_CLOUDY,
    WEATHER_ICON_SUN_CLOUDY,
    WEATHER_ICON_MOON_CLOUDY,
    WEATHER_ICON_SINGLE_CLOUD,
    WEATHER_ICON_DOUBLE_CLOUD,
    WEATHER_ICON_FOG,
    WEATHER_ICON_NIGHT_FOG,
    WEATHER_ICON_DRIZZLE,
    WEATHER_ICON_NIGHT_DRIZZLE,
    WEATHER_ICON_SLEET,
    WEATHER_ICON_NIGHT_SLEET,
    WEATHER_ICON_RAIN,
    WEATHER_ICON_NIGHT_RAIN,
    WEATHER_ICON_FREEZING_RAIN,
    WEATHER_ICON_NIGHT_FREEZING_RAIN,
    WEATHER_ICON_SNOW,
    WEATHER_ICON_NIGHT_SNOW,
    WEATHER_ICON_SHOWERS,
    WEATHER_ICON_NIGHT_SHOWERS,
    WEATHER_ICON_THUNDERSTORM,
    WEATHER_ICON_NIGHT_THUNDERSTORM,
    WEATHER_ICON_RAINDROP,
    WEATHER_ICON_SNOWFLAKE,
    WEATHER_ICON_WIND_GUST,
    WEATHER_ICON_THERMOMETER,
    WEATHER_ICON_SUNRISE,
    WEATHER_ICON_SUNSET,
    WEATHER_ICON_REFRESH,
    WEATHER_ICON_HUMIDITY,
    WEATHER_ICON_ARROW_UP,
    WEATHER_ICON_ARROW_DOWN,
    WEATHER_ICON_ARROW_RIGHT,
    WEATHER_ICON_ARROW_LEFT,
    WEATHER_ICON_COUNT
} weather_icon_id_t;

typedef enum {
    WEATHER_DAILY_VIEW_TODAY = 0,
    WEATHER_DAILY_VIEW_NEXT3,
} weather_daily_view_t;

static const uint32_t s_weather_icon_codepoint_lookup_48[WEATHER_ICON_COUNT] = {
    [WEATHER_ICON_SUN] = 0xF00D,
    [WEATHER_ICON_MOON] = 0xF02E,
    [WEATHER_ICON_SUN_PARTLY_CLOUDY] = 0xF00C,
    [WEATHER_ICON_MOON_PARTLY_CLOUDY] = 0xF081,
    [WEATHER_ICON_SUN_CLOUDY] = 0xF002,
    [WEATHER_ICON_MOON_CLOUDY] = 0xF086,
    [WEATHER_ICON_SINGLE_CLOUD] = 0xF041,
    [WEATHER_ICON_DOUBLE_CLOUD] = 0xF013,
    [WEATHER_ICON_FOG] = 0xF014,
    [WEATHER_ICON_NIGHT_FOG] = 0xF04A,
    [WEATHER_ICON_DRIZZLE] = 0xF01C,
    [WEATHER_ICON_NIGHT_DRIZZLE] = 0xF02B,
    [WEATHER_ICON_SLEET] = 0xF0B5,
    [WEATHER_ICON_NIGHT_SLEET] = 0xF0B4,
    [WEATHER_ICON_RAIN] = 0xF01A,
    [WEATHER_ICON_NIGHT_RAIN] = 0xF029,
    [WEATHER_ICON_FREEZING_RAIN] = 0xF017,
    [WEATHER_ICON_NIGHT_FREEZING_RAIN] = 0xF026,
    [WEATHER_ICON_SNOW] = 0xF01B,
    [WEATHER_ICON_NIGHT_SNOW] = 0xF02A,
    [WEATHER_ICON_SHOWERS] = 0xF019,
    [WEATHER_ICON_NIGHT_SHOWERS] = 0xF028,
    [WEATHER_ICON_THUNDERSTORM] = 0xF01E,
    [WEATHER_ICON_NIGHT_THUNDERSTORM] = 0xF02D,
};

static const uint32_t s_weather_icon_codepoint_lookup_24[WEATHER_ICON_COUNT] = {
    [WEATHER_ICON_SUN] = 0xF00D,
    [WEATHER_ICON_MOON] = 0xF02E,
    [WEATHER_ICON_SUN_PARTLY_CLOUDY] = 0xF00C,
    [WEATHER_ICON_MOON_PARTLY_CLOUDY] = 0xF081,
    [WEATHER_ICON_SUN_CLOUDY] = 0xF002,
    [WEATHER_ICON_MOON_CLOUDY] = 0xF086,
    [WEATHER_ICON_SINGLE_CLOUD] = 0xF041,
    [WEATHER_ICON_DOUBLE_CLOUD] = 0xF013,
    [WEATHER_ICON_FOG] = 0xF014,
    [WEATHER_ICON_NIGHT_FOG] = 0xF04A,
    [WEATHER_ICON_DRIZZLE] = 0xF01C,
    [WEATHER_ICON_NIGHT_DRIZZLE] = 0xF02B,
    [WEATHER_ICON_SLEET] = 0xF0B5,
    [WEATHER_ICON_NIGHT_SLEET] = 0xF0B4,
    [WEATHER_ICON_RAIN] = 0xF01A,
    [WEATHER_ICON_NIGHT_RAIN] = 0xF029,
    [WEATHER_ICON_FREEZING_RAIN] = 0xF017,
    [WEATHER_ICON_NIGHT_FREEZING_RAIN] = 0xF026,
    [WEATHER_ICON_SNOW] = 0xF01B,
    [WEATHER_ICON_NIGHT_SNOW] = 0xF02A,
    [WEATHER_ICON_SHOWERS] = 0xF019,
    [WEATHER_ICON_NIGHT_SHOWERS] = 0xF028,
    [WEATHER_ICON_THUNDERSTORM] = 0xF01E,
    [WEATHER_ICON_NIGHT_THUNDERSTORM] = 0xF02D,
    [WEATHER_ICON_RAINDROP] = 0xF078,
    [WEATHER_ICON_SNOWFLAKE] = 0xF076,
    [WEATHER_ICON_WIND_GUST] = 0xF050,
    [WEATHER_ICON_THERMOMETER] = 0xF055,
    [WEATHER_ICON_SUNRISE] = 0xF051,
    [WEATHER_ICON_SUNSET] = 0xF052,
    [WEATHER_ICON_REFRESH] = 0xF04C,
    [WEATHER_ICON_HUMIDITY] = 0xF07A,
    [WEATHER_ICON_ARROW_UP] = 0xF058,
    [WEATHER_ICON_ARROW_DOWN] = 0xF044,
    [WEATHER_ICON_ARROW_RIGHT] = 0xF04D,
    [WEATHER_ICON_ARROW_LEFT] = 0xF048,
};

static _lock_t s_lvgl_api_lock;
static TaskHandle_t s_lvgl_task_handle;
static TaskHandle_t s_ui_weather_task_handle;
static EventGroupHandle_t s_ui_event_group;
static lv_display_t *s_display;
static esp_timer_handle_t s_lvgl_tick_timer;

static const lv_font_t *font_normal = &lv_font_montserrat_16;
static lcd_ui_panel_t s_starting_panel;
static lcd_ui_panel_t s_time_band_panel;
static lcd_ui_panel_t s_weather_panel;
static lcd_ui_panel_t s_weather_hourly_panel;
static lcd_ui_panel_t s_weather_forecast_panel;
static lcd_ui_panel_t s_weather_forecast_toggle_panel;
static weather_daily_view_t s_weather_daily_view = WEATHER_DAILY_VIEW_TODAY;
static lv_obj_t *s_starting_label = NULL;
static lv_obj_t *s_date_label = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_last_refreshed_label = NULL;
static lv_obj_t *s_refresh_icon_label = NULL;
static lv_timer_t *s_time_timer = NULL;
static lv_obj_t *s_weather_header_row = NULL;
static lv_obj_t *s_weather_left_col = NULL;
static lv_obj_t *s_weather_right_col = NULL;
static lv_obj_t *s_weather_icon_label = NULL;
static lv_obj_t *s_weather_desc_label = NULL;
static lv_obj_t *s_weather_temp_label = NULL;
static lv_obj_t *s_weather_humidity_icon_label = NULL;
static lv_obj_t *s_weather_humidity_label = NULL;
static lv_obj_t *s_weather_wind_icon_label = NULL;
static lv_obj_t *s_weather_wind_label = NULL;
static lv_obj_t *s_weather_cloud_icon_label = NULL;
static lv_obj_t *s_weather_cloud_label = NULL;
static lv_obj_t *s_hourly_panel = NULL;
static lv_obj_t *s_hourly_nav_row = NULL;
static lv_obj_t *s_hourly_strip = NULL;
static lv_obj_t *s_hourly_left_button_label = NULL;
static lv_obj_t *s_hourly_page_label = NULL;
static lv_obj_t *s_hourly_right_button_label = NULL;
static lv_obj_t *s_hourly_hour_labels[HOURLY_FORECAST_PER_PAGE] = {0};
static lv_obj_t *s_hourly_icon_labels[HOURLY_FORECAST_PER_PAGE] = {0};
static lv_obj_t *s_hourly_hilo_labels[HOURLY_FORECAST_PER_PAGE] = {0};
static lv_obj_t *s_hourly_pop_icon_labels[HOURLY_FORECAST_PER_PAGE] = {0};
static lv_obj_t *s_hourly_pop_labels[HOURLY_FORECAST_PER_PAGE] = {0};
static lv_obj_t *s_weather_hilo_label = NULL;
static lv_obj_t *s_weather_today_pop_icon_label = NULL;
static lv_obj_t *s_weather_today_pop_label = NULL;
static lv_obj_t *s_weather_sunrise_icon_label = NULL;
static lv_obj_t *s_weather_sunrise_label = NULL;
static lv_obj_t *s_weather_sunset_icon_label = NULL;
static lv_obj_t *s_weather_sunset_label = NULL;
static lv_obj_t *s_forecast_toggle_button_label = NULL;
static lv_obj_t *s_forecast_date_labels[FORECAST_DAYS_VISIBLE] = {0};
static lv_obj_t *s_forecast_icon_labels[FORECAST_DAYS_VISIBLE] = {0};
static lv_obj_t *s_forecast_hilo_labels[FORECAST_DAYS_VISIBLE] = {0};
static lv_obj_t *s_forecast_pop_icon_labels[FORECAST_DAYS_VISIBLE] = {0};
static lv_obj_t *s_forecast_pop_labels[FORECAST_DAYS_VISIBLE] = {0};
static time_t s_last_weather_refresh_time = 0;
static bool s_last_weather_refresh_valid = false;
static bool s_weather_refresh_in_progress = false;
static int s_hourly_forecast_page = 0;

static bool weather_icon_font_has_glyph(const lv_font_t *font, uint32_t codepoint);
static uint32_t weather_icon_get_codepoint(const lv_font_t *font, weather_icon_id_t icon_id);
static weather_icon_id_t weather_code_to_icon_id(int code, bool is_day);
static bool set_label_to_icon_id(lv_obj_t *label, const lv_font_t *font, weather_icon_id_t icon_id);
static lv_obj_t *create_weather_metric_row(lv_obj_t *parent, lv_obj_t **icon_label_out, lv_obj_t **value_label_out);
static lv_obj_t *create_weather_dual_metric_row(lv_obj_t *parent,
                                                lv_obj_t **left_icon_label_out,
                                                lv_obj_t **left_value_label_out,
                                                lv_obj_t **right_icon_label_out,
                                                lv_obj_t **right_value_label_out);
static lv_obj_t *create_weather_header_row(lv_obj_t *parent, lv_obj_t **icon_label_out, lv_obj_t **temp_label_out);
static lv_obj_t *create_weather_right_icon_row(lv_obj_t *parent, lv_obj_t **icon_label_out, lv_obj_t **value_label_out);
static lv_obj_t *create_weather_forecast_row(lv_obj_t *parent,
                                             lv_obj_t **date_label_out,
                                             lv_obj_t **icon_label_out,
                                             lv_obj_t **hilo_label_out,
                                             lv_obj_t **pop_icon_label_out,
                                             lv_obj_t **pop_label_out);
static void set_weather_daily_view(weather_daily_view_t view);
static bool handle_daily_view_touch(const touch_event_t *event);
static bool handle_hourly_forecast_touch(const touch_event_t *event);
static void set_hourly_forecast_page(int page);
static bool handle_time_band_refresh_touch(const touch_event_t *event);
static bool point_in_rect(int x, int y, int x1, int y1, int x2, int y2);
static bool format_local_date_with_day_offset(size_t day_offset, char *out, size_t out_len);
static size_t get_hourly_display_start_index(const weather_forecast_t *forecast);
static lv_color_t weather_icon_color_from_temp_f(float temp_f);
static void set_icon_temp_color(lv_obj_t *label, float temp_f);
static const char *weather_code_to_interpretation(int code);
static void update_last_refreshed_label(void);
static lv_obj_t *create_hourly_forecast_cell(lv_obj_t *parent,
                                             lv_obj_t **hour_label_out,
                                             lv_obj_t **icon_label_out,
                                             lv_obj_t **hilo_label_out,
                                             lv_obj_t **pop_icon_label_out,
                                             lv_obj_t **pop_label_out);

/*
 * Format a floating-point value with 1 decimal place, including optional prefix and suffix.
 */
static void format_value_1dp(char *buf, size_t buf_len, const char *prefix, float value, const char *suffix)
{
    int scaled = (int)(value * 10.0f + (value >= 0.0f ? 0.5f : -0.5f));
    int abs_scaled = (scaled < 0) ? -scaled : scaled;
    int whole = abs_scaled / 10;
    int frac = abs_scaled % 10;
    snprintf(buf, buf_len, "%s%s%d.%d%s", prefix, (scaled < 0) ? "-" : "", whole, frac, suffix);
}

/*
 * Map Fahrenheit temperature bands to icon colors.
 */
static lv_color_t weather_icon_color_from_temp_f(float temp_f)
{
    if (temp_f >= 100.0f) {
        return lv_color_hex(0xFF1A1A); // bright red
    }
    if (temp_f >= 90.0f) {
        return lv_color_hex(0xFF5A00); // vivid dark orange
    }
    if (temp_f >= 80.0f) {
        return lv_color_hex(0xFFA300); // vivid orange
    }
    if (temp_f >= 70.0f) {
        return lv_color_hex(0x66FF66); // bright light green
    }
    if (temp_f >= 60.0f) {
        return lv_color_hex(0x00C853); // vivid green
    }
    if (temp_f >= 50.0f) {
        return lv_color_hex(0x00E5C8); // bright teal
    }
    if (temp_f >= 40.0f) {
        return lv_color_hex(0x5EC8FF); // vivid light blue
    }
    if (temp_f >= 30.0f) {
        return lv_color_hex(0x2F7DFF); // vivid blue
    }
    if (temp_f >= 20.0f) {
        return lv_color_hex(0x0050FF); // vivid dark blue
    }
    if (temp_f >= 10.0f) {
        return lv_color_hex(0x0030CC); // vivid navy
    }
    if (temp_f >= 0.0f) {
        return lv_color_hex(0x9B00FF); // vivid purple
    }
    return lv_color_hex(0xFF00C8); // vivid magenta
}

/*
 * Apply temperature-derived color to a weather icon label.
 */
static void set_icon_temp_color(lv_obj_t *label, float temp_f)
{
    if (label == NULL) {
        return;
    }
    lv_obj_set_style_text_color(label, weather_icon_color_from_temp_f(temp_f), 0);
}

/*
 * Encode a Unicode codepoint as UTF-8.
 */
static size_t utf8_encode_codepoint(char *dst, size_t dst_len, uint32_t codepoint)
{
    if (dst == NULL || dst_len == 0) {
        return 0;
    }

    if (codepoint <= 0x7F) {
        if (dst_len < 1) {
            return 0;
        }
        dst[0] = (char)codepoint;
        return 1;
    }
    if (codepoint <= 0x7FF) {
        if (dst_len < 2) {
            return 0;
        }
        dst[0] = (char)(0xC0 | ((codepoint >> 6) & 0x1F));
        dst[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint <= 0xFFFF) {
        if (dst_len < 3) {
            return 0;
        }
        dst[0] = (char)(0xE0 | ((codepoint >> 12) & 0x0F));
        dst[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        dst[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    }

    if (dst_len < 4) {
        return 0;
    }
    dst[0] = (char)(0xF0 | ((codepoint >> 18) & 0x07));
    dst[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
    dst[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    dst[3] = (char)(0x80 | (codepoint & 0x3F));
    return 4;
}

/*
 * Check if the weather icon font has a glyph for the given codepoint.
 */
static bool weather_icon_font_has_glyph(const lv_font_t *font, uint32_t codepoint)
{
    if (font == NULL) {
        return false;
    }

    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(font, &dsc, codepoint, 0);
}

/*
 * Get the Unicode codepoint for a weather icon ID.
 */
static uint32_t weather_icon_get_codepoint(const lv_font_t *font, weather_icon_id_t icon_id)
{
    if (font == NULL || icon_id < 0 || icon_id >= WEATHER_ICON_COUNT) {
        return 0;
    }

    if (font == &weather_icons_24) {
        return s_weather_icon_codepoint_lookup_24[icon_id];
    }

    if (font == &weather_icons_48) {
        return s_weather_icon_codepoint_lookup_48[icon_id];
    }

    return s_weather_icon_codepoint_lookup_24[icon_id];
}

/*
 * Convert a weather code to a weather icon ID.
 */
static weather_icon_id_t weather_code_to_icon_id(int code, bool is_day)
{
    switch (code) {
        case 0:
            return is_day ? WEATHER_ICON_SUN : WEATHER_ICON_MOON;
        case 1:
        case 2:
            return is_day ? WEATHER_ICON_SUN_PARTLY_CLOUDY : WEATHER_ICON_MOON_PARTLY_CLOUDY;
        case 3:
            return is_day ? WEATHER_ICON_SUN_CLOUDY : WEATHER_ICON_MOON_CLOUDY;
        case 45:
        case 48:
            return is_day ? WEATHER_ICON_FOG : WEATHER_ICON_NIGHT_FOG;
        case 51:
        case 53:
        case 55:
            return is_day ? WEATHER_ICON_DRIZZLE : WEATHER_ICON_NIGHT_DRIZZLE;
        case 56:
        case 57:
            return is_day ? WEATHER_ICON_SLEET : WEATHER_ICON_NIGHT_SLEET;
        case 61:
        case 63:
        case 65:
            return is_day ? WEATHER_ICON_RAIN : WEATHER_ICON_NIGHT_RAIN;
        case 66:
        case 67:
            return is_day ? WEATHER_ICON_FREEZING_RAIN : WEATHER_ICON_NIGHT_FREEZING_RAIN;
        case 71:
        case 73:
        case 75:
        case 77:
        case 85:
        case 86:
            return is_day ? WEATHER_ICON_SNOW : WEATHER_ICON_NIGHT_SNOW;
        case 80:
        case 81:
        case 82:
            return is_day ? WEATHER_ICON_SHOWERS : WEATHER_ICON_NIGHT_SHOWERS;
        case 95:
        case 96:
        case 99:
            return is_day ? WEATHER_ICON_THUNDERSTORM : WEATHER_ICON_NIGHT_THUNDERSTORM;
        default:
            return is_day ? WEATHER_ICON_SUN : WEATHER_ICON_MOON;
    }
}

/*
 * Convert a weather code to a human-readable interpretation.
 */
static const char *weather_code_to_interpretation(int code)
{
    switch (code) {
        case 0:
            return "Clear sky";
        case 1:
        case 2:
        case 3:
            return "Mainly clear, partly cloudy, and overcast";
        case 45:
        case 48:
            return "Fog and depositing rime fog";
        case 51:
        case 53:
        case 55:
            return "Drizzle: Light, moderate, and dense intensity";
        case 56:
        case 57:
            return "Freezing Drizzle: Light and dense intensity";
        case 61:
        case 63:
        case 65:
            return "Rain: Slight, moderate and heavy intensity";
        case 66:
        case 67:
            return "Freezing Rain: Light and heavy intensity";
        case 71:
        case 73:
        case 75:
            return "Snow fall: Slight, moderate, and heavy intensity";
        case 77:
            return "Snow grains";
        case 80:
        case 81:
        case 82:
            return "Rain showers: Slight, moderate, and violent";
        case 85:
        case 86:
            return "Snow showers slight and heavy";
        case 95:
            return "Thunderstorm: Slight or moderate";
        case 96:
        case 99:
            return "Thunderstorm with slight and heavy hail";
        default:
            return "Unknown weather code";
    }
}

/*
 * Set a label to display a weather icon based on the icon ID.
 */
static bool set_label_to_icon_id(lv_obj_t *label, const lv_font_t *font, weather_icon_id_t icon_id)
{
    if (label == NULL || font == NULL) {
        return false;
    }

    uint32_t codepoint = weather_icon_get_codepoint(font, icon_id);
    if (codepoint == 0 || !weather_icon_font_has_glyph(font, codepoint)) {
        return false;
    }

    char utf8[5] = {0};
    size_t written = utf8_encode_codepoint(utf8, sizeof(utf8) - 1, codepoint);
    if (written == 0) {
        return false;
    }

    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_text(label, utf8);
    return true;
}

/*
 * Create a row in the weather metrics section with an icon label and a value label.
 */
static lv_obj_t *create_weather_metric_row(lv_obj_t *parent, lv_obj_t **icon_label_out, lv_obj_t **value_label_out)
{
    if (parent == NULL || icon_label_out == NULL || value_label_out == NULL) {
        return NULL;
    }

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    *icon_label_out = lv_label_create(row);
    lv_obj_set_style_text_color(*icon_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*icon_label_out, &weather_icons_24, 0);

    *value_label_out = lv_label_create(row);
    lv_obj_set_style_text_color(*value_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*value_label_out, font_normal, 0);

    return row;
}

/*
 * Create one row containing two weather metrics (left and right).
 */
static lv_obj_t *create_weather_dual_metric_row(lv_obj_t *parent,
                                                lv_obj_t **left_icon_label_out,
                                                lv_obj_t **left_value_label_out,
                                                lv_obj_t **right_icon_label_out,
                                                lv_obj_t **right_value_label_out)
{
    if (parent == NULL ||
        left_icon_label_out == NULL || left_value_label_out == NULL ||
        right_icon_label_out == NULL || right_value_label_out == NULL) {
        return NULL;
    }

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_width(left, LV_PCT(50));
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_style_pad_column(left, 6, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    *left_icon_label_out = lv_label_create(left);
    lv_obj_set_style_text_color(*left_icon_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*left_icon_label_out, &weather_icons_24, 0);

    *left_value_label_out = lv_label_create(left);
    lv_obj_set_style_text_color(*left_value_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*left_value_label_out, font_normal, 0);

    lv_obj_t *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_width(right, LV_PCT(50));
    lv_obj_set_height(right, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_style_pad_column(right, 6, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    *right_icon_label_out = lv_label_create(right);
    lv_obj_set_style_text_color(*right_icon_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*right_icon_label_out, &weather_icons_24, 0);

    *right_value_label_out = lv_label_create(right);
    lv_obj_set_style_text_color(*right_value_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*right_value_label_out, font_normal, 0);

    return row;
}

/*
 * Create a row in the weather header section with an icon label and a temperature label.
 */
static lv_obj_t *create_weather_header_row(lv_obj_t *parent, lv_obj_t **icon_label_out, lv_obj_t **temp_label_out)
{
    if (parent == NULL || icon_label_out == NULL || temp_label_out == NULL) {
        return NULL;
    }

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    *icon_label_out = lv_label_create(row);
    lv_obj_set_style_text_color(*icon_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*icon_label_out, &weather_icons_48, 0);

    *temp_label_out = lv_label_create(row);
    lv_obj_set_style_text_color(*temp_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*temp_label_out, &lv_font_montserrat_36, 0);

    return row;
}

/*
 * Create a row in the weather right icon section with an icon label and a value label.
 */
static lv_obj_t *create_weather_right_icon_row(lv_obj_t *parent, lv_obj_t **icon_label_out, lv_obj_t **value_label_out)
{
    if (parent == NULL || icon_label_out == NULL || value_label_out == NULL) {
        return NULL;
    }

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    *icon_label_out = lv_label_create(row);
    lv_obj_set_style_text_color(*icon_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*icon_label_out, &weather_icons_24, 0);

    *value_label_out = lv_label_create(row);
    lv_obj_set_style_text_color(*value_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*value_label_out, font_normal, 0);
    lv_obj_set_style_text_align(*value_label_out, LV_TEXT_ALIGN_RIGHT, 0);

    return row;
}

/*
 * Create a row in the weather forecast section with date, icon, high/low temperature, and precipitation probability labels.
 */
static lv_obj_t *create_weather_forecast_row(lv_obj_t *parent,
                                             lv_obj_t **date_label_out,
                                             lv_obj_t **icon_label_out,
                                             lv_obj_t **hilo_label_out,
                                             lv_obj_t **pop_icon_label_out,
                                             lv_obj_t **pop_label_out)
{
    if (parent == NULL || date_label_out == NULL || icon_label_out == NULL || hilo_label_out == NULL ||
        pop_icon_label_out == NULL || pop_label_out == NULL) {
        return NULL;
    }

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_bottom(row, 1, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_width(left, LV_PCT(56));
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_style_pad_column(left, 8, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    *icon_label_out = lv_label_create(left);
    lv_obj_set_style_text_color(*icon_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*icon_label_out, &weather_icons_48, 0);

    *date_label_out = lv_label_create(left);
    lv_obj_set_style_text_color(*date_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*date_label_out, font_normal, 0);
    lv_obj_set_style_pad_top(*date_label_out, 10, 0);
    lv_obj_set_width(*date_label_out, LV_PCT(100));
    lv_label_set_long_mode(*date_label_out, LV_LABEL_LONG_WRAP);

    lv_obj_t *right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_width(right, LV_PCT(44));
    lv_obj_set_height(right, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_style_pad_top(right, 10, 0);
    lv_obj_set_style_pad_right(right, 20, 0);
    lv_obj_set_style_pad_row(right, 2, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

    *hilo_label_out = lv_label_create(right);
    lv_obj_set_style_text_color(*hilo_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*hilo_label_out, font_normal, 0);
    lv_obj_set_style_text_align(*hilo_label_out, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(*hilo_label_out, LV_PCT(100));

    lv_obj_t *pop_row = lv_obj_create(right);
    lv_obj_remove_style_all(pop_row);
    lv_obj_set_width(pop_row, LV_PCT(100));
    lv_obj_set_height(pop_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(pop_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(pop_row, 0, 0);
    lv_obj_set_style_pad_column(pop_row, 6, 0);
    lv_obj_set_flex_flow(pop_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pop_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    *pop_icon_label_out = lv_label_create(pop_row);
    lv_obj_set_style_text_color(*pop_icon_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*pop_icon_label_out, &weather_icons_24, 0);

    *pop_label_out = lv_label_create(pop_row);
    lv_obj_set_style_text_color(*pop_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*pop_label_out, font_normal, 0);
    lv_obj_set_style_text_align(*pop_label_out, LV_TEXT_ALIGN_RIGHT, 0);

    return row;
}

/*
 * Create a cell in the hourly forecast section with hour, icon, high/low temperature, and precipitation probability labels.
 */
static lv_obj_t *create_hourly_forecast_cell(lv_obj_t *parent,
                                             lv_obj_t **hour_label_out,
                                             lv_obj_t **icon_label_out,
                                             lv_obj_t **hilo_label_out,
                                             lv_obj_t **pop_icon_label_out,
                                             lv_obj_t **pop_label_out)
{
    if (parent == NULL || hour_label_out == NULL || icon_label_out == NULL || hilo_label_out == NULL ||
        pop_icon_label_out == NULL || pop_label_out == NULL) {
        return NULL;
    }

    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_width(cell, LV_PCT(100));
    lv_obj_set_height(cell, LV_PCT(100));
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_set_style_pad_top(cell, 1, 0);
    lv_obj_set_style_pad_row(cell, 1, 0);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    *hour_label_out = lv_label_create(cell);
    lv_obj_set_style_text_color(*hour_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*hour_label_out, font_normal, 0);
    lv_obj_set_style_text_align(*hour_label_out, LV_TEXT_ALIGN_CENTER, 0);

    *icon_label_out = lv_label_create(cell);
    lv_obj_set_style_text_color(*icon_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*icon_label_out, &weather_icons_48, 0);
    lv_obj_set_style_text_align(*icon_label_out, LV_TEXT_ALIGN_CENTER, 0);

    *hilo_label_out = lv_label_create(cell);
    lv_obj_set_style_text_color(*hilo_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*hilo_label_out, font_normal, 0);
    lv_obj_set_style_text_align(*hilo_label_out, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *pop_row = lv_obj_create(cell);
    lv_obj_remove_style_all(pop_row);
    lv_obj_set_width(pop_row, LV_SIZE_CONTENT);
    lv_obj_set_height(pop_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(pop_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(pop_row, 0, 0);
    lv_obj_set_style_pad_column(pop_row, 3, 0);
    lv_obj_set_flex_flow(pop_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pop_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    *pop_icon_label_out = lv_label_create(pop_row);
    lv_obj_set_style_text_color(*pop_icon_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*pop_icon_label_out, &weather_icons_24, 0);

    *pop_label_out = lv_label_create(pop_row);
    lv_obj_set_style_text_color(*pop_label_out, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_font(*pop_label_out, font_normal, 0);

    return cell;
}

/*
 * Show a weather panel.
 */
static void ui_panel_show(lcd_ui_panel_t *panel)
{
    if (panel == NULL) {
        return;
    }

    panel->visible = true;
    if (panel->root != NULL) {
        lv_obj_clear_flag(panel->root, LV_OBJ_FLAG_HIDDEN);
    }
}

/*
 * Hide a weather panel.
 */
static void ui_panel_hide(lcd_ui_panel_t *panel)
{
    if (panel == NULL) {
        return;
    }

    panel->visible = false;
    if (panel->root != NULL) {
        lv_obj_add_flag(panel->root, LV_OBJ_FLAG_HIDDEN);
    }
}

/*
 * Check if a point is within a rectangle.
 */
static bool point_in_rect(int x, int y, int x1, int y1, int x2, int y2)
{
    return x >= x1 && x <= x2 && y >= y1 && y <= y2;
}

/*
 * Format a local date string for "today + day_offset".
 */
static bool format_local_date_with_day_offset(size_t day_offset, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return false;
    }

    time_t now = time(NULL);
    struct tm local_tm;
    if (localtime_r(&now, &local_tm) == NULL) {
        return false;
    }

    // Use noon to avoid day-roll issues near DST boundaries.
    local_tm.tm_hour = 12;
    local_tm.tm_min = 0;
    local_tm.tm_sec = 0;
    local_tm.tm_mday += (int)day_offset;

    time_t target = mktime(&local_tm);
    if (target == (time_t)-1) {
        return false;
    }

    struct tm target_tm;
    if (localtime_r(&target, &target_tm) == NULL) {
        return false;
    }

    return strftime(out, out_len, "%a %m/%d", &target_tm) > 0;
}

/*
 * Find the first hourly entry at or after the current weather timestamp.
 */
static size_t get_hourly_display_start_index(const weather_forecast_t *forecast)
{
    if (forecast == NULL || forecast->hourly_count == 0) {
        return 0;
    }

    int64_t pivot = (forecast->current.time > 0) ? forecast->current.time : (int64_t)time(NULL);
    for (size_t i = 0; i < forecast->hourly_count; i++) {
        if (forecast->hourly[i].time >= pivot) {
            return i;
        }
    }

    return 0;
}

/*
 * Set the weather daily view.
 */
static void set_weather_daily_view(weather_daily_view_t view)
{
    s_weather_daily_view = view;

    if (s_forecast_toggle_button_label != NULL) {
        weather_icon_id_t arrow_id = (view == WEATHER_DAILY_VIEW_TODAY)
                                  ? WEATHER_ICON_ARROW_DOWN
                                  : WEATHER_ICON_ARROW_UP;
        if (!set_label_to_icon_id(s_forecast_toggle_button_label, &weather_icons_24, arrow_id)) {
            lv_obj_set_style_text_font(s_forecast_toggle_button_label, font_normal, 0);
            lv_label_set_text(s_forecast_toggle_button_label, (view == WEATHER_DAILY_VIEW_TODAY) ? "d" : "u");
        }
    }
}

/*
 * Set the hourly forecast page.
 */
static void set_hourly_forecast_page(int page)
{
    int wrapped = page % HOURLY_FORECAST_PAGE_COUNT;
    if (wrapped < 0) {
        wrapped += HOURLY_FORECAST_PAGE_COUNT;
    }
    s_hourly_forecast_page = wrapped;

    if (s_hourly_page_label != NULL) {
        lv_label_set_text_fmt(s_hourly_page_label,
                              "%d/%d",
                              s_hourly_forecast_page + 1,
                              HOURLY_FORECAST_PAGE_COUNT);
    }
}

/*
 * Handle touch events for the hourly forecast section.
 */
static bool handle_hourly_forecast_touch(const touch_event_t *event)
{
    if (event == NULL || !s_weather_panel.visible) {
        return false;
    }

    if (event->touch_type == SWIPE_LEFT) {
        set_hourly_forecast_page(s_hourly_forecast_page + 1);
        return true;
    }
    if (event->touch_type == SWIPE_RIGHT) {
        set_hourly_forecast_page(s_hourly_forecast_page - 1);
        return true;
    }

    if (event->touch_type != TAP) {
        return false;
    }

    if (s_hourly_left_button_label == NULL || s_hourly_right_button_label == NULL) {
        return false;
    }

    lv_area_t left_area;
    lv_area_t right_area;
    lv_obj_get_coords(s_hourly_left_button_label, &left_area);
    lv_obj_get_coords(s_hourly_right_button_label, &right_area);

    bool on_left = point_in_rect(event->x,
                                 event->y,
                                 left_area.x1 - 12,
                                 left_area.y1 - 8,
                                 left_area.x2 + 12,
                                 left_area.y2 + 8);
    bool on_right = point_in_rect(event->x,
                                  event->y,
                                  right_area.x1 - 12,
                                  right_area.y1 - 8,
                                  right_area.x2 + 12,
                                  right_area.y2 + 8);

    if (!on_left && !on_right) {
        return false;
    }

    if (on_left) {
        set_hourly_forecast_page(s_hourly_forecast_page - 1);
    } else {
        set_hourly_forecast_page(s_hourly_forecast_page + 1);
    }
    return true;
}

/*
 * Handle touch events for the daily weather view section.
 */
static bool handle_daily_view_touch(const touch_event_t *event)
{
    if (event == NULL) {
        return false;
    }

    if (!s_weather_forecast_panel.visible || !s_weather_forecast_toggle_panel.visible) {
        return false;
    }

    bool on_forecast_panel = point_in_rect(event->x, event->y,
                                        PANEL_X1(s_weather_forecast_panel), PANEL_Y1(s_weather_forecast_panel),
                                        PANEL_X2(s_weather_forecast_panel), PANEL_Y2(s_weather_forecast_panel));
    bool on_toggle_panel = point_in_rect(event->x, event->y,
                                         PANEL_X1(s_weather_forecast_toggle_panel), PANEL_Y1(s_weather_forecast_toggle_panel),
                                         PANEL_X2(s_weather_forecast_toggle_panel), PANEL_Y2(s_weather_forecast_toggle_panel));

    if (s_weather_daily_view == WEATHER_DAILY_VIEW_TODAY) {
        if ((event->touch_type == TAP && on_toggle_panel) ||
            (event->touch_type == SWIPE_DOWN && on_forecast_panel)) {
            set_weather_daily_view(WEATHER_DAILY_VIEW_NEXT3);
            return true;
        }
    } else {
        if ((event->touch_type == TAP && on_toggle_panel) ||
            (event->touch_type == SWIPE_UP && on_forecast_panel)) {
            set_weather_daily_view(WEATHER_DAILY_VIEW_TODAY);
            return true;
        }
    }

    return false;
}

/*
 * Update the time label.
 */
static void update_time_label(void)
{
    if (s_date_label == NULL || s_time_label == NULL) {
        return;
    }

    time_t now = time(NULL);
    struct tm local_tm;
    localtime_r(&now, &local_tm);

    char date_buf[24];
    char time_buf[16];
    strftime(date_buf, sizeof(date_buf), "%a %b %d", &local_tm);
    strftime(time_buf, sizeof(time_buf), "%H:%M:%S", &local_tm);
    lv_label_set_text(s_date_label, date_buf);
    lv_label_set_text(s_time_label, time_buf);
}

/*
 * Update the last refreshed label.
 */
static void update_last_refreshed_label(void)
{
    if (s_last_refreshed_label == NULL) {
        return;
    }

    if (s_weather_refresh_in_progress) {
        lv_label_set_text(s_last_refreshed_label, "refreshing...");
        return;
    }

    if (!s_last_weather_refresh_valid) {
        lv_label_set_text(s_last_refreshed_label, "Last Refreshed: --:--");
        return;
    }

    struct tm local_tm;
    char refresh_time_buf[8];
    if (localtime_r(&s_last_weather_refresh_time, &local_tm) == NULL) {
        lv_label_set_text(s_last_refreshed_label, "Last Refreshed: --:--");
        return;
    }

    strftime(refresh_time_buf, sizeof(refresh_time_buf), "%H:%M", &local_tm);
    lv_label_set_text_fmt(s_last_refreshed_label, "Last Refreshed: %s", refresh_time_buf);
}

/*
 * Time band timer callback.
 */
static void time_band_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_time_label();
}

/*
 * Handle touch events for the time band refresh icon.
 */
static bool handle_time_band_refresh_touch(const touch_event_t *event)
{
    if (event == NULL || event->touch_type != TAP || !s_time_band_panel.visible) {
        return false;
    }

    bool on_refresh = false;
    if (s_refresh_icon_label != NULL) {
        lv_area_t icon_area;
        lv_obj_get_coords(s_refresh_icon_label, &icon_area);
        on_refresh = point_in_rect(event->x,
                                   event->y,
                                   icon_area.x1 - 14,
                                   icon_area.y1 - 10,
                                   icon_area.x2 + 14,
                                   icon_area.y2 + 10);
    }

    if (!on_refresh) {
        int refresh_x1 = PANEL_X2(s_time_band_panel) - TIME_BAND_REFRESH_TOUCH_W + 1;
        int refresh_x2 = PANEL_X2(s_time_band_panel);
        on_refresh = point_in_rect(event->x,
                                   event->y,
                                   refresh_x1,
                                   PANEL_Y1(s_time_band_panel),
                                   refresh_x2,
                                   PANEL_Y2(s_time_band_panel));
    }

    if (!on_refresh) {
        return false;
    }

    esp_err_t refresh_ret = wifi_call_request_refresh();
    if (refresh_ret != ESP_OK) {
        ESP_LOGW(TAG, "Weather refresh request failed: %s", esp_err_to_name(refresh_ret));
    } else {
        s_weather_refresh_in_progress = true;
        update_last_refreshed_label();
        ESP_LOGI(TAG, "Weather refresh requested from UI");
    }

    return true;
}

/*
 * Render the time band UI.
 */
static void render_time_band_ui(lv_display_t *disp)
{
    lv_obj_t *screen = lv_display_get_screen_active(disp);

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);

    ui_panel_hide(&s_starting_panel);

    if (s_time_band_panel.root == NULL) {
        s_time_band_panel.root = lv_obj_create(screen);
        lv_obj_remove_style_all(s_time_band_panel.root);
        s_time_band_panel.x = 0;
        s_time_band_panel.y = 0;
        s_time_band_panel.w = DISPLAY_LCD_H_RES;
        s_time_band_panel.h = 44;
        lv_obj_set_pos(s_time_band_panel.root, s_time_band_panel.x, s_time_band_panel.y);
        lv_obj_set_size(s_time_band_panel.root, s_time_band_panel.w, s_time_band_panel.h);
        lv_obj_set_style_bg_color(s_time_band_panel.root, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_time_band_panel.root, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(s_time_band_panel.root, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(s_time_band_panel.root, 2, 0);
        lv_obj_set_style_border_color(s_time_band_panel.root, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        lv_obj_set_style_pad_hor(s_time_band_panel.root, 10, 0);
        lv_obj_set_style_pad_ver(s_time_band_panel.root, 6, 0);

        s_date_label = lv_label_create(s_time_band_panel.root);
        lv_obj_set_style_text_color(s_date_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        lv_obj_set_style_text_font(s_date_label, font_normal, 0);
        lv_obj_align(s_date_label, LV_ALIGN_LEFT_MID, 0, 0);

        s_last_refreshed_label = lv_label_create(s_time_band_panel.root);
        lv_obj_set_style_text_color(s_last_refreshed_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        lv_obj_set_style_text_font(s_last_refreshed_label, font_normal, 0);
        lv_obj_align(s_last_refreshed_label, LV_ALIGN_CENTER, 0, 0);

        s_time_label = lv_label_create(s_time_band_panel.root);
        lv_obj_set_style_text_color(s_time_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        lv_obj_set_style_text_font(s_time_label, font_normal, 0);
        lv_obj_align(s_time_label, LV_ALIGN_RIGHT_MID, -34, 0);

        s_refresh_icon_label = lv_label_create(s_time_band_panel.root);
        lv_obj_set_style_text_color(s_refresh_icon_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        if (!set_label_to_icon_id(s_refresh_icon_label, &weather_icons_24, WEATHER_ICON_REFRESH)) {
            lv_obj_set_style_text_font(s_refresh_icon_label, font_normal, 0);
            lv_label_set_text(s_refresh_icon_label, "R");
        }
        lv_obj_align(s_refresh_icon_label, LV_ALIGN_RIGHT_MID, -6, 0);
    }

    ui_panel_show(&s_time_band_panel);

    update_time_label();
    update_last_refreshed_label();

    if (s_time_timer == NULL) {
        s_time_timer = lv_timer_create(time_band_timer_cb, 1000, NULL);
    }
}

/*
 * Render the time band UI callback.
 */
static void render_time_band_ui_cb(lv_display_t *disp, void *user_ctx)
{
    (void)user_ctx;
    render_time_band_ui(disp);
}

/*
 * Render the weather current UI.
 */
static void render_weather_current_ui(lv_display_t *disp)
{
    weather_forecast_t forecast = {0};
    if (get_weather_forecast(&forecast) != ESP_OK) {
        ESP_LOGW(TAG, "Weather forecast not ready yet");
        return;
    }

    lv_obj_t *screen = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);

    ui_panel_hide(&s_starting_panel);

    if (s_weather_panel.root == NULL) {
        s_weather_panel.root = lv_obj_create(screen);
        lv_obj_remove_style_all(s_weather_panel.root);
        s_weather_panel.x = 0;
        s_weather_panel.y = WEATHER_PANEL_TOP_OFFSET;
        s_weather_panel.w = WEATHER_LEFT_PANEL_W;
        s_weather_panel.h = WEATHER_PANEL_HALF_H;
        lv_obj_set_pos(s_weather_panel.root, s_weather_panel.x, s_weather_panel.y);
        lv_obj_set_size(s_weather_panel.root, s_weather_panel.w, s_weather_panel.h);
        lv_obj_set_style_bg_color(s_weather_panel.root, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_weather_panel.root, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_weather_panel.root, 2, 0);
        lv_obj_set_style_border_color(s_weather_panel.root, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        lv_obj_set_style_border_side(s_weather_panel.root,
                         LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM,
                         0);
        lv_obj_set_style_pad_left(s_weather_panel.root, 10, 0);
        lv_obj_set_style_pad_right(s_weather_panel.root, 10, 0);
        lv_obj_set_style_pad_top(s_weather_panel.root, 6, 0);
        lv_obj_set_style_pad_bottom(s_weather_panel.root, 6, 0);
        lv_obj_set_style_pad_column(s_weather_panel.root, 12, 0);
        lv_obj_set_flex_flow(s_weather_panel.root, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_weather_panel.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        s_weather_left_col = lv_obj_create(s_weather_panel.root);
        lv_obj_remove_style_all(s_weather_left_col);
        lv_obj_set_width(s_weather_left_col, LV_PCT(62));
        lv_obj_set_height(s_weather_left_col, LV_PCT(100));
        lv_obj_set_style_bg_opa(s_weather_left_col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(s_weather_left_col, 0, 0);
        lv_obj_set_style_pad_row(s_weather_left_col, 2, 0);
        lv_obj_set_flex_flow(s_weather_left_col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_weather_left_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        s_weather_right_col = lv_obj_create(s_weather_panel.root);
        lv_obj_remove_style_all(s_weather_right_col);
        lv_obj_set_width(s_weather_right_col, LV_PCT(38));
        lv_obj_set_height(s_weather_right_col, LV_PCT(100));
        lv_obj_set_style_bg_opa(s_weather_right_col, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_left(s_weather_right_col, 4, 0);
        lv_obj_set_style_pad_right(s_weather_right_col, 8, 0);
        lv_obj_set_style_pad_top(s_weather_right_col, 2, 0);
        lv_obj_set_style_pad_row(s_weather_right_col, 6, 0);
        lv_obj_set_flex_flow(s_weather_right_col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_weather_right_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);

        s_weather_header_row = create_weather_header_row(s_weather_left_col, &s_weather_icon_label, &s_weather_temp_label);
        (void)s_weather_header_row;

        s_weather_desc_label = lv_label_create(s_weather_left_col);
        lv_obj_set_style_text_color(s_weather_desc_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        lv_obj_set_style_text_font(s_weather_desc_label, font_normal, 0);
        lv_obj_set_width(s_weather_desc_label, LV_PCT(100));
        lv_label_set_long_mode(s_weather_desc_label, LV_LABEL_LONG_WRAP);

        create_weather_metric_row(s_weather_left_col, &s_weather_humidity_icon_label, &s_weather_humidity_label);
        create_weather_dual_metric_row(s_weather_left_col,
                           &s_weather_wind_icon_label,
                           &s_weather_wind_label,
                           &s_weather_cloud_icon_label,
                           &s_weather_cloud_label);

        s_hourly_panel = lv_obj_create(screen);
        s_weather_hourly_panel.root = s_hourly_panel;
        lv_obj_remove_style_all(s_hourly_panel);
        s_weather_hourly_panel.x = 0;
        s_weather_hourly_panel.y = WEATHER_PANEL_TOP_OFFSET + WEATHER_PANEL_HALF_H;
        s_weather_hourly_panel.w = DISPLAY_LCD_H_RES;
        s_weather_hourly_panel.h = WEATHER_PANEL_HALF_H;
        lv_obj_set_pos(s_hourly_panel, s_weather_hourly_panel.x, s_weather_hourly_panel.y);
        lv_obj_set_size(s_hourly_panel, s_weather_hourly_panel.w, s_weather_hourly_panel.h);
        lv_obj_set_style_bg_color(s_hourly_panel, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_hourly_panel, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(s_hourly_panel,
                                     LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM,
                         0);
        lv_obj_set_style_border_width(s_hourly_panel, 2, 0);
        lv_obj_set_style_border_color(s_hourly_panel, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        lv_obj_set_style_pad_top(s_hourly_panel, 4, 0);
        lv_obj_set_style_pad_bottom(s_hourly_panel, 4, 0);
        lv_obj_set_style_pad_left(s_hourly_panel, 8, 0);
        lv_obj_set_style_pad_right(s_hourly_panel, 8, 0);
        lv_obj_set_style_pad_row(s_hourly_panel, 2, 0);
        lv_obj_set_flex_flow(s_hourly_panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_hourly_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        s_hourly_nav_row = lv_obj_create(s_hourly_panel);
        lv_obj_remove_style_all(s_hourly_nav_row);
        lv_obj_set_width(s_hourly_nav_row, LV_PCT(100));
        lv_obj_set_height(s_hourly_nav_row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(s_hourly_nav_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(s_hourly_nav_row, 0, 0);
        lv_obj_set_style_pad_bottom(s_hourly_nav_row, 2, 0);
        lv_obj_set_flex_flow(s_hourly_nav_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_hourly_nav_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        s_hourly_left_button_label = lv_label_create(s_hourly_nav_row);
        lv_obj_set_style_text_color(s_hourly_left_button_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        if (!set_label_to_icon_id(s_hourly_left_button_label, &weather_icons_24, WEATHER_ICON_ARROW_LEFT)) {
            lv_obj_set_style_text_font(s_hourly_left_button_label, font_normal, 0);
            lv_label_set_text(s_hourly_left_button_label, "<");
        }

        s_hourly_page_label = lv_label_create(s_hourly_nav_row);
        lv_obj_set_style_text_color(s_hourly_page_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        lv_obj_set_style_text_font(s_hourly_page_label, font_normal, 0);
        lv_label_set_text(s_hourly_page_label, "1/3");

        s_hourly_right_button_label = lv_label_create(s_hourly_nav_row);
        lv_obj_set_style_text_color(s_hourly_right_button_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        if (!set_label_to_icon_id(s_hourly_right_button_label, &weather_icons_24, WEATHER_ICON_ARROW_RIGHT)) {
            lv_obj_set_style_text_font(s_hourly_right_button_label, font_normal, 0);
            lv_label_set_text(s_hourly_right_button_label, ">");
        }

        s_hourly_strip = lv_obj_create(s_hourly_panel);
        lv_obj_remove_style_all(s_hourly_strip);
        lv_obj_set_width(s_hourly_strip, LV_PCT(100));
        lv_obj_set_height(s_hourly_strip, LV_PCT(100));
        lv_obj_set_flex_grow(s_hourly_strip, 1);
        lv_obj_set_style_bg_opa(s_hourly_strip, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(s_hourly_strip, 0, 0);
        lv_obj_set_style_pad_column(s_hourly_strip, 0, 0);
        lv_obj_set_flex_flow(s_hourly_strip, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(s_hourly_strip, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        for (int i = 0; i < HOURLY_FORECAST_PER_PAGE; i++) {
            lv_obj_t *slot = lv_obj_create(s_hourly_strip);
            lv_obj_remove_style_all(slot);
            lv_obj_set_width(slot, 0);
            lv_obj_set_height(slot, LV_PCT(100));
            lv_obj_set_flex_grow(slot, 1);
            lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_all(slot, 0, 0);
            if (i == 0) {
                lv_obj_set_style_border_side(slot, LV_BORDER_SIDE_NONE, 0);
            } else {
                lv_obj_set_style_border_side(slot, LV_BORDER_SIDE_LEFT, 0);
                lv_obj_set_style_border_width(slot, 1, 0);
                lv_obj_set_style_border_color(slot, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
            }

            create_hourly_forecast_cell(slot,
                                        &s_hourly_hour_labels[i],
                                        &s_hourly_icon_labels[i],
                                        &s_hourly_hilo_labels[i],
                                        &s_hourly_pop_icon_labels[i],
                                        &s_hourly_pop_labels[i]);
        }

        s_weather_hilo_label = lv_label_create(s_weather_right_col);
        lv_obj_set_style_text_color(s_weather_hilo_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        lv_obj_set_style_text_font(s_weather_hilo_label, font_normal, 0);
        lv_obj_set_style_text_align(s_weather_hilo_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(s_weather_hilo_label, LV_PCT(100));
        lv_label_set_long_mode(s_weather_hilo_label, LV_LABEL_LONG_WRAP);

        create_weather_right_icon_row(s_weather_right_col, &s_weather_today_pop_icon_label, &s_weather_today_pop_label);
        create_weather_right_icon_row(s_weather_right_col, &s_weather_sunrise_icon_label, &s_weather_sunrise_label);
        create_weather_right_icon_row(s_weather_right_col, &s_weather_sunset_icon_label, &s_weather_sunset_label);
    }

    if (s_weather_forecast_panel.root == NULL) {
        s_weather_forecast_panel.root = lv_obj_create(screen);
        lv_obj_remove_style_all(s_weather_forecast_panel.root);
        s_weather_forecast_panel.x = WEATHER_LEFT_PANEL_W;
        s_weather_forecast_panel.y = WEATHER_PANEL_TOP_OFFSET;
        s_weather_forecast_panel.w = WEATHER_FORECAST_PANEL_W;
        s_weather_forecast_panel.h = WEATHER_PANEL_HALF_H;
        lv_obj_set_pos(s_weather_forecast_panel.root, s_weather_forecast_panel.x, s_weather_forecast_panel.y);
        lv_obj_set_size(s_weather_forecast_panel.root, s_weather_forecast_panel.w, s_weather_forecast_panel.h);
        lv_obj_set_style_bg_color(s_weather_forecast_panel.root, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_weather_forecast_panel.root, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_weather_forecast_panel.root, 2, 0);
        lv_obj_set_style_border_color(s_weather_forecast_panel.root, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        lv_obj_set_style_border_side(s_weather_forecast_panel.root,
                                     LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM,
                                     0);
        lv_obj_set_style_pad_left(s_weather_forecast_panel.root, 8, 0);
        lv_obj_set_style_pad_right(s_weather_forecast_panel.root, 8, 0);
        lv_obj_set_style_pad_top(s_weather_forecast_panel.root, 2, 0);
        lv_obj_set_style_pad_bottom(s_weather_forecast_panel.root, 6, 0);
        lv_obj_set_style_pad_row(s_weather_forecast_panel.root, 1, 0);
        lv_obj_set_flex_flow(s_weather_forecast_panel.root, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(s_weather_forecast_panel.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

        for (int i = 0; i < FORECAST_DAYS_VISIBLE; i++) {
            create_weather_forecast_row(s_weather_forecast_panel.root,
                                        &s_forecast_date_labels[i],
                                        &s_forecast_icon_labels[i],
                                        &s_forecast_hilo_labels[i],
                                        &s_forecast_pop_icon_labels[i],
                                        &s_forecast_pop_labels[i]);
        }
    }

    if (s_weather_forecast_toggle_panel.root == NULL) {
        s_weather_forecast_toggle_panel.root = lv_obj_create(screen);
        lv_obj_remove_style_all(s_weather_forecast_toggle_panel.root);
        s_weather_forecast_toggle_panel.x = WEATHER_LEFT_PANEL_W + WEATHER_FORECAST_PANEL_W;
        s_weather_forecast_toggle_panel.y = WEATHER_PANEL_TOP_OFFSET;
        s_weather_forecast_toggle_panel.w = WEATHER_TOGGLE_PANEL_ACTUAL_W;
        s_weather_forecast_toggle_panel.h = WEATHER_PANEL_HALF_H;
        lv_obj_set_pos(s_weather_forecast_toggle_panel.root,
                       s_weather_forecast_toggle_panel.x,
                       s_weather_forecast_toggle_panel.y);
        lv_obj_set_size(s_weather_forecast_toggle_panel.root,
                        s_weather_forecast_toggle_panel.w,
                        s_weather_forecast_toggle_panel.h);
        lv_obj_set_style_bg_color(s_weather_forecast_toggle_panel.root, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_weather_forecast_toggle_panel.root, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_weather_forecast_toggle_panel.root, 2, 0);
        lv_obj_set_style_border_color(s_weather_forecast_toggle_panel.root, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        lv_obj_set_style_border_side(s_weather_forecast_toggle_panel.root,
                                     LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM,
                                     0);
        lv_obj_set_style_pad_all(s_weather_forecast_toggle_panel.root, 0, 0);

        s_forecast_toggle_button_label = lv_label_create(s_weather_forecast_toggle_panel.root);
        lv_obj_set_style_text_color(s_forecast_toggle_button_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        lv_obj_set_style_text_font(s_forecast_toggle_button_label, font_normal, 0);
        lv_obj_center(s_forecast_toggle_button_label);
    }

    set_hourly_forecast_page(s_hourly_forecast_page);
    ui_panel_show(&s_weather_panel);
    ui_panel_show(&s_weather_hourly_panel);
    ui_panel_show(&s_weather_forecast_panel);
    ui_panel_show(&s_weather_forecast_toggle_panel);
    set_weather_daily_view(s_weather_daily_view);

    char value_buf[48];

    const weather_current_t *current = &forecast.current;
    const lv_font_t *icon_font = &weather_icons_48;
    float current_temp_f = current->temperature_2m;
    weather_icon_id_t icon_id = weather_code_to_icon_id(current->weather_code, current->is_day != 0);
    set_icon_temp_color(s_weather_icon_label, current_temp_f);
    if (!set_label_to_icon_id(s_weather_icon_label, icon_font, icon_id)) {
        lv_obj_set_style_text_font(s_weather_icon_label, font_normal, 0);
        lv_label_set_text(s_weather_icon_label, "?");
    }
    lv_label_set_text(s_weather_desc_label, weather_code_to_interpretation(current->weather_code));

    format_value_1dp(value_buf, sizeof(value_buf), "", current->temperature_2m, " F");
    lv_label_set_text(s_weather_temp_label, value_buf);

    lv_obj_set_style_text_color(s_weather_humidity_icon_label, lv_color_hex(UI_COLOR_HUMIDITY_PRECIP_HEX), 0);
    lv_obj_set_style_text_color(s_weather_wind_icon_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_color(s_weather_cloud_icon_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    set_label_to_icon_id(s_weather_humidity_icon_label, &weather_icons_24, WEATHER_ICON_HUMIDITY);
    set_label_to_icon_id(s_weather_wind_icon_label, &weather_icons_24, WEATHER_ICON_WIND_GUST);
    set_label_to_icon_id(s_weather_cloud_icon_label, &weather_icons_24, WEATHER_ICON_SINGLE_CLOUD);

    lv_obj_set_style_text_color(s_weather_humidity_label, lv_color_hex(UI_COLOR_HUMIDITY_PRECIP_HEX), 0);
    lv_label_set_text_fmt(s_weather_humidity_label, "%d%%", current->relative_humidity_2m);
    format_value_1dp(value_buf, sizeof(value_buf), "", current->wind_speed_10m, " mph");
    lv_label_set_text(s_weather_wind_label, value_buf);
    lv_label_set_text_fmt(s_weather_cloud_label, "%d%%", current->cloud_cover);

    if (forecast.daily_count > 0) {
        const weather_daily_point_t *today = &forecast.daily[0];
        int high = (int)(today->temperature_2m_max + (today->temperature_2m_max >= 0.0f ? 0.5f : -0.5f));
        int low = (int)(today->temperature_2m_min + (today->temperature_2m_min >= 0.0f ? 0.5f : -0.5f));
        lv_label_set_text_fmt(s_weather_hilo_label, "H: %d F\n\nL: %d F", high, low);
        lv_obj_set_style_text_color(s_weather_today_pop_label, lv_color_hex(UI_COLOR_HUMIDITY_PRECIP_HEX), 0);
        lv_label_set_text_fmt(s_weather_today_pop_label, "%d%%", today->precipitation_probability_max);

        char sun_time_buf[8] = "--:--";
        struct tm sun_tm;
        time_t sun_time;

        sun_time = (time_t)today->sunrise;
        if (today->sunrise > 0 && localtime_r(&sun_time, &sun_tm) != NULL) {
            strftime(sun_time_buf, sizeof(sun_time_buf), "%H:%M", &sun_tm);
        }
        lv_label_set_text(s_weather_sunrise_label, sun_time_buf);

        snprintf(sun_time_buf, sizeof(sun_time_buf), "--:--");
        sun_time = (time_t)today->sunset;
        if (today->sunset > 0 && localtime_r(&sun_time, &sun_tm) != NULL) {
            strftime(sun_time_buf, sizeof(sun_time_buf), "%H:%M", &sun_tm);
        }
        lv_label_set_text(s_weather_sunset_label, sun_time_buf);
    } else {
        lv_label_set_text(s_weather_hilo_label, "H: -- F\n\nL: -- F");
        lv_obj_set_style_text_color(s_weather_today_pop_label, lv_color_hex(UI_COLOR_HUMIDITY_PRECIP_HEX), 0);
        lv_label_set_text(s_weather_today_pop_label, "--%");
        lv_label_set_text(s_weather_sunrise_label, "--:--");
        lv_label_set_text(s_weather_sunset_label, "--:--");
    }

    lv_obj_set_style_text_color(s_weather_today_pop_icon_label, lv_color_hex(UI_COLOR_HUMIDITY_PRECIP_HEX), 0);
    lv_obj_set_style_text_color(s_weather_sunrise_icon_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    lv_obj_set_style_text_color(s_weather_sunset_icon_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
    set_label_to_icon_id(s_weather_today_pop_icon_label, &weather_icons_24, WEATHER_ICON_RAINDROP);
    set_label_to_icon_id(s_weather_sunrise_icon_label, &weather_icons_24, WEATHER_ICON_SUNRISE);
    set_label_to_icon_id(s_weather_sunset_icon_label, &weather_icons_24, WEATHER_ICON_SUNSET);

    if (forecast.hourly_count > 0) {
        size_t hourly_start_idx = get_hourly_display_start_index(&forecast);
        for (int i = 0; i < HOURLY_FORECAST_PER_PAGE; i++) {
            size_t offset = (size_t)((s_hourly_forecast_page * HOURLY_FORECAST_PER_PAGE) + i);
            size_t idx = (hourly_start_idx + offset) % forecast.hourly_count;
            const weather_hourly_point_t *h = &forecast.hourly[idx];

            time_t hour_time = (time_t)h->time;
            struct tm hour_tm;
            char hour_buf[8] = "--:--";
            if (h->time > 0 && localtime_r(&hour_time, &hour_tm) != NULL) {
                strftime(hour_buf, sizeof(hour_buf), "%H:%M", &hour_tm);
            }
            lv_label_set_text(s_hourly_hour_labels[i], hour_buf);

            weather_icon_id_t hourly_icon = weather_code_to_icon_id(h->weather_code, h->is_day != 0);
            set_icon_temp_color(s_hourly_icon_labels[i], h->temperature_2m);
            if (!set_label_to_icon_id(s_hourly_icon_labels[i], &weather_icons_48, hourly_icon)) {
                lv_obj_set_style_text_font(s_hourly_icon_labels[i], font_normal, 0);
                lv_label_set_text(s_hourly_icon_labels[i], "?");
            }

            int hourly_temp = (int)(h->temperature_2m + (h->temperature_2m >= 0.0f ? 0.5f : -0.5f));
            lv_label_set_text_fmt(s_hourly_hilo_labels[i], "%d F", hourly_temp);

            lv_obj_set_style_text_color(s_hourly_pop_icon_labels[i], lv_color_hex(UI_COLOR_HUMIDITY_PRECIP_HEX), 0);
            set_label_to_icon_id(s_hourly_pop_icon_labels[i], &weather_icons_24, WEATHER_ICON_RAINDROP);
            lv_obj_set_style_text_color(s_hourly_pop_labels[i], lv_color_hex(UI_COLOR_HUMIDITY_PRECIP_HEX), 0);
            lv_label_set_text_fmt(s_hourly_pop_labels[i], "%d%%", h->precipitation_probability);
        }
    } else {
        for (int i = 0; i < HOURLY_FORECAST_PER_PAGE; i++) {
            lv_label_set_text(s_hourly_hour_labels[i], "--:--");
            lv_obj_set_style_text_font(s_hourly_icon_labels[i], font_normal, 0);
            set_icon_temp_color(s_hourly_icon_labels[i], current_temp_f);
            lv_label_set_text(s_hourly_icon_labels[i], "-");
            lv_label_set_text(s_hourly_hilo_labels[i], "-- F");
            lv_obj_set_style_text_color(s_hourly_pop_icon_labels[i], lv_color_hex(UI_COLOR_HUMIDITY_PRECIP_HEX), 0);
            set_label_to_icon_id(s_hourly_pop_icon_labels[i], &weather_icons_24, WEATHER_ICON_RAINDROP);
            lv_obj_set_style_text_color(s_hourly_pop_labels[i], lv_color_hex(UI_COLOR_HUMIDITY_PRECIP_HEX), 0);
            lv_label_set_text(s_hourly_pop_labels[i], "--%");
        }
    }

    size_t forecast_base_idx = (s_weather_daily_view == WEATHER_DAILY_VIEW_TODAY) ? 1 : 4;
    for (int i = 0; i < FORECAST_DAYS_VISIBLE; i++) {
        size_t daily_idx = forecast_base_idx + (size_t)i;
        if (daily_idx < forecast.daily_count) {
            const weather_daily_point_t *d = &forecast.daily[daily_idx];
            char date_buf[24] = "-- --/--";
            (void)format_local_date_with_day_offset(daily_idx, date_buf, sizeof(date_buf));
            lv_label_set_text(s_forecast_date_labels[i], date_buf);

            weather_icon_id_t daily_icon = weather_code_to_icon_id(d->weather_code, true);
            lv_obj_set_style_text_color(s_forecast_icon_labels[i], lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
            if (!set_label_to_icon_id(s_forecast_icon_labels[i], &weather_icons_48, daily_icon)) {
                lv_obj_set_style_text_font(s_forecast_icon_labels[i], font_normal, 0);
                lv_label_set_text(s_forecast_icon_labels[i], "?");
            }

            int f_high = (int)(d->temperature_2m_max + (d->temperature_2m_max >= 0.0f ? 0.5f : -0.5f));
            int f_low = (int)(d->temperature_2m_min + (d->temperature_2m_min >= 0.0f ? 0.5f : -0.5f));
            lv_label_set_text_fmt(s_forecast_hilo_labels[i], "%d / %d F", f_high, f_low);
            lv_obj_set_style_text_color(s_forecast_pop_labels[i], lv_color_hex(UI_COLOR_HUMIDITY_PRECIP_HEX), 0);
            lv_label_set_text_fmt(s_forecast_pop_labels[i], "%d%%", d->precipitation_probability_max);
            lv_obj_set_style_text_color(s_forecast_pop_icon_labels[i], lv_color_hex(UI_COLOR_HUMIDITY_PRECIP_HEX), 0);
            set_label_to_icon_id(s_forecast_pop_icon_labels[i], &weather_icons_24, WEATHER_ICON_RAINDROP);
        } else {
            lv_label_set_text(s_forecast_date_labels[i], "-- --/--");
            lv_obj_set_style_text_font(s_forecast_icon_labels[i], font_normal, 0);
            lv_obj_set_style_text_color(s_forecast_icon_labels[i], lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
            lv_label_set_text(s_forecast_icon_labels[i], "-");
            lv_label_set_text(s_forecast_hilo_labels[i], "-- / -- F");
            lv_obj_set_style_text_color(s_forecast_pop_labels[i], lv_color_hex(UI_COLOR_HUMIDITY_PRECIP_HEX), 0);
            lv_label_set_text(s_forecast_pop_labels[i], "--%");
            lv_obj_set_style_text_color(s_forecast_pop_icon_labels[i], lv_color_hex(UI_COLOR_HUMIDITY_PRECIP_HEX), 0);
            set_label_to_icon_id(s_forecast_pop_icon_labels[i], &weather_icons_24, WEATHER_ICON_RAINDROP);
        }
    }
}

/*
 * Render the weather current UI callback.
 */
static void render_weather_current_ui_cb(lv_display_t *disp, void *user_ctx)
{
    (void)user_ctx;
    render_weather_current_ui(disp);
}

/*
 * Render the starting UI.
 */
static void render_starting_ui(lv_display_t *disp)
{
    lv_obj_t *screen = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    if (s_starting_panel.root == NULL) {
        s_starting_panel.root = lv_obj_create(screen);
        lv_obj_remove_style_all(s_starting_panel.root);
        lv_obj_set_style_bg_color(s_starting_panel.root, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_starting_panel.root, LV_OPA_COVER, 0);

        s_starting_label = lv_label_create(s_starting_panel.root);
        lv_obj_set_style_text_color(s_starting_label, lv_color_hex(UI_COLOR_PRIMARY_HEX), 0);
        lv_obj_set_style_text_font(s_starting_label, font_normal, 0);
    }

    s_starting_panel.x = 0;
    s_starting_panel.y = 0;
    s_starting_panel.w = DISPLAY_LCD_H_RES;
    s_starting_panel.h = DISPLAY_LCD_V_RES;
    lv_obj_set_pos(s_starting_panel.root, s_starting_panel.x, s_starting_panel.y);
    lv_obj_set_size(s_starting_panel.root, s_starting_panel.w, s_starting_panel.h);
    lv_label_set_text(s_starting_label, "starting...");
    lv_obj_center(s_starting_label);

    ui_panel_hide(&s_time_band_panel);
    ui_panel_hide(&s_weather_panel);
    ui_panel_hide(&s_weather_hourly_panel);
    ui_panel_hide(&s_weather_forecast_panel);
    ui_panel_hide(&s_weather_forecast_toggle_panel);
    ui_panel_show(&s_starting_panel);
}

/*
 * Render the starting UI callback.
 */
static void render_starting_ui_cb(lv_display_t *disp, void *user_ctx)
{
    (void)user_ctx;
    render_starting_ui(disp);
}

/*
 * UI weather coordinator task.
 */
static void ui_weather_coordinator_task(void *arg)
{
    (void)arg;

    while (1) {
        esp_err_t wait_ret = wifi_call_wait_weather_ready(portMAX_DELAY);
        if (wait_ret != ESP_OK) {
            ESP_LOGW(TAG, "Weather ready wait failed: %s", esp_err_to_name(wait_ret));
            continue;
        }

        s_last_weather_refresh_time = time(NULL);
        s_last_weather_refresh_valid = true;
        s_weather_refresh_in_progress = false;

        esp_err_t ui_ret = lcd_ui_show_time_band();
        if (ui_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to render top time band: %s", esp_err_to_name(ui_ret));
            continue;
        }

        ui_ret = lcd_ui_show_weather_current();
        if (ui_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to render current weather: %s", esp_err_to_name(ui_ret));
        }
    }
}

#if CONFIG_DISPLAY_USE_DOUBLE_FB
static bool on_frame_buf_complete(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *event_data, void *user_ctx)
{
    (void)panel;
    (void)event_data;
    (void)user_ctx;
    BaseType_t need_yield = pdFALSE;

    if (s_lvgl_task_handle) {
        vTaskNotifyGiveFromISR(s_lvgl_task_handle, &need_yield);
    }
    return need_yield == pdTRUE;
}

static void lvgl_flush_wait_cb(lv_display_t *disp)
{
    if (lv_display_flush_is_last(disp)) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    lv_display_flush_ready(disp);
}
#else
static bool notify_lvgl_flush_ready(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_data_t *event_data, void *user_ctx)
{
    (void)panel;
    (void)event_data;
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}
#endif

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    esp_lcd_panel_handle_t panel_handle = lv_display_get_user_data(disp);
    int offsetx1 = area->x1;
    int offsetx2 = area->x2;
    int offsety1 = area->y1;
    int offsety2 = area->y2;
#if CONFIG_DISPLAY_USE_DOUBLE_FB
    if (!lv_display_flush_is_last(disp)) {
        lv_display_flush_ready(disp);
        return;
    }
    offsetx1 = 0;
    offsety1 = 0;
    offsetx2 = DISPLAY_LCD_H_RES - 1;
    offsety2 = DISPLAY_LCD_V_RES - 1;
    ulTaskNotifyTake(pdTRUE, 0);
#endif
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, px_map);
}

static void increase_lvgl_tick(void *arg)
{
    (void)arg;
    lv_tick_inc(DISPLAY_LVGL_TICK_PERIOD_MS);
}

/*
 * LVGL task to handle LVGL timers and events. 
 * lv_timer_handler() processes LVGL timers and events. 
 * The task sleeps for a duration determined by the time until the next LVGL timer is due, 
 * ensuring efficient CPU usage while maintaining responsive UI updates.
*/
static void lvgl_port_task(void *arg)
{
    (void)arg;
    static touch_event_t touch_event_data;
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&s_lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&s_lvgl_api_lock);
        time_till_next_ms = MAX(time_till_next_ms, DISPLAY_LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, DISPLAY_LVGL_TASK_MAX_DELAY_MS);

        TickType_t wait_ticks = pdMS_TO_TICKS(time_till_next_ms);
        if (wait_ticks == 0) {
            wait_ticks = 1;
        }

        QueueHandle_t touch_queue = cap_touch_get_event_queue();
        if (touch_queue != NULL && xQueueReceive(touch_queue, &touch_event_data, wait_ticks) == pdTRUE) {
            ESP_LOGI(TAG, "Touch event received: type=%d, x=%d, y=%d", touch_event_data.touch_type, touch_event_data.x, touch_event_data.y);
            if (handle_time_band_refresh_touch(&touch_event_data)) {
                continue;
            }
            if (handle_hourly_forecast_touch(&touch_event_data)) {
                _lock_acquire(&s_lvgl_api_lock);
                render_weather_current_ui(s_display);
                _lock_release(&s_lvgl_api_lock);
                continue;
            }
            if (handle_daily_view_touch(&touch_event_data)) {
                _lock_acquire(&s_lvgl_api_lock);
                render_weather_current_ui(s_display);
                _lock_release(&s_lvgl_api_lock);
                continue;
            }
        } else if (touch_queue == NULL) {
            vTaskDelay(wait_ticks);
        }
    }
}

/*
 * Initialize the LCD UI.
 */
esp_err_t lcd_ui_init(esp_lcd_panel_handle_t panel_handle)
{
    if (panel_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (lcd_ui_is_initialized()) {
        return ESP_OK;
    }

    if (s_ui_event_group == NULL) {
        s_ui_event_group = xEventGroupCreate();
        if (s_ui_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "Initialize LVGL library");
    lv_init();
    s_display = lv_display_create(DISPLAY_LCD_H_RES, DISPLAY_LCD_V_RES);
    if (s_display == NULL) {
        return ESP_FAIL;
    }

    lv_display_set_user_data(s_display, panel_handle);
    lv_display_set_color_format(s_display, DISPLAY_LV_COLOR_FORMAT);

    void *buf1 = NULL;
#if CONFIG_DISPLAY_USE_DOUBLE_FB
    void *buf2 = NULL;
    ESP_LOGI(TAG, "Use frame buffers as LVGL draw buffers");
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &buf1, &buf2), TAG,
                        "get frame buffers failed");
    lv_display_set_buffers(s_display, buf1, buf2,
                           DISPLAY_LCD_H_RES * DISPLAY_LCD_V_RES * DISPLAY_PIXEL_SIZE,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
#else
    ESP_LOGI(TAG, "Allocate LVGL draw buffer");
    size_t draw_buffer_sz = DISPLAY_LCD_H_RES * DISPLAY_LVGL_DRAW_BUF_LINES * DISPLAY_PIXEL_SIZE;
    buf1 = esp_lcd_rgb_alloc_draw_buffer(panel_handle, draw_buffer_sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    if (buf1 == NULL) {
        ESP_LOGW(TAG,
                 "Internal RAM draw buffer allocation failed (%u bytes). Trying PSRAM fallback.",
                 (unsigned)draw_buffer_sz);
        buf1 = esp_lcd_rgb_alloc_draw_buffer(panel_handle,
                                             draw_buffer_sz,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    if (buf1 == NULL) {
        ESP_LOGE(TAG,
                 "LVGL draw buffer allocation failed (%u bytes). free internal=%u free psram=%u",
                 (unsigned)draw_buffer_sz,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return ESP_ERR_NO_MEM;
    }

    lv_display_set_buffers(s_display, buf1, NULL, draw_buffer_sz, LV_DISPLAY_RENDER_MODE_PARTIAL);
#endif

    lv_display_set_flush_cb(s_display, lvgl_flush_cb);
#if CONFIG_DISPLAY_USE_DOUBLE_FB
    lv_display_set_flush_wait_cb(s_display, lvgl_flush_wait_cb);
#endif

    esp_lcd_rgb_panel_event_callbacks_t cbs = {
#if CONFIG_DISPLAY_USE_DOUBLE_FB
        .on_frame_buf_complete = on_frame_buf_complete,
#else
        .on_color_trans_done = notify_lvgl_flush_ready,
#endif
    };
    ESP_RETURN_ON_ERROR(esp_lcd_rgb_panel_register_event_callbacks(panel_handle, &cbs, s_display), TAG,
                        "register panel callbacks failed");

    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&lvgl_tick_timer_args, &s_lvgl_tick_timer), TAG,
                        "create lvgl tick timer failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_lvgl_tick_timer, DISPLAY_LVGL_TICK_PERIOD_MS * 1000), TAG,
                        "start lvgl tick timer failed");

    xTaskCreate(lvgl_port_task, "LVGL", DISPLAY_LVGL_TASK_STACK_SIZE, NULL,
                DISPLAY_LVGL_TASK_PRIORITY, &s_lvgl_task_handle);

    xEventGroupSetBits(s_ui_event_group, UI_READY_BIT);
    return ESP_OK;
}

/*
 * Show the time band UI.
 */
esp_err_t lcd_ui_show_time_band(void)
{
    return lcd_ui_render(render_time_band_ui_cb, NULL);
}

/*
 * Show the current weather UI.
 */
esp_err_t lcd_ui_show_weather_current(void)
{
    return lcd_ui_render(render_weather_current_ui_cb, NULL);
}

/*
 * Show the starting UI.
 */
esp_err_t lcd_ui_show_starting(void)
{
    return lcd_ui_render(render_starting_ui_cb, NULL);
}

/*
 * Start the weather coordinator task.
 */
esp_err_t lcd_ui_start_weather_coordinator(void)
{
    if (!lcd_ui_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ui_weather_task_handle != NULL) {
        return ESP_OK;
    }

    BaseType_t created = xTaskCreate(ui_weather_coordinator_task,
                                     "ui_weather",
                                     UI_WEATHER_COORD_TASK_STACK_SIZE,
                                     NULL,
                                     UI_WEATHER_COORD_TASK_PRIORITY,
                                     &s_ui_weather_task_handle);
    if (created != pdPASS) {
        s_ui_weather_task_handle = NULL;
        return ESP_FAIL;
    }

    return ESP_OK;
}

/*
 * Render a generic UI callback.
 */
esp_err_t lcd_ui_render(lcd_ui_render_fn_t render_fn, void *user_ctx)
{
    if (render_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!lcd_ui_is_initialized() || s_display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    _lock_acquire(&s_lvgl_api_lock);
    render_fn(s_display, user_ctx);
    _lock_release(&s_lvgl_api_lock);
    return ESP_OK;
}

/*
 * Check if the LCD UI is initialized.
 */
bool lcd_ui_is_initialized(void)
{
    if (s_ui_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_ui_event_group) & UI_READY_BIT) != 0;
}

/*
 * Wait until the LCD UI is ready or the specified timeout elapses.
 */
esp_err_t lcd_ui_wait_until_ready(uint32_t timeout_ms)
{
    if (lcd_ui_is_initialized()) {
        return ESP_OK;
    }
    if (s_ui_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    TickType_t wait_ticks = (timeout_ms == portMAX_DELAY)
                                ? portMAX_DELAY
                                : pdMS_TO_TICKS(timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_ui_event_group, UI_READY_BIT, pdFALSE, pdFALSE, wait_ticks);
    return (bits & UI_READY_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}

