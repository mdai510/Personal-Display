
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	bool wifi_info_obtained;
	bool location_info_obtained;
	bool data_obtained;
	char ssid[33];
	char password[65];
	double latitude;
	double longitude;
	char utc_offset[8];
	int32_t utc_offset_seconds;
} ble_provisioning_data_t;

esp_err_t ble_init(void);
esp_err_t ble_deinit(void);
bool ble_is_ready(void);
esp_err_t ble_get_identity_address(uint8_t out_addr[6], uint8_t *out_addr_type);
esp_err_t ble_get_identity_address_string(char *out_str, size_t out_len);
esp_err_t ble_get_provisioning_data(ble_provisioning_data_t *out_data);

esp_err_t ble_start_advertising(void);
esp_err_t ble_stop_advertising(void);