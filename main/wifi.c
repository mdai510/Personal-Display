#include <string.h>
#include <stdio.h>
#include "esp_err.h"
#include "esp_event_base.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "wifi.h"

/* The event group allows multiple bits for each event, but we only care about
 * two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group = NULL;

static const char *TAG = "wifi station";

esp_netif_t *sta_netif = NULL;
static esp_event_handler_instance_t ip_event_handler;
static esp_event_handler_instance_t wifi_event_handler;
static bool s_wifi_initialized = false;
static bool s_wifi_shutting_down = false;

static int s_retry_num = 0;

// Keep failure detection fast so onboarding can promptly return to BLE Wi-Fi request.
#define WIFI_CONNECT_RETRY_LIMIT 3

/*
* Event handler for Wi-Fi and IP events.
*/
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data){
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START){
    esp_wifi_connect();
  } 
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED){
  }
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED){
    int retry_limit = DISPLAY_WIFI_MAX_RETRY;
    if (retry_limit > WIFI_CONNECT_RETRY_LIMIT) {
      retry_limit = WIFI_CONNECT_RETRY_LIMIT;
    }

    if (s_wifi_shutting_down) {
      ESP_LOGI(TAG, "Wi-Fi disconnect during shutdown; skipping reconnect");
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    else if (s_retry_num < retry_limit){
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "retry to connect to the AP");
    } 
    else{
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    ESP_LOGI(TAG, "connect to the AP fail");
  } 
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP){
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

/*
 * Initialize the Wi-Fi station.
 */
esp_err_t wifi_station_init(void){
  if (s_wifi_initialized) {
    return ESP_OK;
  }

    if (s_wifi_event_group == NULL) {
      s_wifi_event_group = xEventGroupCreate();
      if (s_wifi_event_group == NULL) {
        return ESP_ERR_NO_MEM;
      }
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "Failed to initialize TCP/IP network stack");
      return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "Failed to create default event loop");
      return ret;
    }

    ret = esp_wifi_set_default_wifi_sta_handlers();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "Failed to set default handlers");
      return ret;
    }

    if (sta_netif == NULL) {
      sta_netif = esp_netif_create_default_wifi_sta();
      if (sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default WiFi STA interface");
        return ESP_FAIL;
      }
    }

    // Wi-Fi stack configuration parameters
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
      ESP_LOGE(TAG, "Failed to initialize Wi-Fi stack");
      return ret;
    }

    ret = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL,
        &wifi_event_handler);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to register Wi-Fi event handler: %s", esp_err_to_name(ret));
      return ret;
    }

    ret = esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &ip_event_handler);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(ret));
      return ret;
    }

    s_retry_num = 0;
    s_wifi_shutting_down = false;
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_wifi_initialized = true;
    return ESP_OK;
}

/*
 * Connect the Wi-Fi station to the specified SSID and password.
 */
esp_err_t wifi_station_connect(const char *ssid, const char *password) {
  size_t ssid_len;
  size_t pass_len;

  if (ssid == NULL || password == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  ssid_len = strnlen(ssid, sizeof(((wifi_config_t *)0)->sta.ssid));
  pass_len = strnlen(password, sizeof(((wifi_config_t *)0)->sta.password));
  if (ssid_len == 0 || ssid_len >= sizeof(((wifi_config_t *)0)->sta.ssid) ||
    pass_len >= sizeof(((wifi_config_t *)0)->sta.password)) {
    return ESP_ERR_INVALID_SIZE;
  }

    wifi_config_t wifi_config = {
        .sta =
            {
                // sets the weakest auth mode that the station will accept
                .threshold.authmode = DISPLAY_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            },
    };

  memcpy(wifi_config.sta.ssid, ssid, ssid_len);
  wifi_config.sta.ssid[ssid_len] = '\0';
  memcpy(wifi_config.sta.password, password, pass_len);
  wifi_config.sta.password[pass_len] = '\0';

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or
    * connection failed for the maximum number of re-tries (WIFI_FAIL_BIT). The
    * bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(TAG, "Connected to Wi-Fi network: %s", wifi_config.sta.ssid);
    return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGE(TAG, "Failed to connect to Wi-Fi network: %s", wifi_config.sta.ssid);
    return ESP_FAIL;
    }
    ESP_LOGE(TAG, "Unexpected Wi-Fi error");
    return ESP_FAIL;
}

/*
 * Disconnect the Wi-Fi station.
 */
esp_err_t wifi_station_disconnect(void){
    return esp_wifi_disconnect();
}

/*
 * Deinitialize the Wi-Fi station.
 */
esp_err_t wifi_station_deinit(void){
  if (!s_wifi_initialized) {
    return ESP_OK;
  }

    s_wifi_shutting_down = true;

    esp_err_t disc_ret = esp_wifi_disconnect();
    if (disc_ret != ESP_OK && disc_ret != ESP_ERR_WIFI_NOT_INIT && disc_ret != ESP_ERR_WIFI_NOT_STARTED) {
      ESP_LOGW(TAG, "Failed to disconnect Wi-Fi: %s", esp_err_to_name(disc_ret));
    }

    esp_err_t ret = esp_wifi_stop();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_INIT && ret != ESP_ERR_WIFI_NOT_STARTED) {
      ESP_LOGW(TAG, "Failed to stop Wi-Fi: %s", esp_err_to_name(ret));
    }

    ret = esp_event_handler_instance_unregister(
        IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_INVALID_ARG) {
      ESP_LOGW(TAG, "Failed to unregister IP event handler: %s", esp_err_to_name(ret));
    }

    ret = esp_event_handler_instance_unregister(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler);
    if (ret != ESP_OK && ret != ESP_ERR_NOT_FOUND && ret != ESP_ERR_INVALID_ARG) {
      ESP_LOGW(TAG, "Failed to unregister Wi-Fi event handler: %s", esp_err_to_name(ret));
    }

    if (sta_netif != NULL) {
      ret = esp_wifi_clear_default_wifi_driver_and_handlers(sta_netif);
      if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to clear default Wi-Fi handlers: %s", esp_err_to_name(ret));
      }
      esp_netif_destroy(sta_netif);
      sta_netif = NULL;
    }

    ret = esp_wifi_deinit();
    if (ret != ESP_OK && ret != ESP_ERR_WIFI_NOT_INIT) {
      ESP_LOGW(TAG, "Failed to deinit Wi-Fi stack: %s", esp_err_to_name(ret));
    }

    if (s_wifi_event_group != NULL) {
      vEventGroupDelete(s_wifi_event_group);
      s_wifi_event_group = NULL;
    }
    s_retry_num = 0;
    s_wifi_shutting_down = false;
    s_wifi_initialized = false;

    return ESP_OK;
}

/*
 * Check if the Wi-Fi station is connected.
 */
bool wifi_station_is_connected(void) {
  if (s_wifi_event_group == NULL) {
    return false;
  }

  EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
  return (bits & WIFI_CONNECTED_BIT) != 0;
}
