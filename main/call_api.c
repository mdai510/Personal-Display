#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "call_api.h"
#include "json_parse.h"

static const char *TAG = "call_api";

#define HTTP_RESPONSE_INITIAL_CAPACITY 2048
#define HTTP_RESPONSE_MAX_CAPACITY     (64 * 1024)

typedef struct {
	char *buffer;
	size_t capacity;
	size_t len;
	bool overflow;
} http_response_buffer_t;

/*
 * Check if a character is an unreserved character in a URL.
 * Unreserved characters are alphanumeric or one of the following: '-', '.', '_', '~'.
 */
static bool is_unreserved_url_char(char c){
	return isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_' || c == '~';
}

/*
 * URL-encode a string.
 */
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

/*
 * Copies response data into provided buffer
 */
static esp_err_t http_event_handler(esp_http_client_event_t *evt){
	if (evt->event_id != HTTP_EVENT_ON_DATA) {
		return ESP_OK;
	}

	http_response_buffer_t *resp = (http_response_buffer_t *)evt->user_data;
	if (resp == NULL || resp->buffer == NULL || resp->capacity == 0) {
		return ESP_OK;
	}

	size_t copy_len = (size_t)evt->data_len;
	size_t required = resp->len + copy_len + 1;
	if (required > resp->capacity) {
		size_t new_capacity = resp->capacity;
		while (new_capacity < required && new_capacity < HTTP_RESPONSE_MAX_CAPACITY) {
			new_capacity *= 2;
		}

		if (new_capacity < required) {
			resp->overflow = true;
			copy_len = (resp->capacity - 1) - resp->len;
		} else {
			char *new_buf = realloc(resp->buffer, new_capacity);
			if (new_buf == NULL) {
				resp->overflow = true;
				copy_len = (resp->capacity - 1) - resp->len;
			} else {
				resp->buffer = new_buf;
				resp->capacity = new_capacity;
			}
		}
	}

	if (copy_len > 0) {
		memcpy(resp->buffer + resp->len, evt->data, copy_len);
		resp->len += copy_len;
		resp->buffer[resp->len] = '\0';
	}

	return ESP_OK;
}

static esp_err_t perform_http_get(const char *url, http_response_buffer_t *response, int *status_code){
	if (url == NULL || response == NULL || status_code == NULL) {
		return ESP_ERR_INVALID_ARG;
	}

	response->buffer = malloc(HTTP_RESPONSE_INITIAL_CAPACITY);
	if (response->buffer == NULL) {
		return ESP_ERR_NO_MEM;
	}
	response->capacity = HTTP_RESPONSE_INITIAL_CAPACITY;
	response->len = 0;
	response->overflow = false;
	response->buffer[0] = '\0';

	esp_http_client_config_t config = {
		.url = url,
		.method = HTTP_METHOD_GET,
		.timeout_ms = 12000,
		.event_handler = http_event_handler,
		.user_data = response,
	};

	esp_http_client_handle_t client = esp_http_client_init(&config);
	if (client == NULL) {
		free(response->buffer);
		response->buffer = NULL;
		return ESP_FAIL;
	}

	esp_err_t err = esp_http_client_perform(client);
	if (err == ESP_OK) {
		*status_code = esp_http_client_get_status_code(client);
	}
	esp_http_client_cleanup(client);

	if (err != ESP_OK) {
		free(response->buffer);
		response->buffer = NULL;
		return err;
	}

	if (response->overflow) {
		ESP_LOGW(TAG, "HTTP response truncated at %u bytes", (unsigned)response->len);
	}

	return ESP_OK;
}

/*
 * Call the ip-api.com API with the provided IPv6 address.
 */
