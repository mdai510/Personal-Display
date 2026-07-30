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
#define WIFI_CONNECTED_IPV6_BIT BIT2

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group = NULL;

static const char *TAG = "wifi station";

esp_netif_t *sta_netif = NULL;
static esp_event_handler_instance_t ip_event_handler;
static esp_event_handler_instance_t wifi_event_handler;
static bool s_wifi_initialized = false;

static int s_retry_num = 0;

// Module-private IPv6 state updated from IP events and exposed via getter APIs.
static esp_ip6_addr_t s_ipv6_addr;
static bool s_has_ipv6 = false;

/*
* Convert an esp_ip6_addr_type_t to a human-readable string.
*/
static const char *ipv6_type_to_str(esp_ip6_addr_type_t type){
  switch (type) {
    case ESP_IP6_ADDR_IS_GLOBAL:
      return "global";
    case ESP_IP6_ADDR_IS_LINK_LOCAL:
      return "link_local";
    case ESP_IP6_ADDR_IS_SITE_LOCAL:
      return "site_local";
    case ESP_IP6_ADDR_IS_UNIQUE_LOCAL:
      return "unique_local";
    case ESP_IP6_ADDR_IS_IPV4_MAPPED_IPV6:
      return "ipv4_mapped";
    default:
      return "unknown";
  }
}

/*
* Event handler for Wi-Fi and IP events.
*/
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data){
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START){
    esp_wifi_connect();
  } 
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED){
    if (sta_netif){
      esp_err_t ret = esp_netif_create_ip6_linklocal(sta_netif);
      if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_IP6_ADDR_FAILED) {
        ESP_LOGW(TAG, "Failed to create IPv6 link-local address: %s", esp_err_to_name(ret));
      }
    }
  } 
  else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED){
    if (s_retry_num < DISPLAY_WIFI_MAX_RETRY){
      esp_wifi_connect();
      s_retry_num++;
      ESP_LOGI(TAG, "retry to connect to the AP");
    } 
    else{
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    s_has_ipv6 = false;
    memset(&s_ipv6_addr, 0, sizeof(s_ipv6_addr));
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_IPV6_BIT);
    ESP_LOGI(TAG, "connect to the AP fail");
  } 
  else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP){
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  } 
  else if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6){
    ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
    if (event->esp_netif == sta_netif){
      esp_ip6_addr_t ip6 = event->ip6_info.ip;
      esp_ip6_addr_type_t type = esp_netif_ip6_get_addr_type(&ip6);
      ESP_LOGI(TAG, "got ipv6 (%s):" IPV6STR, ipv6_type_to_str(type), IPV62STR(ip6));

      // Use only routable global IPv6 for external API requests.
      if (type == ESP_IP6_ADDR_IS_GLOBAL){
        s_ipv6_addr = ip6;
        s_has_ipv6 = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_IPV6_BIT);
      }
    }
  }
}

/*
 * Initialize the Wi-Fi station.
 */
esp_err_t wifi_station_init(void){
  if (s_wifi_initialized) {
    return ESP_OK;
  }

  if (strlen(DISPLAY_WIFI_SSID) == 0) {
    ESP_LOGE(TAG, "Wi-Fi SSID is empty. Set it in menuconfig under Wi-Fi "
                  "Station Configuration.");
    return ESP_ERR_INVALID_ARG;
  }

    //Initialize Non-Volatile Storage (NVS)
    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
      return ESP_ERR_NO_MEM;
    }

    ret = esp_netif_init();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to initialize TCP/IP network stack");
      return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to create default event loop");
      return ret;
    }

    ret = esp_wifi_set_default_wifi_sta_handlers();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to set default handlers");
      return ret;
    }

    sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
      ESP_LOGE(TAG, "Failed to create default WiFi STA interface");
      return ESP_FAIL;
    }

    // Wi-Fi stack configuration parameters
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL,
        &wifi_event_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &ip_event_handler));
    s_wifi_initialized = true;
    return ret;
}

/*
 * Connect the Wi-Fi station to the specified SSID and password.
 */
esp_err_t wifi_station_connect(char* ssid, char* password) {
    wifi_config_t wifi_config = {
        .sta =
            {
                .ssid = DISPLAY_WIFI_SSID,
                .password = DISPLAY_WIFI_PASS,
                // sets the weakest auth mode that the station will accept
                .threshold.authmode = DISPLAY_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            },
    };

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

    esp_err_t ret = esp_wifi_stop();
  if(ret == ESP_ERR_WIFI_NOT_INIT){
        ESP_LOGE(TAG, "Wifi stack not initialized, cannot stop");
        return ret;
    }

    ESP_ERROR_CHECK(esp_wifi_deinit());
    ESP_ERROR_CHECK(
        esp_wifi_clear_default_wifi_driver_and_handlers(sta_netif));
    esp_netif_destroy(sta_netif);

    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler));

    if (s_wifi_event_group != NULL) {
      vEventGroupDelete(s_wifi_event_group);
      s_wifi_event_group = NULL;
    }
    sta_netif = NULL;
    s_retry_num = 0;
    s_has_ipv6 = false;
    memset(&s_ipv6_addr, 0, sizeof(s_ipv6_addr));
    s_wifi_initialized = false;

    return ESP_OK;
}

/*
 * Get the current IPv6 address of the Wi-Fi station.
 */
esp_err_t wifi_station_get_ipv6(char *ipv6_addr, size_t addr_len){
  if (ipv6_addr == NULL || addr_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!s_has_ipv6) {
    return ESP_ERR_NOT_FOUND;
  }

  int written = snprintf(ipv6_addr, addr_len, IPV6STR, IPV62STR(s_ipv6_addr));
  if (written < 0 || (size_t)written >= addr_len) {
    return ESP_ERR_INVALID_SIZE;
  }

  return ESP_OK;
}

/*
 * Wait for the Wi-Fi station to obtain an IPv6 address, with a timeout.
 */
esp_err_t wifi_station_wait_for_ipv6(uint32_t timeout_ms) {
  if (s_wifi_event_group == NULL) return ESP_ERR_INVALID_STATE;
  if (s_has_ipv6) return ESP_OK;

  TickType_t wait_ticks = (timeout_ms == portMAX_DELAY)
                              ? portMAX_DELAY
                              : pdMS_TO_TICKS(timeout_ms);
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_IPV6_BIT,
                                         pdFALSE, pdFALSE, wait_ticks);

  return (bits & WIFI_CONNECTED_IPV6_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
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
