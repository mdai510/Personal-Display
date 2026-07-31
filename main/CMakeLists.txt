idf_component_register(SRCS "weather_icons_24.c" "weather_icons_48.c" "cap_touch.c" "i2c.c" "json_parse.c" "time_sync.c" "lcd_ui.c" "call_api.c" "main.c" "rgb_lcd_panel.c" "wifi.c" "wifi_call.c"
                       PRIV_REQUIRES esp_lcd esp_timer esp_driver_gpio esp_wifi esp_netif esp_event nvs_flash esp_http_client
                       INCLUDE_DIRS ".")
