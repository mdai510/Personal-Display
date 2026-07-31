/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <sys/lock.h>
#include <sys/param.h>
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

#define DISPLAY_LVGL_DRAW_BUF_LINES    50
#define DISPLAY_LVGL_TICK_PERIOD_MS    2
#define DISPLAY_LVGL_TASK_STACK_SIZE   (16 * 1024)
#define DISPLAY_LVGL_TASK_PRIORITY     2
#define DISPLAY_LVGL_TASK_MAX_DELAY_MS 500
#define DISPLAY_LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ

#define UI_READY_BIT BIT0
#define WEATHER_PANEL_TOP_OFFSET 44

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
    WEATHER_ICON_COUNT
} weather_icon_id_t;

static const uint32_t s_weather_icon_codepoint_lookup[WEATHER_ICON_COUNT] = {
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
};

static _lock_t s_lvgl_api_lock;
static TaskHandle_t s_lvgl_task_handle;
static EventGroupHandle_t s_ui_event_group;
static lv_display_t *s_display;
static esp_timer_handle_t s_lvgl_tick_timer;

static const lv_font_t *font_normal = &lv_font_montserrat_16;
static lcd_ui_panel_t s_starting_panel;
static lcd_ui_panel_t s_time_band_panel;
static lcd_ui_panel_t s_weather_panel;
static lv_obj_t *s_starting_label = NULL;
static lv_obj_t *s_date_label = NULL;
static lv_obj_t *s_time_label = NULL;
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
static lv_obj_t *s_weather_hilo_label = NULL;
static lv_obj_t *s_weather_sunrise_icon_label = NULL;
static lv_obj_t *s_weather_sunrise_label = NULL;
static lv_obj_t *s_weather_sunset_icon_label = NULL;
static lv_obj_t *s_weather_sunset_label = NULL;
static lcd_ui_weather_layout_t s_weather_layout = LCD_UI_WEATHER_LAYOUT_BIG;

static bool weather_icon_font_has_glyph(const lv_font_t *font, uint32_t codepoint);
static uint32_t weather_icon_get_codepoint(weather_icon_id_t icon_id);
static weather_icon_id_t weather_code_to_icon_id(int code, bool is_day);
static bool set_label_to_icon_id(lv_obj_t *label, const lv_font_t *font, weather_icon_id_t icon_id);
static lv_obj_t *create_weather_metric_row(lv_obj_t *parent, lv_obj_t **icon_label_out, lv_obj_t **value_label_out);
static lv_obj_t *create_weather_header_row(lv_obj_t *parent, lv_obj_t **icon_label_out, lv_obj_t **temp_label_out);
static lv_obj_t *create_weather_right_icon_row(lv_obj_t *parent, lv_obj_t **icon_label_out, lv_obj_t **value_label_out);
static const char *weather_code_to_interpretation(int code);

static void format_value_1dp(char *buf, size_t buf_len, const char *prefix, float value, const char *suffix)
{
    int scaled = (int)(value * 10.0f + (value >= 0.0f ? 0.5f : -0.5f));
    int abs_scaled = (scaled < 0) ? -scaled : scaled;
    int whole = abs_scaled / 10;
    int frac = abs_scaled % 10;
    snprintf(buf, buf_len, "%s%s%d.%d%s", prefix, (scaled < 0) ? "-" : "", whole, frac, suffix);
}

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

static bool weather_icon_font_has_glyph(const lv_font_t *font, uint32_t codepoint)
{
    if (font == NULL) {
        return false;
    }

    lv_font_glyph_dsc_t dsc;
    return lv_font_get_glyph_dsc(font, &dsc, codepoint, 0);
}

static uint32_t weather_icon_get_codepoint(weather_icon_id_t icon_id)
{
    if (icon_id < 0 || icon_id >= WEATHER_ICON_COUNT) {
        return 0;
    }
    return s_weather_icon_codepoint_lookup[icon_id];
}

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

