#include "wifi_call.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ble.h"
#include "call_api.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lcd_ui.h"
#include "nvs.h"
#include "time_sync.h"
#include "wifi.h"

static const char *TAG = "wifi_call";

#define WIFI_CALL_EVENT_WEATHER_READY BIT0
#define WIFI_CALL_EVENT_LOCATION_CHANGE BIT1
#define WIFI_CALL_EVENT_REFRESH_REQUEST BIT2
#define WIFI_CALL_EVENT_LOCATION_CHANGE_CANCEL BIT3

#define WIFI_INFO_NAMESPACE "wifi_info"
#define LOCATION_INFO_NAMESPACE "location_info"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"
#define NVS_KEY_LATITUDE "lat"
#define NVS_KEY_LONGITUDE "lon"
#define NVS_KEY_UTC_OFFSET "utc_offset"
#define NVS_KEY_UTC_OFFSET_S "utc_off_s"

#define WIFI_RETRY_DELAY_MS 1500
#define BLE_POLL_DELAY_MS 300
#define WEATHER_REFRESH_MS (10 * 60 * 1000)
#define WIFI_CALL_ERR_LOCATION_CHANGE_CANCELED ESP_FAIL

typedef struct {
    double latitude;
    double longitude;
    char utc_offset[64];
    int32_t utc_offset_seconds;
} location_data_t;

static EventGroupHandle_t s_wifi_call_event_group = NULL;
static bool s_wifi_call_task_started = false;
static bool s_use_manual_wifi_credentials = false;
static bool s_manual_location_set = false;
static location_data_t s_manual_location = {0};

static esp_err_t wifi_call_load_wifi_from_nvs(char *ssid, size_t ssid_len, char *password, size_t password_len);
static esp_err_t wifi_call_load_location_from_nvs(location_data_t *out_location);
static esp_err_t wifi_call_get_connect_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len);
static esp_err_t wifi_call_connect_with_retries(void);
static esp_err_t wifi_call_wait_for_wifi_payload(char *ssid, size_t ssid_len, char *password, size_t password_len);
static esp_err_t wifi_call_wait_for_location_payload(location_data_t *out_location);
static esp_err_t wifi_call_wait_for_location_payload_since(location_data_t *out_location,
                                                           uint32_t min_version,
                                                           bool allow_cancel);
static esp_err_t wifi_call_fetch_and_show_weather(void);
static esp_err_t wifi_call_run_onboarding(void);
static esp_err_t wifi_call_fetch_weather_from_location(const location_data_t *location);
static void wifi_call_show_ble_message(const char *base_message, bool location_change_screen);

static void wifi_call_show_ble_message(const char *base_message, bool location_change_screen) {
    char device_name[32] = "PersDisplay";
    char message[224];

    if (base_message == NULL) {
        return;
    }

    if (ble_get_device_name(device_name, sizeof(device_name)) != ESP_OK) {
        snprintf(device_name, sizeof(device_name), "PersDisplay");
    }

    snprintf(message,
             sizeof(message),
             "%s\n\nPair with device:\n%s",
             base_message,
             device_name);

    if (location_change_screen) {
        (void)lcd_ui_show_location_change_screen(message);
    } else {
        (void)lcd_ui_show_message_screen(message);
    }
}

void wifi_call_set_use_manual_wifi_credentials(bool enable) {
    s_use_manual_wifi_credentials = enable;
}

esp_err_t wifi_call_set_manual_location(double latitude,
                                        double longitude,
                                        const char *timezone,
                                        int32_t utc_offset_seconds) {
    if (timezone == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strlen(timezone) + 1 > sizeof(s_manual_location.utc_offset)) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_manual_location.latitude = latitude;
    s_manual_location.longitude = longitude;
    s_manual_location.utc_offset_seconds = utc_offset_seconds;
    memcpy(s_manual_location.utc_offset, timezone, strlen(timezone) + 1);
    s_manual_location_set = true;
    return ESP_OK;
}

bool wifi_call_is_weather_ready(void) {
    if (s_wifi_call_event_group == NULL) {
        return false;
    }

    return (xEventGroupGetBits(s_wifi_call_event_group) & WIFI_CALL_EVENT_WEATHER_READY) != 0;
}

