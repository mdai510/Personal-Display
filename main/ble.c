#include "ble.h"
#include "nimble/ble.h"
#include "esp_err.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_att.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ble";

void ble_store_config_init(void);

static bool is_ble_initialized = false;
static bool is_ble_synced = false;
static uint8_t ble_addr_type = BLE_OWN_ADDR_RANDOM;

static uint8_t addr_val[6] = {0};

static bool advertising_requested = false;
static bool is_ble_connected = false;
static uint16_t active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static char device_name[32] = "PersDisplay";
static bool s_allow_wifi_write = false;
static bool s_allow_location_write = false;

#define PROV_MAX_PAYLOAD_LEN 384
static uint8_t provisioning_payload[PROV_MAX_PAYLOAD_LEN + 1] = {0};
static uint16_t provisioning_payload_len = 0;
static uint32_t s_location_info_version = 0;
static ble_provisioning_data_t provisioning_data = {
    .wifi_info_obtained = false,
    .location_info_obtained = false,
    .data_obtained = false,
    .ssid = "",
    .password = "",
    .latitude = 0.0,
    .longitude = 0.0,
    .utc_offset = "",
    .utc_offset_seconds = 0,
};

enum {
    PROV_CHR_WIFI = 1,
    PROV_CHR_LOCATION = 2,
};

static uint16_t prov_wifi_chr_val_handle = 0;
static uint16_t prov_location_chr_val_handle = 0;

static int provisioning_chr_access_cb(uint16_t conn_handle,
                                      uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt,
                                      void *arg);

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0xFFF0),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = BLE_UUID16_DECLARE(0xFFF1),
                .access_cb = provisioning_chr_access_cb,
                .arg = (void *)PROV_CHR_WIFI,
                .val_handle = &prov_wifi_chr_val_handle,
                .flags = BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = BLE_UUID16_DECLARE(0xFFF2),
                .access_cb = provisioning_chr_access_cb,
                .arg = (void *)PROV_CHR_LOCATION,
                .val_handle = &prov_location_chr_val_handle,
                .flags = BLE_GATT_CHR_F_WRITE |
                         BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {0},
        },
    },
    {0},
};

static esp_err_t ble_start_advertising_internal(void);

static const char *find_json_key(const char *json, const char *key){
    char pattern[64];
    int written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (written < 0 || (size_t)written >= sizeof(pattern)) {
        return NULL;
    }

    return strstr(json, pattern);
}

static bool copy_json_string_value(const char *json,
                                   const char *key,
                                   char *out,
                                   size_t out_len){
    const char *key_pos = find_json_key(json, key);
    const char *colon_pos;
    const char *start_pos;
    const char *end_pos;
    size_t value_len;

    if (key_pos == NULL) {
        return false;
    }

    colon_pos = strchr(key_pos, ':');
    if (colon_pos == NULL) {
        return false;
    }

    start_pos = colon_pos + 1;
    while (*start_pos == ' ' || *start_pos == '\t' || *start_pos == '\r' || *start_pos == '\n') {
        start_pos++;
    }

    if (*start_pos != '"') {
        return false;
    }

    start_pos++;
    end_pos = strchr(start_pos, '"');
    if (end_pos == NULL) {
        return false;
    }

    value_len = (size_t)(end_pos - start_pos);
    if (value_len + 1 > out_len) {
        return false;
    }

    memcpy(out, start_pos, value_len);
    out[value_len] = '\0';
    return true;
}

static bool copy_json_double_value(const char *json,
                                  const char *key,
                                  double *out_value){
    const char *key_pos = find_json_key(json, key);
    const char *colon_pos;
    char value_buf[32];
    char *end_ptr = NULL;
    size_t value_len;

    if (key_pos == NULL) {
        return false;
    }

    colon_pos = strchr(key_pos, ':');
    if (colon_pos == NULL) {
        return false;
    }

    colon_pos++;
    while (*colon_pos == ' ' || *colon_pos == '\t' || *colon_pos == '\r' || *colon_pos == '\n') {
        colon_pos++;
    }

    value_len = strcspn(colon_pos, ",}\r\n\t ");
    if (value_len == 0 || value_len >= sizeof(value_buf)) {
        return false;
    }

    memcpy(value_buf, colon_pos, value_len);
    value_buf[value_len] = '\0';

    *out_value = strtod(value_buf, &end_ptr);
    return end_ptr != value_buf && *end_ptr == '\0';
}

