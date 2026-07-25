/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <assert.h>
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
#include "lcd_ui.h"

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
#define DISPLAY_LVGL_TASK_STACK_SIZE   (5 * 1024)
#define DISPLAY_LVGL_TASK_PRIORITY     2
#define DISPLAY_LVGL_TASK_MAX_DELAY_MS 500
#define DISPLAY_LVGL_TASK_MIN_DELAY_MS 1000 / CONFIG_FREERTOS_HZ

#define UI_READY_BIT BIT0

static _lock_t s_lvgl_api_lock;
static TaskHandle_t s_lvgl_task_handle;
static EventGroupHandle_t s_ui_event_group;
static lv_display_t *s_display;
static esp_timer_handle_t s_lvgl_tick_timer;

static lv_style_t style_bullet;
static lv_obj_t *scale1;
static const lv_font_t *font_normal = &lv_font_montserrat_14;
static lv_obj_t *s_time_band = NULL;
static lv_obj_t *s_date_label = NULL;
static lv_obj_t *s_time_label = NULL;
static lv_timer_t *s_time_timer = NULL;

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

    if (s_time_band == NULL) {
        s_time_band = lv_obj_create(screen);
        lv_obj_remove_style_all(s_time_band);
        lv_obj_set_size(s_time_band, LV_PCT(100), 44);
        lv_obj_align(s_time_band, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(s_time_band, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(s_time_band, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(s_time_band, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(s_time_band, 2, 0);
        lv_obj_set_style_border_color(s_time_band, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_pad_hor(s_time_band, 10, 0);
        lv_obj_set_style_pad_ver(s_time_band, 6, 0);

        s_date_label = lv_label_create(s_time_band);
        lv_obj_set_style_text_color(s_date_label, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_text_font(s_date_label, font_normal, 0);
        lv_obj_align(s_date_label, LV_ALIGN_LEFT_MID, 0, 0);

        s_time_label = lv_label_create(s_time_band);
        lv_obj_set_style_text_color(s_time_label, lv_color_hex(0x00FF00), 0);
        lv_obj_set_style_text_font(s_time_label, font_normal, 0);
        lv_obj_align(s_time_label, LV_ALIGN_RIGHT_MID, 0, 0);
    }

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

static lv_obj_t *create_scale_box(lv_obj_t *parent, const char *text1, const char *text2, const char *text3)
{
    lv_obj_t *scale = lv_scale_create(parent);
    lv_obj_center(scale);
    lv_obj_set_size(scale, 300, 300);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_OUTER);
    lv_scale_set_label_show(scale, false);
    lv_scale_set_post_draw(scale, true);
    lv_obj_set_width(scale, LV_PCT(100));
    lv_obj_set_style_pad_all(scale, 30, 0);

    lv_obj_t *bullet1 = lv_obj_create(parent);
    lv_obj_set_size(bullet1, 13, 13);
    lv_obj_remove_style(bullet1, NULL, LV_PART_SCROLLBAR);
    lv_obj_add_style(bullet1, &style_bullet, 0);
    lv_obj_set_style_bg_color(bullet1, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_t *label1 = lv_label_create(parent);
    lv_label_set_text(label1, text1);

    lv_obj_t *bullet2 = lv_obj_create(parent);
    lv_obj_set_size(bullet2, 13, 13);
    lv_obj_remove_style(bullet2, NULL, LV_PART_SCROLLBAR);
    lv_obj_add_style(bullet2, &style_bullet, 0);
    lv_obj_set_style_bg_color(bullet2, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_t *label2 = lv_label_create(parent);
    lv_label_set_text(label2, text2);

    lv_obj_t *bullet3 = lv_obj_create(parent);
    lv_obj_set_size(bullet3, 13, 13);
    lv_obj_remove_style(bullet3, NULL, LV_PART_SCROLLBAR);
    lv_obj_add_style(bullet3, &style_bullet, 0);
    lv_obj_set_style_bg_color(bullet3, lv_palette_main(LV_PALETTE_GREEN), 0);
    lv_obj_t *label3 = lv_label_create(parent);
    lv_label_set_text(label3, text3);

    static int32_t grid_col_dsc[] = {LV_GRID_CONTENT, LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static int32_t grid_row_dsc[] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(parent, grid_col_dsc, grid_row_dsc);
    lv_obj_set_grid_cell(scale, LV_GRID_ALIGN_START, 0, 2, LV_GRID_ALIGN_START, 1, 1);
    lv_obj_set_grid_cell(bullet1, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_START, 2, 1);
    lv_obj_set_grid_cell(bullet2, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_START, 3, 1);
    lv_obj_set_grid_cell(bullet3, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_START, 4, 1);
    lv_obj_set_grid_cell(label1, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_START, 2, 1);
    lv_obj_set_grid_cell(label2, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_START, 3, 1);
    lv_obj_set_grid_cell(label3, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_START, 4, 1);
    return scale;
}

static void scale1_indic1_anim_cb(void *var, int32_t v)
{
    lv_arc_set_value(var, v);
    lv_obj_t *card = lv_obj_get_parent(scale1);
    lv_obj_t *label = lv_obj_get_child(card, -5);
    lv_label_set_text_fmt(label, "Revenue: %"LV_PRId32" %%", v);
}

static void scale1_indic2_anim_cb(void *var, int32_t v)
{
    lv_arc_set_value(var, v);
    lv_obj_t *card = lv_obj_get_parent(scale1);
    lv_obj_t *label = lv_obj_get_child(card, -3);
    lv_label_set_text_fmt(label, "Sales: %"LV_PRId32" %%", v);
}

static void scale1_indic3_anim_cb(void *var, int32_t v)
{
    lv_arc_set_value(var, v);
    lv_obj_t *card = lv_obj_get_parent(scale1);
    lv_obj_t *label = lv_obj_get_child(card, -1);
    lv_label_set_text_fmt(label, "Costs: %"LV_PRId32" %%", v);
}

static void render_demo_ui(lv_display_t *disp)
{
    lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED), LV_THEME_DEFAULT_DARK,
                          font_normal);
    lv_style_init(&style_bullet);
    lv_style_set_border_width(&style_bullet, 0);
    lv_style_set_radius(&style_bullet, LV_RADIUS_CIRCLE);

    lv_obj_t *parent = lv_display_get_screen_active(disp);
    scale1 = create_scale_box(parent, "Revenue", "Sales", "Costs");

    lv_obj_t *arc;
    arc = lv_arc_create(scale1);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_remove_style(arc, NULL, LV_PART_MAIN);
    lv_obj_set_size(arc, lv_pct(100), lv_pct(100));
    lv_obj_set_style_arc_opa(arc, 0, 0);
    lv_obj_set_style_arc_width(arc, 15, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_values(&a, 20, 100);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&a, scale1_indic1_anim_cb);
    lv_anim_set_var(&a, arc);
    lv_anim_set_duration(&a, 4100);
    lv_anim_set_playback_duration(&a, 2700);
    lv_anim_start(&a);

    arc = lv_arc_create(scale1);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_size(arc, lv_pct(100), lv_pct(100));
    lv_obj_set_style_margin_all(arc, 20, 0);
    lv_obj_set_style_arc_opa(arc, 0, 0);
    lv_obj_set_style_arc_width(arc, 15, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_palette_main(LV_PALETTE_RED), LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(arc);

    lv_anim_set_exec_cb(&a, scale1_indic2_anim_cb);
    lv_anim_set_var(&a, arc);
    lv_anim_set_duration(&a, 2600);
    lv_anim_set_playback_duration(&a, 3200);
    lv_anim_start(&a);

    arc = lv_arc_create(scale1);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_size(arc, lv_pct(100), lv_pct(100));
    lv_obj_set_style_margin_all(arc, 40, 0);
    lv_obj_set_style_arc_opa(arc, 0, 0);
    lv_obj_set_style_arc_width(arc, 15, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_palette_main(LV_PALETTE_GREEN), LV_PART_INDICATOR);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(arc);

    lv_anim_set_exec_cb(&a, scale1_indic3_anim_cb);
    lv_anim_set_var(&a, arc);
    lv_anim_set_duration(&a, 2800);
    lv_anim_set_playback_duration(&a, 1800);
    lv_anim_start(&a);
}

static void render_demo_ui_cb(lv_display_t *disp, void *user_ctx)
{
    (void)user_ctx;
    render_demo_ui(disp);
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

esp_err_t lcd_ui_show_demo(void)
{
    return lcd_ui_render(render_demo_ui_cb, NULL);
}

esp_err_t lcd_ui_show_time_band(void)
{
    return lcd_ui_render(render_time_band_ui_cb, NULL);
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
