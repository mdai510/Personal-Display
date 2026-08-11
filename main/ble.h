
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
esp_err_t ble_get_device_name(char *out_name, size_t out_len);
esp_err_t ble_get_provisioning_data(ble_provisioning_data_t *out_data);
bool ble_has_wifi_info(void);
bool ble_has_location_info(void);
esp_err_t ble_get_wifi_info(char *out_ssid,
							size_t out_ssid_len,
							char *out_password,
							size_t out_password_len);
esp_err_t ble_get_location_info(double *out_latitude,
								double *out_longitude,
								char *out_utc_offset,
								size_t out_utc_offset_len,
								int32_t *out_utc_offset_seconds);
uint32_t ble_get_location_info_version(void);
esp_err_t ble_set_write_permissions(bool allow_wifi_write, bool allow_location_write);

esp_err_t ble_start_advertising(void);
esp_err_t ble_stop_advertising(void);