static bool parse_utc_offset_seconds(const char *offset_str, int32_t *out_seconds){
    int sign = 1;
    int hh = 0;
    int mm = 0;

    if (offset_str == NULL || out_seconds == NULL) {
        return false;
    }

    if (offset_str[0] == '+') {
        sign = 1;
    } else if (offset_str[0] == '-') {
        sign = -1;
    } else {
        return false;
    }

    if (strlen(offset_str) == 6 && offset_str[3] == ':') {
        if (sscanf(offset_str + 1, "%2d:%2d", &hh, &mm) != 2) {
            return false;
        }
    } else if (strlen(offset_str) == 5) {
        if (sscanf(offset_str + 1, "%2d%2d", &hh, &mm) != 2) {
            return false;
        }
    } else {
        return false;
    }

    if (hh < 0 || hh > 14 || mm < 0 || mm > 59) {
        return false;
    }

    if (hh == 14 && mm != 0) {
        return false;
    }

    *out_seconds = sign * ((hh * 3600) + (mm * 60));
    return true;
}

static bool parse_wifi_payload(const uint8_t *payload,
                               size_t payload_len,
                               char *out_ssid,
                               size_t out_ssid_len,
                               char *out_password,
                               size_t out_password_len){
    char json[PROV_MAX_PAYLOAD_LEN + 1];

    if (payload == NULL || out_ssid == NULL || out_password == NULL ||
        payload_len == 0 || payload_len > PROV_MAX_PAYLOAD_LEN) {
        return false;
    }

    memcpy(json, payload, payload_len);
    json[payload_len] = '\0';

    if (!copy_json_string_value(json, "ssid", out_ssid, out_ssid_len)) {
        return false;
    }

    if (!copy_json_string_value(json, "password", out_password, out_password_len)) {
        return false;
    }

    return true;
}

static bool parse_location_payload(const uint8_t *payload,
                                   size_t payload_len,
                                   double *out_lat,
                                   double *out_lon,
                                   char *out_utc_offset,
                                   size_t out_utc_offset_len,
                                   int32_t *out_utc_offset_seconds){
    char json[PROV_MAX_PAYLOAD_LEN + 1];

    if (payload == NULL || out_lat == NULL || out_lon == NULL || out_utc_offset == NULL ||
        out_utc_offset_seconds == NULL || payload_len == 0 || payload_len > PROV_MAX_PAYLOAD_LEN) {
        return false;
    }

    memcpy(json, payload, payload_len);
    json[payload_len] = '\0';

    if (!copy_json_double_value(json, "lat", out_lat)) {
        return false;
    }

    if (!copy_json_double_value(json, "lon", out_lon)) {
        return false;
    }

    if (!copy_json_string_value(json, "utc_offset", out_utc_offset, out_utc_offset_len)) {
        return false;
    }

    return parse_utc_offset_seconds(out_utc_offset, out_utc_offset_seconds);
}

static bool payload_looks_like_location_json(const char *json){
    if (json == NULL) {
        return false;
    }

    return strstr(json, "\"lat\"") != NULL &&
           strstr(json, "\"lon\"") != NULL;
}

static bool payload_looks_like_wifi_json(const char *json){
    if (json == NULL) {
        return false;
    }

    return strstr(json, "\"ssid\"") != NULL &&
           strstr(json, "\"password\"") != NULL;
}

/*
 * Save WiFi information to NVS.
 * Assumes that provisioning_data.ssid and provisioning_data.password are already populated.
 */
