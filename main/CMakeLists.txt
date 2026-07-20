idf_component_register(SRCS "call_api.c" "main.c" "lvgl_demo_ui.c" "rgb_lcd_panel.c" "wifi.c"
                       PRIV_REQUIRES esp_lcd esp_timer esp_driver_gpio esp_wifi esp_netif esp_event nvs_flash esp_http_client
                       INCLUDE_DIRS ".")