esp_err_t call_ip_api_with_ipv6(const char *ipv6){
	if (ipv6 == NULL || ipv6[0] == '\0') {
		return ESP_ERR_INVALID_ARG;
	}

	char encoded_ipv6[128] = {0};
	ESP_RETURN_ON_ERROR(url_encode(ipv6, encoded_ipv6, sizeof(encoded_ipv6)), TAG,
						"Failed to URL-encode IPv6");

	char url[320] = {0};
	int written = snprintf(url, sizeof(url),
					"http://ip-api.com/json/"
					"?fields=status,message,country,region,regionName,city,"
					"lat,lon,timezone,offset,query&query=%s",
					encoded_ipv6);
	if (written < 0 || (size_t)written >= sizeof(url)) return ESP_ERR_INVALID_SIZE;

	http_response_buffer_t response_buf = {0};
	int status = 0;
	esp_err_t err = perform_http_get(url, &response_buf, &status);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
		return err;
	}
	ESP_LOGI(TAG, "ip-api status=%d", status);
	ESP_LOGI(TAG, "ip-api response=%s", response_buf.buffer ? response_buf.buffer : "");

	process_ip_api_response(response_buf.buffer, response_buf.len);
	ip_api_info_t info;
	if (get_ip_api_info(&info) == ESP_OK) {
		ESP_LOGI(TAG, "Processed ip-api response: country=%s, region=%s, city=%s, lat=%f, lon=%f, timezone=%s, offset=%ld",
					info.country ? info.country : "N/A",
					info.regionName ? info.regionName : "N/A",
					info.city ? info.city : "N/A",
					info.lat,
					info.lon,
					info.timezone ? info.timezone : "N/A",
					(long)info.utc_offset_seconds);
		}

	free(response_buf.buffer);
	if (status >= 200 && status < 300) return ESP_OK;
	return ESP_FAIL;
}

esp_err_t call_weather_api_24h(double latitude, double longitude, const char *timezone)
{
	const char *tz = (timezone && timezone[0]) ? timezone : "UTC";
	char encoded_tz[128] = {0};
	ESP_RETURN_ON_ERROR(url_encode(tz, encoded_tz, sizeof(encoded_tz)), TAG, "Failed to URL-encode timezone");

	char url[768] = {0};
	int written = snprintf(url, sizeof(url),
		"http://api.open-meteo.com/v1/forecast"
		"?latitude=%.6f&longitude=%.6f"
		"&hourly=temperature_2m,precipitation_probability,precipitation,relative_humidity_2m,rain,showers,snowfall,cloud_cover,weather_code,is_day"
		"&current=temperature_2m,relative_humidity_2m,is_day,showers,snowfall,rain,precipitation,weather_code,wind_speed_10m,cloud_cover"
		"&timezone=%s&timeformat=unixtime&wind_speed_unit=mph&temperature_unit=fahrenheit&precipitation_unit=inch"
		"&forecast_hours=24",
		latitude, longitude, encoded_tz);
	if (written < 0 || (size_t)written >= sizeof(url)) {
		return ESP_ERR_INVALID_SIZE;
	}

	http_response_buffer_t response = {0};
	int status = 0;
	esp_err_t err = perform_http_get(url, &response, &status);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Open-Meteo 24h request failed: %s", esp_err_to_name(err));
		return err;
	}

	ESP_LOGI(TAG, "open-meteo 24h status=%d, bytes=%u", status, (unsigned)response.len);
	if (status >= 200 && status < 300) {
		err = process_weather_hourly_response(response.buffer, response.len);
	}

	free(response.buffer);
	return (status >= 200 && status < 300) ? err : ESP_FAIL;
}

esp_err_t call_weather_api_7d(double latitude, double longitude, const char *timezone)
{
	const char *tz = (timezone && timezone[0]) ? timezone : "UTC";
	char encoded_tz[128] = {0};
	ESP_RETURN_ON_ERROR(url_encode(tz, encoded_tz, sizeof(encoded_tz)), TAG, "Failed to URL-encode timezone");

	char url[768] = {0};
	int written = snprintf(url, sizeof(url),
		"http://api.open-meteo.com/v1/forecast"
		"?latitude=%.6f&longitude=%.6f"
		"&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,daylight_duration,rain_sum,snowfall_sum,showers_sum,wind_speed_10m_max"
		"&timezone=%s&timeformat=unixtime&wind_speed_unit=mph&temperature_unit=fahrenheit&precipitation_unit=inch"
		"&forecast_days=7",
		latitude, longitude, encoded_tz);
	if (written < 0 || (size_t)written >= sizeof(url)) {
		return ESP_ERR_INVALID_SIZE;
	}

	http_response_buffer_t response = {0};
	int status = 0;
	esp_err_t err = perform_http_get(url, &response, &status);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "Open-Meteo 7d request failed: %s", esp_err_to_name(err));
		return err;
	}

	ESP_LOGI(TAG, "open-meteo 7d status=%d, bytes=%u", status, (unsigned)response.len);
	if (status >= 200 && status < 300) {
		err = process_weather_daily_response(response.buffer, response.len);
	}

	free(response.buffer);
	return (status >= 200 && status < 300) ? err : ESP_FAIL;
}