static bool set_label_to_icon_id(lv_obj_t *label, const lv_font_t *font, weather_icon_id_t icon_id)
{
    if (label == NULL || font == NULL) {
        return false;
    }

    uint32_t codepoint = weather_icon_get_codepoint(icon_id);
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
    lv_obj_set_style_text_color(*icon_label_out, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(*icon_label_out, &weather_icons_24, 0);

    *value_label_out = lv_label_create(row);
    lv_obj_set_style_text_color(*value_label_out, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(*value_label_out, font_normal, 0);

    return row;
}

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
    lv_obj_set_style_text_color(*icon_label_out, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(*icon_label_out, &weather_icons_48, 0);

    *temp_label_out = lv_label_create(row);
    lv_obj_set_style_text_color(*temp_label_out, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(*temp_label_out, &lv_font_montserrat_36, 0);

    return row;
}

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
    lv_obj_set_style_text_color(*icon_label_out, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(*icon_label_out, &weather_icons_24, 0);

    *value_label_out = lv_label_create(row);
    lv_obj_set_style_text_color(*value_label_out, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_font(*value_label_out, font_normal, 0);
    lv_obj_set_style_text_align(*value_label_out, LV_TEXT_ALIGN_RIGHT, 0);

    return row;
}


static void ui_panel_change_layout(lcd_ui_panel_t *panel, int32_t x, int32_t y, lv_coord_t w, lv_coord_t h)
{
    if (panel == NULL) {
        return;
    }

    panel->x = x;
    panel->y = y;
    panel->w = w;
    panel->h = h;

    if (panel->root != NULL) {
        lv_obj_set_pos(panel->root, panel->x, panel->y);
        lv_obj_set_size(panel->root, panel->w, panel->h);
    }
}

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

static void weather_panel_apply_layout(lcd_ui_weather_layout_t layout)
{
    if (layout == LCD_UI_WEATHER_LAYOUT_SMALL) {
        ui_panel_change_layout(&s_weather_panel,
                               0,
                               WEATHER_PANEL_TOP_OFFSET,
                               LV_PCT(35),
                               (DISPLAY_LCD_V_RES - WEATHER_PANEL_TOP_OFFSET) / 2);
        return;
    }

    ui_panel_change_layout(&s_weather_panel,
                           0,
                           WEATHER_PANEL_TOP_OFFSET,
                           LV_PCT(50),
                           (DISPLAY_LCD_V_RES - WEATHER_PANEL_TOP_OFFSET) / 2);
}

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

static void time_band_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_time_label();
}

static void render_time_band_ui(lv_display_t *disp)
{
    lv_obj_t *screen = lv_display_get_screen_active(disp);

    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(0x00FF00), 0);

    ui_panel_hide(&s_starting_panel);

    if (s_time_band_panel.root == NULL) {
        s_time_band_panel.root = lv_obj_create(screen);
        lv_obj_remove_style_all(s_time_band_panel.root);
        ui_panel_change_layout(&s_time_band_panel, 0, 0, LV_PCT(100), 44);
        lv_obj_set_style_bg_color(s_time_band_panel.root, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_time_band_panel.root, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(s_time_band_panel.root, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(s_time_band_panel.root, 2, 0);
        lv_obj_set_style_border_color(s_time_band_panel.root, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_pad_hor(s_time_band_panel.root, 10, 0);
        lv_obj_set_style_pad_ver(s_time_band_panel.root, 6, 0);

        s_date_label = lv_label_create(s_time_band_panel.root);
        lv_obj_set_style_text_color(s_date_label, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_text_font(s_date_label, font_normal, 0);
        lv_obj_align(s_date_label, LV_ALIGN_LEFT_MID, 0, 0);

        s_time_label = lv_label_create(s_time_band_panel.root);
        lv_obj_set_style_text_color(s_time_label, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_text_font(s_time_label, font_normal, 0);
        lv_obj_align(s_time_label, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    ui_panel_show(&s_time_band_panel);

    update_time_label();

    if (s_time_timer == NULL) {
        s_time_timer = lv_timer_create(time_band_timer_cb, 1000, NULL);
    }
}

static void render_time_band_ui_cb(lv_display_t *disp, void *user_ctx)
{
    (void)user_ctx;
    render_time_band_ui(disp);
}

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
    lv_obj_set_style_text_color(screen, lv_color_hex(0x00FF00), 0);

    ui_panel_hide(&s_starting_panel);

    if (s_weather_panel.root == NULL) {
        s_weather_panel.root = lv_obj_create(screen);
        lv_obj_remove_style_all(s_weather_panel.root);
        weather_panel_apply_layout(s_weather_layout);
        lv_obj_set_style_bg_color(s_weather_panel.root, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_weather_panel.root, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(s_weather_panel.root, 2, 0);
        lv_obj_set_style_border_color(s_weather_panel.root, lv_color_hex(0x00FF00), 0);
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
        lv_obj_set_style_text_color(s_weather_desc_label, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_text_font(s_weather_desc_label, font_normal, 0);
        lv_obj_set_width(s_weather_desc_label, LV_PCT(100));
        lv_label_set_long_mode(s_weather_desc_label, LV_LABEL_LONG_WRAP);

        create_weather_metric_row(s_weather_left_col, &s_weather_humidity_icon_label, &s_weather_humidity_label);
        create_weather_metric_row(s_weather_left_col, &s_weather_wind_icon_label, &s_weather_wind_label);
        create_weather_metric_row(s_weather_left_col, &s_weather_cloud_icon_label, &s_weather_cloud_label);

        s_weather_hilo_label = lv_label_create(s_weather_right_col);
        lv_obj_set_style_text_color(s_weather_hilo_label, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_text_font(s_weather_hilo_label, font_normal, 0);
        lv_obj_set_style_text_align(s_weather_hilo_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_width(s_weather_hilo_label, LV_PCT(100));
        lv_label_set_long_mode(s_weather_hilo_label, LV_LABEL_LONG_WRAP);

        create_weather_right_icon_row(s_weather_right_col, &s_weather_sunrise_icon_label, &s_weather_sunrise_label);
        create_weather_right_icon_row(s_weather_right_col, &s_weather_sunset_icon_label, &s_weather_sunset_label);
    }

    weather_panel_apply_layout(s_weather_layout);
    ui_panel_show(&s_weather_panel);

    char value_buf[48];

    const weather_current_t *current = &forecast.current;
    const lv_font_t *icon_font = &weather_icons_48;
    weather_icon_id_t icon_id = weather_code_to_icon_id(current->weather_code, current->is_day != 0);
    if (!set_label_to_icon_id(s_weather_icon_label, icon_font, icon_id)) {
        lv_obj_set_style_text_font(s_weather_icon_label, font_normal, 0);
        lv_label_set_text(s_weather_icon_label, "?");
    }
    lv_label_set_text(s_weather_desc_label, weather_code_to_interpretation(current->weather_code));

    format_value_1dp(value_buf, sizeof(value_buf), "", current->temperature_2m, " F");
    lv_label_set_text(s_weather_temp_label, value_buf);

    set_label_to_icon_id(s_weather_humidity_icon_label, &weather_icons_24, WEATHER_ICON_HUMIDITY);
    set_label_to_icon_id(s_weather_wind_icon_label, &weather_icons_24, WEATHER_ICON_WIND_GUST);
    set_label_to_icon_id(s_weather_cloud_icon_label, &weather_icons_24, WEATHER_ICON_SINGLE_CLOUD);

    lv_label_set_text_fmt(s_weather_humidity_label, "%d%%", current->relative_humidity_2m);
    format_value_1dp(value_buf, sizeof(value_buf), "", current->wind_speed_10m, " mph");
    lv_label_set_text(s_weather_wind_label, value_buf);
    lv_label_set_text_fmt(s_weather_cloud_label, "%d%%", current->cloud_cover);

    if (forecast.daily_count > 0) {
        const weather_daily_point_t *today = &forecast.daily[0];
        int high = (int)(today->temperature_2m_max + (today->temperature_2m_max >= 0.0f ? 0.5f : -0.5f));
        int low = (int)(today->temperature_2m_min + (today->temperature_2m_min >= 0.0f ? 0.5f : -0.5f));
        lv_label_set_text_fmt(s_weather_hilo_label, "H: %d F\nL: %d F", high, low);

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
        lv_label_set_text(s_weather_hilo_label, "H: -- F\nL: -- F");
        lv_label_set_text(s_weather_sunrise_label, "--:--");
        lv_label_set_text(s_weather_sunset_label, "--:--");
    }

    set_label_to_icon_id(s_weather_sunrise_icon_label, &weather_icons_24, WEATHER_ICON_SUNRISE);
    set_label_to_icon_id(s_weather_sunset_icon_label, &weather_icons_24, WEATHER_ICON_SUNSET);
}

static void render_weather_current_ui_cb(lv_display_t *disp, void *user_ctx)
{
    (void)user_ctx;
    render_weather_current_ui(disp);
}

static void render_weather_layout_cb(lv_display_t *disp, void *user_ctx)
{
    (void)disp;
    if (user_ctx == NULL) {
        return;
    }

    s_weather_layout = *(lcd_ui_weather_layout_t *)user_ctx;
    weather_panel_apply_layout(s_weather_layout);
}

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
        lv_obj_set_style_text_color(s_starting_label, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_text_font(s_starting_label, font_normal, 0);
    }

    ui_panel_change_layout(&s_starting_panel, 0, 0, LV_PCT(100), LV_PCT(100));
    lv_label_set_text(s_starting_label, "starting...");
    lv_obj_center(s_starting_label);

    ui_panel_hide(&s_time_band_panel);
    ui_panel_hide(&s_weather_panel);
    ui_panel_show(&s_starting_panel);
}

static void render_starting_ui_cb(lv_display_t *disp, void *user_ctx)
{
    (void)user_ctx;
    render_starting_ui(disp);
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
    uint32_t time_till_next_ms = 0;
    while (1) {
        _lock_acquire(&s_lvgl_api_lock);
        time_till_next_ms = lv_timer_handler();
        _lock_release(&s_lvgl_api_lock);
        time_till_next_ms = MAX(time_till_next_ms, DISPLAY_LVGL_TASK_MIN_DELAY_MS);
        time_till_next_ms = MIN(time_till_next_ms, DISPLAY_LVGL_TASK_MAX_DELAY_MS);
        usleep(1000 * time_till_next_ms);
    }
}

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
    assert(buf1);
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

esp_err_t lcd_ui_show_time_band(void)
{
    return lcd_ui_render(render_time_band_ui_cb, NULL);
}

esp_err_t lcd_ui_show_weather_current(void)
{
    return lcd_ui_render(render_weather_current_ui_cb, NULL);
}

esp_err_t lcd_ui_show_starting(void)
{
    return lcd_ui_render(render_starting_ui_cb, NULL);
}

esp_err_t lcd_ui_set_weather_layout(lcd_ui_weather_layout_t layout)
{
    if (layout != LCD_UI_WEATHER_LAYOUT_BIG && layout != LCD_UI_WEATHER_LAYOUT_SMALL) {
        return ESP_ERR_INVALID_ARG;
    }
    return lcd_ui_render(render_weather_layout_cb, &layout);
}

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

bool lcd_ui_is_initialized(void)
{
    if (s_ui_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_ui_event_group) & UI_READY_BIT) != 0;
}

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
