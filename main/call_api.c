#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "call_api.h"

static const char *TAG = "call_api";

typedef struct {
	char *buffer;
	size_t capacity;
	size_t len;
} http_response_buffer_t;

static bool is_unreserved_url_char(char c){
	return isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_' || c == '~';
}

static esp_err_t url_encode(const char *src, char *dst, size_t dst_size){
	size_t out = 0;
	for (size_t i = 0; src[i] != '\0'; ++i) {
		if (is_unreserved_url_char(src[i])) {
			if (out + 1 >= dst_size) {
				return ESP_ERR_INVALID_SIZE;
			}
			dst[out++] = src[i];
		} else {
			if (out + 3 >= dst_size) {
				return ESP_ERR_INVALID_SIZE;
			}
			snprintf(&dst[out], dst_size - out, "%%%02X", (unsigned char)src[i]);
			out += 3;
		}
	}

	if (out >= dst_size) {
		return ESP_ERR_INVALID_SIZE;
	}
	dst[out] = '\0';
	return ESP_OK;
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt){
	if (evt->event_id != HTTP_EVENT_ON_DATA) {
		return ESP_OK;
	}

	http_response_buffer_t *resp = (http_response_buffer_t *)evt->user_data;
	if (resp == NULL || resp->buffer == NULL || resp->capacity == 0) {
		return ESP_OK;
	}

	size_t copy_len = (size_t)evt->data_len;
	size_t available = (resp->capacity - 1) - resp->len;
	if (copy_len > available) {
		copy_len = available;
	}

	if (copy_len > 0) {
		memcpy(resp->buffer + resp->len, evt->data, copy_len);
		resp->len += copy_len;
		resp->buffer[resp->len] = '\0';
	}

	return ESP_OK;
}

esp_err_t call_ip_api_with_ipv6(const char *ipv6){
	if (ipv6 == NULL || ipv6[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}

	char encoded_ipv6[128] = {0};
	ESP_RETURN_ON_ERROR(url_encode(ipv6, encoded_ipv6, sizeof(encoded_ipv6)), TAG,
						"Failed to URL-encode IPv6");

	char url[320] = {0};
	int written = snprintf(url, sizeof(url),
						   "http://ip-api.com/json/?fields=status,message,query,country,regionName,city,isp&query=%s",
						   encoded_ipv6);
	if (written < 0 || (size_t)written >= sizeof(url)) {
		return ESP_ERR_INVALID_SIZE;
	}

	char response[768] = {0};
	http_response_buffer_t response_buf = {
		.buffer = response,
		.capacity = sizeof(response),
		.len = 0,
	};

	esp_http_client_config_t config = {
		.url = url,
		.method = HTTP_METHOD_GET,
		.timeout_ms = 8000,
		.event_handler = http_event_handler,
		.user_data = &response_buf,
	};

	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (client == NULL) {
		return ESP_FAIL;
	}

	esp_err_t err = esp_http_client_perform(client);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
		esp_http_client_cleanup(client);
		return err;
	}

	int status = esp_http_client_get_status_code(client);
	ESP_LOGI(TAG, "ip-api status=%d", status);
	ESP_LOGI(TAG, "ip-api response=%s", response_buf.buffer);

	esp_http_client_cleanup(client);
	if (status >= 200 && status < 300) {
		return ESP_OK;
	}
	return ESP_FAIL;
}