static esp_err_t nvs_set_wifi(){
    esp_err_t err;
    nvs_handle_t nvs_handle;

    err = nvs_open("wifi_info", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open Wi-Fi NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, "ssid", provisioning_data.ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set SSID in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_str(nvs_handle, "password", provisioning_data.password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set password in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit Wi-Fi NVS changes: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    nvs_close(nvs_handle);
    return err;
}

static esp_err_t nvs_set_location(){
    esp_err_t err;
    nvs_handle_t nvs_handle;

    err = nvs_open("location_info", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open location NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(nvs_handle, "latitude", &provisioning_data.latitude, sizeof(provisioning_data.latitude));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set latitude in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_blob(nvs_handle, "longitude", &provisioning_data.longitude, sizeof(provisioning_data.longitude));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set longitude in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_set_i32(nvs_handle, "utc_off_s", provisioning_data.utc_offset_seconds);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set UTC offset seconds in NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit location NVS changes: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    nvs_close(nvs_handle);
    return err;
}

static int provisioning_chr_access_cb(uint16_t conn_handle,
                                      uint16_t attr_handle,
                                      struct ble_gatt_access_ctxt *ctxt,
                                      void *arg){
    struct ble_gap_conn_desc desc;
    const uintptr_t chr = (uintptr_t)arg;
    uint16_t pkt_len = 0;

    if (ctxt->om != NULL) {
        pkt_len = OS_MBUF_PKTLEN(ctxt->om);
    }

    if (ble_gap_conn_find(conn_handle, &desc) == 0) {
        ESP_LOGI(TAG,
                 "GATT access: op=%d chr=%lu attr=%u len=%u enc=%d auth=%d bonded=%d",
                 (int)ctxt->op,
                 (unsigned long)chr,
                 attr_handle,
                 (unsigned)pkt_len,
                 desc.sec_state.encrypted,
                 desc.sec_state.authenticated,
                 desc.sec_state.bonded);
    } else {
        ESP_LOGI(TAG,
                 "GATT access: op=%d chr=%lu attr=%u len=%u",
                 (int)ctxt->op,
                 (unsigned long)chr,
                 attr_handle,
                 (unsigned)pkt_len);
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR &&
        (chr == PROV_CHR_WIFI || chr == PROV_CHR_LOCATION)) {
        uint16_t payload_len = pkt_len;
        uint16_t out_len = 0;
        uint16_t att_mtu = ble_att_mtu(conn_handle);
        int rc;

        if (ctxt->om == NULL) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        if (ble_gap_conn_find(conn_handle, &desc) == 0 && !desc.sec_state.encrypted) {
            int sec_rc = ble_gap_security_initiate(conn_handle);
            if (sec_rc == 0 || sec_rc == BLE_HS_EALREADY) {
                ESP_LOGI(TAG, "Write requires encryption; security initiated");
            } else {
                ESP_LOGW(TAG, "Security initiate on write failed: rc=%d", sec_rc);
            }
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }

        if (payload_len == 0 || payload_len > PROV_MAX_PAYLOAD_LEN) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        if ((chr == PROV_CHR_WIFI && !s_allow_wifi_write) ||
            (chr == PROV_CHR_LOCATION && !s_allow_location_write)) {
            ESP_LOGW(TAG,
                     "Rejected provisioning write for chr=%lu (wifi_allowed=%d, location_allowed=%d)",
                     (unsigned long)chr,
                     s_allow_wifi_write,
                     s_allow_location_write);
            return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        }

        ESP_LOGI(TAG,
                 "Provisioning write received: len=%u att_mtu=%u",
                 (unsigned)payload_len,
                 (unsigned)att_mtu);

        rc = ble_hs_mbuf_to_flat(ctxt->om,
                                 provisioning_payload,
                                 sizeof(provisioning_payload) - 1,
                                 &out_len);
        if (rc != 0) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        provisioning_payload[out_len] = '\0';
        provisioning_payload_len = out_len;

        if (chr == PROV_CHR_WIFI) {
            esp_err_t nvs_err;

            if (!parse_wifi_payload(provisioning_payload,
                                    provisioning_payload_len,
                                    provisioning_data.ssid,
                                    sizeof(provisioning_data.ssid),
                                    provisioning_data.password,
                                    sizeof(provisioning_data.password))) {
                if (payload_looks_like_location_json((const char *)provisioning_payload)) {
                    ESP_LOGW(TAG, "Location payload was written to FFF1. Send location JSON to FFF2.");
                }
                ESP_LOGW(TAG, "Failed to parse Wi-Fi provisioning payload");
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            nvs_err = nvs_set_wifi();
            if (nvs_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to persist Wi-Fi provisioning to NVS: %s", esp_err_to_name(nvs_err));
                return BLE_ATT_ERR_UNLIKELY;
            }

            provisioning_data.wifi_info_obtained = true;
            ESP_LOGI(TAG, "Wi-Fi provisioning updated (ssid=%s)", provisioning_data.ssid);
        } else {
            esp_err_t nvs_err;

            if (!parse_location_payload(provisioning_payload,
                                        provisioning_payload_len,
                                        &provisioning_data.latitude,
                                        &provisioning_data.longitude,
                                        provisioning_data.utc_offset,
                                        sizeof(provisioning_data.utc_offset),
                                        &provisioning_data.utc_offset_seconds)) {
                if (payload_looks_like_wifi_json((const char *)provisioning_payload)) {
                    ESP_LOGW(TAG, "Wi-Fi payload was written to FFF2. Send Wi-Fi JSON to FFF1.");
                }
                ESP_LOGW(TAG, "Failed to parse location provisioning payload");
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            nvs_err = nvs_set_location();
            if (nvs_err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to persist location provisioning to NVS: %s", esp_err_to_name(nvs_err));
                return BLE_ATT_ERR_UNLIKELY;
            }

            provisioning_data.location_info_obtained = true;
            s_location_info_version++;
            ESP_LOGI(TAG,
                     "Location provisioning updated: lat=%.6f lon=%.6f utc_offset=%s",
                     provisioning_data.latitude,
                     provisioning_data.longitude,
                     provisioning_data.utc_offset);
        }

        provisioning_data.data_obtained =
            provisioning_data.wifi_info_obtained && provisioning_data.location_info_obtained;

        ESP_LOGI(TAG,
                 "Provisioning state: wifi=%d location=%d complete=%d",
                 provisioning_data.wifi_info_obtained,
                 provisioning_data.location_info_obtained,
                 provisioning_data.data_obtained);

        return 0;
    }

    ESP_LOGW(TAG,
             "Unhandled GATT access: op=%d chr=%lu attr=%u",
             (int)ctxt->op,
             (unsigned long)chr,
             attr_handle);

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }

    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
}

static void ble_update_device_name_from_addr(void){
    int rc = snprintf(device_name,
                      sizeof(device_name),
                      "PersDisplay_%02X%02X",
                      addr_val[1],
                      addr_val[0]);
    if (rc < 0 || (size_t)rc >= sizeof(device_name)) {
        ESP_LOGE(TAG, "Failed to format BLE device name");
        return;
    }

    rc = ble_svc_gap_device_name_set(device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set BLE device name: rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE device name set to: %s", device_name);
}

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg){
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            is_ble_connected = true;
            active_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE connected; conn_handle=%d", event->connect.conn_handle);
        } else {
            is_ble_connected = false;
            active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            ESP_LOGW(TAG, "BLE connect failed; status=%d", event->connect.status);
            if (advertising_requested) {
                (void)ble_start_advertising_internal();
            }
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            struct ble_gap_conn_desc desc;
            int rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
            if (rc == 0) {
                ESP_LOGI(TAG,
                         "BLE encryption changed: encrypted=%d, authenticated=%d, bonded=%d",
                         desc.sec_state.encrypted,
                         desc.sec_state.authenticated,
                         desc.sec_state.bonded);
            } else {
                ESP_LOGI(TAG, "BLE encryption changed (desc unavailable)");
            }
        } else {
            ESP_LOGW(TAG, "BLE encryption change failed; status=%d", event->enc_change.status);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG,
                 "BLE MTU updated; conn_handle=%d channel_id=%d mtu=%d",
                 event->mtu.conn_handle,
                 event->mtu.channel_id,
                 event->mtu.value);
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        ESP_LOGI(TAG,
                 "BLE passkey action event; conn_handle=%d action=%d",
                 event->passkey.conn_handle,
                 event->passkey.params.action);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING:
    {
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);

        ESP_LOGW(TAG,
                 "BLE repeat pairing event; conn_handle=%d",
                 event->repeat_pairing.conn_handle);

        if (rc == 0) {
            int del_rc = ble_store_util_delete_peer(&desc.peer_id_addr);
            if (del_rc != 0) {
                ESP_LOGW(TAG, "Failed to delete old bond: rc=%d", del_rc);
            } else {
                ESP_LOGI(TAG, "Deleted old peer bond; retrying pairing");
            }
        } else {
            ESP_LOGW(TAG, "Failed to get conn desc for repeat pairing: rc=%d", rc);
        }

        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        is_ble_connected = false;
        active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "BLE disconnected; reason=%d", event->disconnect.reason);
        if (advertising_requested) {
            (void)ble_start_advertising_internal();
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "BLE advertising complete");
        if (advertising_requested) {
            (void)ble_start_advertising_internal();
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "BLE subscribe event; conn_handle=%d, attr_handle=%d, cur_notify=%d",
                 event->subscribe.conn_handle,
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        return 0;

    default:
        return 0;
    }
}

static void ble_host_task(void *param){
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/*
* BLE host stack synchronization callback. 
* This function is called when the BLE host stack has completed its \
* initialization and is ready to start advertising or scanning.
*/
static void ble_on_sync(void){
    int rc;

    rc = ble_hs_id_infer_auto(0, &ble_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer BLE address type: %d", rc);
        is_ble_synced = false;
        return;
    }

    rc = ble_hs_id_copy_addr(ble_addr_type, addr_val, NULL);
    if (rc == 0){
        ESP_LOGI(TAG, "BLE address set to: %02X:%02X:%02X:%02X:%02X:%02X",
                 addr_val[5], addr_val[4], addr_val[3],
                 addr_val[2], addr_val[1], addr_val[0]);
        ble_update_device_name_from_addr();
    } else {
        ESP_LOGE(TAG, "Failed to copy BLE address: %d", rc);
    }

    is_ble_synced = true;

    if (advertising_requested) {
        (void)ble_start_advertising_internal();
    }
}

/*
 * BLE host stack reset callback.
 * BLE host will go through internal recovery path after reset
 */
static void ble_on_reset(int reason){
    is_ble_synced = false;
    is_ble_connected = false;
    active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGE(TAG, "BLE host stack reset, reason: %d", reason);
}

bool ble_is_ready(void){
    return is_ble_initialized && is_ble_synced;
}

esp_err_t ble_get_identity_address(uint8_t out_addr[6], uint8_t *out_addr_type){
    if (out_addr == NULL || out_addr_type == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(out_addr, addr_val, sizeof(addr_val));
    *out_addr_type = ble_addr_type;
    return ESP_OK;
}

esp_err_t ble_get_identity_address_string(char *out_str, size_t out_len){
    if (out_str == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (out_len < 18) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ble_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    int written = snprintf(out_str, out_len,
                           "%02X:%02X:%02X:%02X:%02X:%02X",
                           addr_val[5], addr_val[4], addr_val[3],
                           addr_val[2], addr_val[1], addr_val[0]);
    if (written < 0 || (size_t)written >= out_len) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ble_get_device_name(char *out_name, size_t out_len){
    size_t name_len;

    if (out_name == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    name_len = strlen(device_name);
    if (name_len + 1 > out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out_name, device_name, name_len + 1);
    return ESP_OK;
}

esp_err_t ble_get_provisioning_data(ble_provisioning_data_t *out_data){
    if (out_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!provisioning_data.data_obtained) {
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(out_data, &provisioning_data, sizeof(*out_data));
    return ESP_OK;
}

bool ble_has_wifi_info(void){
    return provisioning_data.wifi_info_obtained;
}

bool ble_has_location_info(void){
    return provisioning_data.location_info_obtained;
}

esp_err_t ble_get_wifi_info(char *out_ssid,
                            size_t out_ssid_len,
                            char *out_password,
                            size_t out_password_len){
    if (out_ssid == NULL || out_password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (out_ssid_len == 0 || out_password_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!provisioning_data.wifi_info_obtained) {
        return ESP_ERR_NOT_FOUND;
    }

    if (strlen(provisioning_data.ssid) + 1 > out_ssid_len ||
        strlen(provisioning_data.password) + 1 > out_password_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(out_ssid, provisioning_data.ssid, strlen(provisioning_data.ssid) + 1);
    memcpy(out_password, provisioning_data.password, strlen(provisioning_data.password) + 1);
    return ESP_OK;
}

esp_err_t ble_get_location_info(double *out_latitude,
                                double *out_longitude,
                                char *out_utc_offset,
                                size_t out_utc_offset_len,
                                int32_t *out_utc_offset_seconds){
    if (out_latitude == NULL || out_longitude == NULL || out_utc_offset == NULL ||
        out_utc_offset_seconds == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (out_utc_offset_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!provisioning_data.location_info_obtained) {
        return ESP_ERR_NOT_FOUND;
    }

    if (strlen(provisioning_data.utc_offset) + 1 > out_utc_offset_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_latitude = provisioning_data.latitude;
    *out_longitude = provisioning_data.longitude;
    *out_utc_offset_seconds = provisioning_data.utc_offset_seconds;
    memcpy(out_utc_offset, provisioning_data.utc_offset, strlen(provisioning_data.utc_offset) + 1);
    return ESP_OK;
}

uint32_t ble_get_location_info_version(void){
    return s_location_info_version;
}

esp_err_t ble_set_write_permissions(bool allow_wifi_write, bool allow_location_write){
    s_allow_wifi_write = allow_wifi_write;
    s_allow_location_write = allow_location_write;
    ESP_LOGI(TAG,
             "BLE write permissions updated: wifi=%d location=%d",
             s_allow_wifi_write,
             s_allow_location_write);
    return ESP_OK;
}

esp_err_t ble_init(void){
    //check if ble already initialized
    if(is_ble_initialized){
        ESP_LOGW(TAG, "BLE already initialized");
        return ESP_OK;
    }

    // Initialize Non-Volatile Storage (NVS)
    esp_err_t ret = nvs_flash_init();
    int rc = 0;
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize the NimBLE host stack
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NimBLE host stack: %s", esp_err_to_name(ret));
        return ret;
    }

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1; // Enable Secure Connections
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    rc = ble_att_set_preferred_mtu(185);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to set preferred ATT MTU: rc=%d", rc);
    }

    ble_store_config_init();
    
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "Provisioning handles: FFF1(Wi-Fi)=%u FFF2(Location)=%u",
             prov_wifi_chr_val_handle,
             prov_location_chr_val_handle);

    if (prov_wifi_chr_val_handle == 0 || prov_location_chr_val_handle == 0) {
        ESP_LOGE(TAG, "Provisioning characteristic registration incomplete (FFF1=%u FFF2=%u)",
                 prov_wifi_chr_val_handle,
                 prov_location_chr_val_handle);
    }

    rc = ble_svc_gap_device_name_set(device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to set device name: rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    nimble_port_freertos_init(ble_host_task);

    is_ble_initialized = true;
    return ESP_OK;
}

esp_err_t ble_deinit(void){
    if (!is_ble_initialized) {
        ESP_LOGW(TAG, "BLE not initialized");
        return ESP_OK;
    }

    nimble_port_stop();
    nimble_port_deinit();

    is_ble_initialized = false;
    is_ble_synced = false;
    is_ble_connected = false;
    active_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    return ESP_OK;
}

static esp_err_t ble_start_advertising_internal(void){
    int rc;
    struct ble_hs_adv_fields adv_fields;
    struct ble_gap_adv_params adv_params;

    if (!ble_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    if (is_ble_connected) {
        return ESP_OK;
    }

    if (ble_gap_adv_active()) {
        return ESP_OK;
    }

    memset(&adv_fields, 0, sizeof(adv_fields));
    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: rc=%d", rc);
        return ESP_FAIL;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(ble_addr_type,
                           NULL,
                           BLE_HS_FOREVER,
                           &adv_params,
                           ble_gap_event_cb,
                           NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: rc=%d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "BLE advertising started");
    return ESP_OK;
}

esp_err_t ble_start_advertising(){
    advertising_requested = true;

    if (!ble_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    return ble_start_advertising_internal();
}

esp_err_t ble_stop_advertising(void){
    int rc;

    advertising_requested = false;

    if (!is_ble_initialized) {
        return ESP_OK;
    }

    if (is_ble_connected && active_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        rc = ble_gap_terminate(active_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (rc != 0 && rc != BLE_HS_ENOTCONN && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "ble_gap_terminate failed: rc=%d", rc);
            return ESP_FAIL;
        }

        if (rc == 0) {
            ESP_LOGI(TAG, "BLE disconnect requested for conn_handle=%d", active_conn_handle);
        }
    }

    if (ble_gap_adv_active()) {
        rc = ble_gap_adv_stop();
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gap_adv_stop failed: rc=%d", rc);
            return ESP_FAIL;
        }

        ESP_LOGI(TAG, "BLE advertising stopped");
    }

    return ESP_OK;
}