esp_err_t wifi_call_wait_weather_ready(uint32_t timeout_ms) {
    EventBits_t bits;
    TickType_t timeout_ticks;

    if (s_wifi_call_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    timeout_ticks = (timeout_ms == portMAX_DELAY) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

    bits = xEventGroupWaitBits(s_wifi_call_event_group,
                               WIFI_CALL_EVENT_WEATHER_READY,
                               pdTRUE,
                               pdTRUE,
                               timeout_ticks);

    if ((bits & WIFI_CALL_EVENT_WEATHER_READY) == 0) {
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t wifi_call_request_location_change(void) {
    if (s_wifi_call_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xEventGroupSetBits(s_wifi_call_event_group, WIFI_CALL_EVENT_LOCATION_CHANGE);
    return ESP_OK;
}

esp_err_t wifi_call_request_location_change_cancel(void) {
    if (s_wifi_call_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xEventGroupSetBits(s_wifi_call_event_group, WIFI_CALL_EVENT_LOCATION_CHANGE_CANCEL);
    return ESP_OK;
}

esp_err_t wifi_call_fetch_weather_once(void) {
    esp_err_t err = wifi_call_fetch_and_show_weather();
    if (err == ESP_OK && s_wifi_call_event_group != NULL) {
        xEventGroupSetBits(s_wifi_call_event_group, WIFI_CALL_EVENT_WEATHER_READY);
    }
    return err;
}

esp_err_t wifi_call_request_refresh(void) {
    if (s_wifi_call_event_group == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xEventGroupSetBits(s_wifi_call_event_group, WIFI_CALL_EVENT_REFRESH_REQUEST);
    return ESP_OK;
}

static esp_err_t wifi_call_load_wifi_from_nvs(char *ssid, size_t ssid_len, char *password, size_t password_len) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    size_t ssid_size = ssid_len;
    size_t pass_size = password_len;

    err = nvs_open(WIFI_INFO_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_str(nvs_handle, NVS_KEY_SSID, ssid, &ssid_size);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_get_str(nvs_handle, NVS_KEY_PASSWORD, password, &pass_size);
    nvs_close(nvs_handle);
    return err;
}

static esp_err_t wifi_call_load_location_from_nvs(location_data_t *out_location) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    size_t utc_len = sizeof(out_location->utc_offset);

    if (out_location == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(LOCATION_INFO_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_get_blob(nvs_handle, NVS_KEY_LATITUDE, &out_location->latitude, &(size_t){sizeof(out_location->latitude)});
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_get_blob(nvs_handle, NVS_KEY_LONGITUDE, &out_location->longitude, &(size_t){sizeof(out_location->longitude)});
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_get_str(nvs_handle, NVS_KEY_UTC_OFFSET, out_location->utc_offset, &utc_len);
    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_get_i32(nvs_handle, NVS_KEY_UTC_OFFSET_S, &out_location->utc_offset_seconds);
    nvs_close(nvs_handle);
    return err;
}

static esp_err_t wifi_call_get_connect_credentials(char *ssid, size_t ssid_len, char *password, size_t password_len) {
    esp_err_t err;

    if (ssid == NULL || password == NULL || ssid_len == 0 || password_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_use_manual_wifi_credentials) {
        if (strlen(DISPLAY_WIFI_SSID) + 1 > ssid_len || strlen(DISPLAY_WIFI_PASS) + 1 > password_len) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(ssid, DISPLAY_WIFI_SSID, strlen(DISPLAY_WIFI_SSID) + 1);
        memcpy(password, DISPLAY_WIFI_PASS, strlen(DISPLAY_WIFI_PASS) + 1);
        return ESP_OK;
    }

    err = wifi_call_load_wifi_from_nvs(ssid, ssid_len, password, password_len);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    if (strlen(DISPLAY_WIFI_SSID) + 1 > ssid_len || strlen(DISPLAY_WIFI_PASS) + 1 > password_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(ssid, DISPLAY_WIFI_SSID, strlen(DISPLAY_WIFI_SSID) + 1);
    memcpy(password, DISPLAY_WIFI_PASS, strlen(DISPLAY_WIFI_PASS) + 1);
    return ESP_OK;
}

static esp_err_t wifi_call_fetch_weather_from_location(const location_data_t *location) {
    const char *timezone = NULL;
    esp_err_t err;

    if (location == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (location->utc_offset[0] == '+' || location->utc_offset[0] == '-') {
        timezone = "auto";
    } else {
        timezone = location->utc_offset;
    }

    err = call_weather_api_24h(location->latitude, location->longitude, timezone);
    if (err != ESP_OK) {
        return err;
    }

    return call_weather_api_7d(location->latitude, location->longitude, timezone);
}

static esp_err_t wifi_call_connect_with_retries(void) {
    char ssid[33] = {0};
    char password[65] = {0};
    esp_err_t err;
    int attempt;

    for (attempt = 0; attempt < 1; ++attempt) {
        (void)wifi_station_disconnect();
        (void)wifi_station_deinit();

        err = wifi_station_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init Wi-Fi stack: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
            continue;
        }

        err = wifi_call_get_connect_credentials(ssid, sizeof(ssid), password, sizeof(password));
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get Wi-Fi credentials: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
            continue;
        }

        err = wifi_station_connect(ssid, password);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Wi-Fi connected with SSID '%s'", ssid);
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Wi-Fi connect attempt %d failed for SSID '%s': %s",
                 attempt + 1,
                 ssid,
                 esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
    }

    return ESP_FAIL;
}

static esp_err_t wifi_call_wait_for_wifi_payload(char *ssid, size_t ssid_len, char *password, size_t password_len) {
    esp_err_t err;

    (void)ble_set_write_permissions(true, false);
    wifi_call_show_ble_message("Wi-Fi failed\nSend SSID/password over BLE", false);

    for (;;) {
        err = ble_get_wifi_info(ssid, ssid_len, password, password_len);
        if (err == ESP_OK) {
            char msg[96];
            snprintf(msg, sizeof(msg), "Received SSID:\n%s\nRetrying Wi-Fi...", ssid);
            lcd_ui_show_message_screen(msg);
            (void)ble_set_write_permissions(false, false);
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(BLE_POLL_DELAY_MS));
    }
}

static esp_err_t wifi_call_wait_for_location_payload(location_data_t *out_location) {
    return wifi_call_wait_for_location_payload_since(out_location, 0, false);
}

static esp_err_t wifi_call_wait_for_location_payload_since(location_data_t *out_location,
                                                           uint32_t min_version,
                                                           bool allow_cancel) {
    esp_err_t err;

    if (out_location == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    (void)ble_set_write_permissions(false, true);
    wifi_call_show_ble_message("Need location\nSend lat/lon/UTC over BLE", allow_cancel);

    for (;;) {
        uint32_t current_version;

        if (allow_cancel) {
            EventBits_t bits = xEventGroupGetBits(s_wifi_call_event_group);
            if ((bits & WIFI_CALL_EVENT_LOCATION_CHANGE_CANCEL) != 0) {
                xEventGroupClearBits(s_wifi_call_event_group, WIFI_CALL_EVENT_LOCATION_CHANGE_CANCEL);
                (void)ble_set_write_permissions(false, false);
                return WIFI_CALL_ERR_LOCATION_CHANGE_CANCELED;
            }
        }

        current_version = ble_get_location_info_version();
        if (current_version <= min_version) {
            vTaskDelay(pdMS_TO_TICKS(BLE_POLL_DELAY_MS));
            continue;
        }

        err = ble_get_location_info(&out_location->latitude,
                                    &out_location->longitude,
                                    out_location->utc_offset,
                                    sizeof(out_location->utc_offset),
                                    &out_location->utc_offset_seconds);
        if (err == ESP_OK) {
            lcd_ui_show_message_screen("Location received\nRetrying weather...");
            (void)ble_set_write_permissions(false, false);
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(BLE_POLL_DELAY_MS));
    }
}

static esp_err_t wifi_call_fetch_and_show_weather(void) {
    esp_err_t err;
    location_data_t location = {0};

    if (s_manual_location_set) {
        err = time_sync_once_with_utc_offset(s_manual_location.utc_offset_seconds,
                                             s_manual_location.utc_offset);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Time sync failed for manual location: %s", esp_err_to_name(err));
            return err;
        }

        err = wifi_call_fetch_weather_from_location(&s_manual_location);
        return err;
    }

    err = wifi_call_load_location_from_nvs(&location);
    if (err == ESP_OK) {
        err = time_sync_once_with_utc_offset(location.utc_offset_seconds, location.utc_offset);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Time sync failed for saved location: %s", esp_err_to_name(err));
            return err;
        }

        err = wifi_call_fetch_weather_from_location(&location);
        return err;
    }

    return ESP_ERR_NOT_FOUND;
}

static esp_err_t wifi_call_run_onboarding(void) {
    location_data_t location = {0};
    char ssid[33] = {0};
    char password[65] = {0};
    esp_err_t err;

    for (;;) {
        err = wifi_call_connect_with_retries();
        if (err != ESP_OK) {
            wifi_call_show_ble_message("Wi-Fi failed\nWaiting for BLE update", false);
            err = wifi_call_wait_for_wifi_payload(ssid, sizeof(ssid), password, sizeof(password));
            if (err != ESP_OK) {
                continue;
            }
            (void)wifi_station_disconnect();
            (void)wifi_station_deinit();
            continue;
        }

        err = wifi_call_fetch_and_show_weather();
        if (err == ESP_OK) {
            xEventGroupSetBits(s_wifi_call_event_group, WIFI_CALL_EVENT_WEATHER_READY);
            if (ble_has_wifi_info() && (ble_has_location_info() || s_manual_location_set)) {
                ble_stop_advertising();
            }
            return ESP_OK;
        }

        wifi_call_show_ble_message("Weather fetch failed\nNeed location via BLE", false);
        err = wifi_call_wait_for_location_payload(&location);
        if (err != ESP_OK) {
            continue;
        }

        err = wifi_call_fetch_weather_from_location(&location);
        if (err != ESP_OK) {
            lcd_ui_show_message_screen("Invalid location\nSend again via BLE");
            continue;
        }

        err = time_sync_once_with_utc_offset(location.utc_offset_seconds, location.utc_offset);
        if (err != ESP_OK) {
            wifi_call_show_ble_message("Time sync failed\nRetry location via BLE", false);
            continue;
        }

        err = ESP_OK;
        if (err == ESP_OK) {
            xEventGroupSetBits(s_wifi_call_event_group, WIFI_CALL_EVENT_WEATHER_READY);
            if (ble_has_wifi_info()) {
                ble_stop_advertising();
            }
            return ESP_OK;
        }

        wifi_call_show_ble_message("Weather still failing\nRetry location via BLE", false);
    }
}

static void wifi_call_task(void *arg) {
    (void)arg;

    for (;;) {
        EventBits_t bits;
        esp_err_t err;

        (void)ble_set_write_permissions(false, false);

        xEventGroupClearBits(s_wifi_call_event_group, WIFI_CALL_EVENT_WEATHER_READY);
        lcd_ui_show_message_screen("Starting connectivity...");

        if (wifi_call_run_onboarding() == ESP_OK) {
            for (;;) {
                location_data_t location = {0};

                bits = xEventGroupWaitBits(s_wifi_call_event_group,
                                           WIFI_CALL_EVENT_LOCATION_CHANGE | WIFI_CALL_EVENT_REFRESH_REQUEST,
                                           pdTRUE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WEATHER_REFRESH_MS));

                if ((bits & WIFI_CALL_EVENT_LOCATION_CHANGE) != 0) {
                    uint32_t start_version = ble_get_location_info_version();
                    xEventGroupClearBits(s_wifi_call_event_group, WIFI_CALL_EVENT_LOCATION_CHANGE_CANCEL);

                    wifi_call_show_ble_message("Location change requested\nSend new location via BLE", true);
                    ble_start_advertising();

                    err = wifi_call_wait_for_location_payload_since(&location, start_version, true);
                    if (err == WIFI_CALL_ERR_LOCATION_CHANGE_CANCELED) {
                        (void)ble_stop_advertising();
                        (void)lcd_ui_show_time_band();
                        (void)lcd_ui_show_weather_current();
                        continue;
                    }

                    if (err != ESP_OK) {
                        lcd_ui_show_message_screen("Location input failed\nRetrying...");
                        continue;
                    }

                    if (wifi_call_fetch_weather_from_location(&location) != ESP_OK) {
                        lcd_ui_show_message_screen("Invalid location\nSend again via BLE");
                        continue;
                    }

                    if (time_sync_once_with_utc_offset(location.utc_offset_seconds, location.utc_offset) != ESP_OK) {
                        lcd_ui_show_message_screen("Time sync failed\nSend location again");
                        continue;
                    }

                    xEventGroupSetBits(s_wifi_call_event_group, WIFI_CALL_EVENT_WEATHER_READY);
                    (void)ble_stop_advertising();
                    continue;
                }

                if (wifi_call_fetch_and_show_weather() != ESP_OK) {
                    lcd_ui_show_message_screen("Periodic refresh failed\nRetrying flow");
                    break;
                }

                xEventGroupSetBits(s_wifi_call_event_group, WIFI_CALL_EVENT_WEATHER_READY);
            }
        } else {
            lcd_ui_show_message_screen("Startup flow failed\nRetrying...");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t wifi_call_start(void) {
    BaseType_t task_result;

    if (s_wifi_call_task_started) {
        return ESP_OK;
    }

    if (s_wifi_call_event_group == NULL) {
        s_wifi_call_event_group = xEventGroupCreate();
        if (s_wifi_call_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    task_result = xTaskCreatePinnedToCore(wifi_call_task,
                                          "wifi_call_task",
                                          8192,
                                          NULL,
                                          5,
                                          NULL,
                                          1);
    if (task_result != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_wifi_call_task_started = true;
    return ESP_OK;
}
