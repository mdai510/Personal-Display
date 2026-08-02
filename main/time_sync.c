#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_netif_sntp.h"
#include "time_sync.h"

static const char *TAG = "time_sync";

/*
 * Build a POSIX timezone string from a UTC offset.
 */
static esp_err_t build_posix_tz_from_utc_offset(int32_t utc_offset_seconds, char *out, size_t out_len)
{
    if (out == NULL || out_len < 16) {
        return ESP_ERR_INVALID_ARG;
    }

    // POSIX TZ offset has inverted sign compared to UTC offset.
    int32_t posix_total = -utc_offset_seconds;
    int sign = (posix_total >= 0) ? 1 : -1;
    int32_t abs_total = (posix_total >= 0) ? posix_total : -posix_total;

    int32_t hours = abs_total / 3600;
    int32_t minutes = (abs_total % 3600) / 60;
    int32_t seconds = abs_total % 60;

    char sign_char = (sign >= 0) ? '+' : '-';
    int written = 0;
    if (seconds != 0) {
        written = snprintf(out, out_len, "UTC%c%ld:%02ld:%02ld", sign_char,
                           (long)hours, (long)minutes, (long)seconds);
    } else if (minutes != 0) {
        written = snprintf(out, out_len, "UTC%c%ld:%02ld", sign_char,
                           (long)hours, (long)minutes);
    } else {
        written = snprintf(out, out_len, "UTC%c%ld", sign_char, (long)hours);
    }

    if (written < 0 || (size_t)written >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

/*
 * Pick an NTP server based on the timezone hint.
 */
static const char *pick_ntp_server_from_timezone(const char *timezone_hint){
    if (timezone_hint == NULL || timezone_hint[0] == '\0'){
        return "pool.ntp.org";
    }

    if (strncmp(timezone_hint, "America/", 8) == 0){
        return "north-america.pool.ntp.org";
    }
    if (strncmp(timezone_hint, "Europe/", 7) == 0){
        return "europe.pool.ntp.org";
    }
    if (strncmp(timezone_hint, "Asia/", 5) == 0){
        return "asia.pool.ntp.org";
    }
    if (strncmp(timezone_hint, "Africa/", 7) == 0){
        return "africa.pool.ntp.org";
    }
    if (strncmp(timezone_hint, "Australia/", 10) == 0 || strncmp(timezone_hint, "Pacific/", 8) == 0){
        return "oceania.pool.ntp.org";
    }

    return "pool.ntp.org";
}

/*
 * Perform a one-time time synchronization using the given UTC offset and timezone hint.
 */
esp_err_t time_sync_once_with_utc_offset(int32_t utc_offset_seconds, const char *timezone_hint){
    char posix_tz[32] = {0};
    ESP_RETURN_ON_ERROR(build_posix_tz_from_utc_offset(utc_offset_seconds, posix_tz, sizeof(posix_tz)), TAG,
                        "Failed to build POSIX timezone from UTC offset");

    setenv("TZ", posix_tz, 1);
    tzset();

    const char *server = pick_ntp_server_from_timezone(timezone_hint);
    ESP_LOGI(TAG, "Starting SNTP sync (utc_offset=%ld, posix_tz=%s, server=%s)",
             (long)utc_offset_seconds, posix_tz, server);

    esp_netif_sntp_deinit();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    config.wait_for_sync = true;
    ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&config), TAG, "SNTP init failed");

    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP sync failed: %s", esp_err_to_name(err));
        esp_netif_sntp_deinit();
        return err;
    }

    time_t now = 0;
    struct tm local_tm = {0};
    time(&now);
    localtime_r(&now, &local_tm);

    char time_buf[64] = {0};
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S %Z", &local_tm);
    ESP_LOGI(TAG, "SNTP sync complete: %s", time_buf);

    // Startup requirement is one-shot sync, so stop SNTP afterward.
    esp_netif_sntp_deinit();
    return ESP_OK;
}
